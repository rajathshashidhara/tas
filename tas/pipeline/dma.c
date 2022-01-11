#include <stdint.h>
#include <stdlib.h>
#include <rte_config.h>
#include <rte_memcpy.h>
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_ioat_rawdev.h>

#include "pipeline.h"

#define MAX_NB_TX   NUM_FLOWGRPS
#define BATCH_SIZE  32

struct userdata {
  union {
    struct {
      uint64_t addr:48;
      uint64_t id:16;
    } __attribute__((packed));
    uint64_t __raw;
  };
};

extern struct rte_ring *dma_ring;
extern struct rte_ring *arx_ring;
extern struct rte_ring *nbi_tx_queues[MAX_NB_TX];

//#define DMA_IOAT

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

#ifdef DMA_IOAT

static int dev_id;
static unsigned issue_copies(struct dma_cmt_t *cmds[BATCH_SIZE],
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

#else

static unsigned issue_copies(struct dma_cmt_t *cmds[BATCH_SIZE],
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

  /* Perform copies */
  for (i = 0; i < num; i++) {
    if (cmds[i]->len1 > 0) {
      rte_memcpy(cmds[i]->dst_addr1, cmds[i]->src_addr1, cmds[i]->len1);
      num_copy++;
    }

    if (cmds[i]->len0 > 0) {
      rte_memcpy(cmds[i]->dst_addr0, cmds[i]->src_addr0, cmds[i]->len0);
      num_copy++;
    }
  }

  return num_copy;
}

#endif
