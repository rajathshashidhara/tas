#include <stdint.h>
#include <stdlib.h>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_atomic.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_memcpy.h>
#include <rte_prefetch.h>

#include "tas_memif.h"
#include "dma.h"
#include "pipeline.h"
#include "tas.h"

#define BATCH_SIZE      16

static unsigned poll_atx_queue(uint16_t atxq,
    struct appctx_desc_t **desc, unsigned max_num)
{
  unsigned i;
  struct flextcp_pl_appctx *actx = &fp_state->appctx[0][atxq];
  struct flextcp_pl_atx *atx;
  uint8_t type;

  /* stop if context is not in use */
  if (actx->tx_len == 0)
    return 0;

  for (i = 0; i < max_num; i++) {
    atx = dma_pointer(actx->tx_base + actx->tx_head, sizeof(struct flextcp_pl_atx));
    type = atx->type;
    
    MEM_BARRIER();

    if (type == 0)  /*> Not a valid entry */
      break;

    /* Read descriptor */
    rte_memcpy(desc[i], atx, sizeof(struct flextcp_pl_atx));

    MEM_BARRIER();
    atx->type = 0;    /*> Mark entry as free! */

    actx->tx_head += sizeof(struct flextcp_pl_atx);
    if (actx->tx_head >= actx->tx_len)
      actx->tx_head -= actx->tx_len;
  }

  return i;
}

static unsigned poll_arx_queue_free_entries(uint16_t arxq)
{
  struct flextcp_pl_appctx *actx = &fp_state->appctx[0][arxq];
  struct flextcp_pl_arx *parx;
  uint32_t pos, i;

  if (actx->rx_avail > actx->rx_len / 2)
    return 0;

  pos = actx->rx_head + actx->rx_avail;
  if (pos >= actx->rx_len)
    pos -= actx->rx_len;

  i = 0;
  while (actx->rx_avail < actx->rx_len && i < BATCH_SIZE) {
    parx = dma_pointer(actx->rx_base + pos, sizeof(struct flextcp_pl_arx));

    if (parx->type != 0)
      break;

    actx->rx_avail += sizeof(struct flextcp_pl_arx);
    pos += sizeof(struct flextcp_pl_arx);
    if (pos >= actx->rx_len)
      pos -= actx->rx_len;
    i++;

    MEM_BARRIER();
  }

  return i;
}

static unsigned flush_arx_queues(struct actxptr_t *descptr,
    unsigned num, uint64_t tsc)
{
  unsigned i, actx_id;
  uint32_t rxnhead;
  struct flextcp_pl_appctx *actx;
  struct flextcp_pl_arx *arx_entries[BATCH_SIZE];
  struct appctx_desc_t *desc[BATCH_SIZE];

  /* Prefetch the descriptors */
  for (i = 0; i < num; i++) {
    rte_prefetch0(desc[i]);
  }

  /* Allocate entries on CTX queue */
  for (i = 0; i < num; i++) {
    actx_id = descptr[i].db_id;
    desc[i] = BUF_FROM_PTR(descptr[i]);
    actx = &fp_state->appctx[0][actx_id];

    if (actx->rx_avail == 0) {
      fprintf(stderr, "%s: no space in app rx queue\n", __func__);
      abort();
    }

    MEM_BARRIER();

    arx_entries[i] = dma_pointer(actx->rx_base + actx->rx_head, sizeof(struct flextcp_pl_arx));

    rxnhead = actx->rx_head + sizeof(struct flextcp_pl_arx);
    if (rxnhead >= actx->rx_len) {
      rxnhead -= actx->rx_len;
    }
    actx->rx_head = rxnhead;
    actx->rx_avail -= sizeof(struct flextcp_pl_arx);
  }

  /* Prefetch entries */
  for (i = 0; i < num; i++) {
    rte_prefetch0(arx_entries[i]);
  }

  /* Copy entries */
  for (i = 0; i < num; i++) {
    rte_memcpy(arx_entries[i], desc[i], sizeof(struct flextcp_pl_arx));
  }

  /* Notify contexts */
  for (i = 0; i < num; i++) {
    actx_id = descptr[i].db_id;
    actx = &fp_state->appctx[0][actx_id];
    notify_appctx(actx, tsc);
  }

  /* Free descriptors */
  rte_mempool_put_bulk(arx_desc_pool, (void **) desc, num);

  return num;
}

struct appctx_thread_conf {

};

static void appctx_thread_init(struct appctx_thread_conf *conf)
{
  /* TODO: ? */
  (void) conf;
}

static void move_to_front(void **arr, unsigned s, unsigned n)
{
  unsigned i;

  i = 0;
  while (s < n) {
    arr[i++] = arr[s++];
  }
}

int appctx_thread(void *args)
{
  unsigned n, q, q_poll, num_rx, num_alloc, num_tx, ret;
  uint64_t tsc;
  struct actxptr_t arx_descptr[BATCH_SIZE];
  struct actxptr_t atx_descptr[BATCH_SIZE];
  struct appctx_desc_t *atx_desc[BATCH_SIZE];
  struct appctx_thread_conf *conf = (struct appctx_thread_conf *) args;

  appctx_thread_init(conf);

  q_poll = 0;
  num_alloc = 0;
  while (1) {
    tsc = rte_get_tsc_cycles();
    
    /* Handle ARX descriptors */
    num_rx = rte_ring_sc_dequeue_burst(arx_ring, (void **) arx_descptr, BATCH_SIZE, NULL);
    flush_arx_queues(arx_descptr, num_rx, tsc);

    /* Probe ARX free entries */
    for (q = 0; q < FLEXNIC_PL_APPCTX_NUM; q++) {
      poll_arx_queue_free_entries(q);
    }

    /* Allocate ATX descriptors */
    if (num_alloc < BATCH_SIZE) {
      if (rte_mempool_get_bulk(atx_desc_pool, (void **) &atx_desc[num_alloc], BATCH_SIZE - num_alloc) == 0) {
        num_alloc = BATCH_SIZE;
      }
    }

    /* Poll ATX queues */
    num_tx = 0;
    for (q = 0; q < FLEXNIC_PL_APPCTX_NUM && num_tx < num_alloc; q++) {
      ret = poll_atx_queue(q_poll, &atx_desc[num_tx], num_alloc - num_tx);
      for (n = 0; n < ret; n++) {
        atx_descptr[n + num_tx].desc = BUF_TO_PTR(atx_desc[n + num_tx]);
        atx_descptr[n + num_tx].db_id = q_poll;
        atx_descptr[n + num_tx].dir = ACTX_DIR_TX;
        atx_descptr[n + num_tx].seqno = 0;    /*> UNUSED */
      }
      num_tx += ret;
      q_poll = (q_poll + 1) % FLEXNIC_PL_APPCTX_NUM;
    }

    if (num_tx == 0) {
      continue;
    }

    /* Push ATX descriptors for processing */
    ret = rte_ring_sp_enqueue_burst(atx_ring, (void **) atx_descptr, num_tx, NULL);
    num_tx = MIN(ret, num_tx);
    move_to_front((void **) atx_desc, num_tx, num_alloc - num_tx);
    num_alloc -= num_tx;
  }

  return EXIT_SUCCESS;
}