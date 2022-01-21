#include <stdint.h>
#include <stdlib.h>
#include <rte_config.h>
#include <rte_memcpy.h>
#include <rte_ring.h>
#include <rte_mbuf.h>

#include "pipeline.h"

#define BATCH_SIZE  16

// #define DMA_IOAT

#ifdef DMA_IOAT

#include <rte_ioat_rawdev.h>

static unsigned process_completion(uint64_t buf_handles[BATCH_SIZE],
                                   uint64_t desc_handles[BATCH_SIZE],
                                   unsigned num)
{
  unsigned i, k, ret;

  struct appctx_desc_t *arx_desc[BATCH_SIZE];
  struct rte_mbuf *tx_mbufs[MAX_NB_TX][BATCH_SIZE];
  struct rte_mbuf *free_mbufs[BATCH_SIZE];

  unsigned desc_cnt;
  unsigned tx_cnt[MAX_NB_TX];
  unsigned free_cnt;

  uint16_t flow_grp, actx_id;
  struct rte_mbuf *buf;
  struct appctx_desc_t *desc;
  struct userdata data;

  for (i = 0; i < num; i++) {
    data.__raw = buf_handles[i];
    flow_grp = data.id;
    buf = (struct rte_mbuf *) data.addr;

    data.__raw = desc_handles[i];
    actx_id = data.id;
    desc = (struct appctx_desc_t *) data.addr;

    if (buf == NULL)
      continue;

    /*> Free if outsize sequencer limits */
    if (flow_grp >= MAX_NB_TX) {
      free_mbufs[free_cnt++] = buf;
    }
    else {
      tx_mbufs[tx_cnt[flow_grp]++] = buf;
    }

    if (desc == NULL)
      continue;
    
    arx_desc[desc_cnt++] = desc;
  }

  /* Transmit packets */
  for (i = 0; i < MAX_NB_TX; i++) {
    if (tx_cnt[i] > 0) {
      ret = rte_ring_mp_enqueue_burst(nbi_tx_queues[i], tx_mbufs[i], tx_cnt[i], NULL);

      /* Free unsent mbufs */
      if (ret < tx_cnt[i]) {
        for (k = ret; k < tx_cnt[i]; k++) {
          free_mbufs[free_cnt++] = tx_mbufs[i][k];
        }
      }
    }
  }

  /* Forward descriptors */
  if (desc_cnt > 0) {
    ret = rte_ring_mp_enqueue_bulk(arx_ring, arx_desc, desc_cnt, NULL);

    if (ret < desc_cnt) {
      fprintf(stderr, "%s: failed to enqueue desc to arx ring", __func__);
      abort();
    }
  }

  /*> Free packets */
  for (i = 0; i < free_cnt; i++) {
    rte_pktmbuf_free_seg(free_mbufs[i]);   // NOTE: We do not handle chained mbufs for efficiency!
  }

  return num;
}

static int dev_id;
static unsigned issue_copies(struct dma_cmd_t *cmds[BATCH_SIZE],
                                 unsigned num)
{
  int ret;
  unsigned i, num_copy, num_cmpl;
  struct userdata data0, data1;
  uint64_t buf_handles[BATCH_SIZE], desc_handles[BATCH_SIZE];

  num_cmpl = num_copy = 0;

  /* Prefetch DMA commands */
  for (i = 0; i < num; i++) {
    rte_prefetch0(cmds[i]);
  }

  /* Issue CBDMAs */
  for (i = 0; i < num; i++) {
    if (cmds[i]->len1 > 0) {
      ret = rte_ioat_enqueue_copy(dev_id,
              cmds[i]->src_addr1, cmds[i]->dst_addr1, cmds[i]->len1,
              NULL, NULL, 0);
      
      if (ret == 0) {
        fprintf(stderr, "%s: failed to enqueue payload copy to I/OAT rawdev", __func__);
        abort();
      }

      num_copy += ret;
    }

    if (cmds[i]->len0 > 0) {
      data0.addr = (uint64_t) cmds[i]->buf;
      data0.id = cmds[i]->flow_grp;
      data1.addr = (uint64_t) cmds[i]->desc;
      data0.id = cmds[i]->actx_id;

      ret = rte_ioat_enqueue_copy(dev_id,
              cmds[i]->src_addr0, cmds[i]->dst_addr0, cmds[i]->len0,
              data0.__raw, data1.__raw, 0);
      
      if (ret == 0) {
        fprintf(stderr, "%s: failed to enqueue payload copy to I/OAT rawdev", __func__);
        abort();
      }

      num_copy += ret;
    }
    else {
      /*> process DMA completion */
      data0.addr = (uint64_t) cmds[i]->buf;
      data0.id = cmds[i]->flow_grp;
      data1.addr = (uint64_t) cmds[i]->desc;
      data0.id = cmds[i]->actx_id;

      buf_handles[num_cmpl]  = data0.__raw;
      desc_handles[num_cmpl] = data1.__raw;
      num_cmpl++;
    }
  }

  /* Trigger copies */
  rte_ioat_do_copies(dev_id);

  /* Process completion */
  process_completion(buf_handles, desc_handles, num_cmpl);
  return num_copy;
}

#endif

static unsigned issue_copies(struct dma_cmd_t **cmds,
                             unsigned num)
{
  unsigned i, num_copy;

  num_copy = 0;

  /* Perform copies */
  for (i = 0; i < num; i++) {
    if (cmds[i]->len1 > 0) {
      rte_memcpy((void *) cmds[i]->dst_addr1, (void *) cmds[i]->src_addr1, cmds[i]->len1);
      num_copy++;
    }

    if (cmds[i]->len0 > 0) {
      rte_memcpy((void *) cmds[i]->dst_addr0, (void *) cmds[i]->src_addr0, cmds[i]->len0);
      num_copy++;
    }
  }

  return num_copy;
}

static unsigned process_completion(struct dma_cmd_t **cmds,
                                   unsigned num)
{
  unsigned i, m;

  struct nbi_pkt_t tx_pkts[BATCH_SIZE];
  struct actxptr_t arx_descs[BATCH_SIZE];
  unsigned num_desc;

  num_desc = 0;

  for (i = 0; i < num; i++) {
    tx_pkts[i] = cmds[i]->buf;

    if (cmds[i]->desc.__rawptr != 0) {
      arx_descs[num_desc++] = cmds[i]->desc;
    }
  }

  /* Send packets to NBI */
  m = rte_ring_mp_enqueue_burst(nbi_tx_queue, (void **) tx_pkts, num, NULL);
  if (m < num) {
    /* Free untransmitted segments */
    rte_pktmbuf_free_seg((struct rte_mbuf *) BUF_FROM_PTR(tx_pkts[i]));
  }

  /* Send descriptors to ACTX */
  m = rte_ring_mp_enqueue_burst(arx_ring, (void **) arx_descs, num_desc, NULL);
  if (m < num_desc) {
    /* TODO: How to handle this? */
    fprintf(stderr, "%s:%d\n", __func__, __LINE__);
    abort();
  }

  return num;
}

int dma_thread(void *args)
{
  unsigned i, num;
  struct workptr_t wptrs[BATCH_SIZE];
  struct dma_cmd_t *cmds[BATCH_SIZE];

  (void) args;
  dataplane_stats_coreinit(DMA_CORE_ID);

  while (1) {
    num = rte_ring_mc_dequeue_burst(dma_cmd_ring, (void **) wptrs, BATCH_SIZE, NULL);
    dataplane_stats_record(DMA_CORE_ID, num);
    if (num == 0)
      continue;

    /* Prefetch */
    for (i = 0; i < num; i++) {
      cmds[i] = (struct dma_cmd_t *) BUF_FROM_PTR(wptrs[i]);
      rte_prefetch0(cmds[i]);
    }

    issue_copies(cmds, num);
    process_completion(cmds, num);
  }

  return EXIT_SUCCESS;
}