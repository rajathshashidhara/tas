#include <stdint.h>
#include <stdlib.h>
#include <rte_config.h>
#include <rte_atomic.h>
#include <rte_ring.h>
#include <rte_mempool.h>

#include "tas_memif.h"
#include "dma.h"

extern struct rte_mempool *arx_desc_pool;        /*> Pool for RX APPCTX descriptors */
extern struct rte_mempool *atx_desc_pool;        /*> Pool for TX APPCTX descriptors */

extern struct rte_ring *atx_ring;
extern struct rte_ring *arx_ring;

extern void notify_appctx(struct flextcp_pl_appctx *ctx, uint64_t tsc);

#define BATCH_SIZE      16
struct appctx_desc_t {
  union {
    struct flextcp_pl_atx atx;
    struct flextcp_pl_arx arx;
    uint32_t __raw[16];      /*> Cacheline size */
  };
};

static unsigned int poll_atx_queue(uint16_t atxq,
    struct appctx_desc_t *desc[BATCH_SIZE], unsigned int k)
{
  unsigned int i;
  struct flextcp_pl_appctx *actx = &fp_state->appctx[0][atxq];
  struct flextcp_pl_atx *atx;
  uint8_t type;

  /* stop if context is not in use */
  if (actx->tx_len == 0)
    return 0;

  for (i = k; i < BATCH_SIZE; i++) {
    atx = dma_pointer(actx->tx_base + actx->tx_head, sizeof(struct flextcp_pl_atx));
    type = atx->type;
    
    MEM_BARRIER();

    if (type == 0)  /*> Not a valid entry */
      break;

    /* Read descriptor */
    dma_read(atx, sizeof(struct flextcp_pl_atx), desc[i]);

    MEM_BARRIER();
    atx->type = 0;    /*> Mark entry as free! */

    actx->tx_head += sizeof(struct flextcp_pl_atx);
    if (actx->tx_head >= actx->tx_len)
      actx->tx_head -= actx->tx_len;
  }

  return i - k;
}

static unsigned int poll_arx_queue_free_entries(uint16_t arxq)
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

static unsigned int flush_arx_queues(struct appctx_desc_t *desc[BATCH_SIZE],
    unsigned int num, uint64_t tsc)
{
  unsigned int i, actx_id;
  uint32_t rxnhead;
  struct flextcp_pl_appctx *actx;
  struct flextcp_pl_arx *arx_entries[BATCH_SIZE];

  /* Prefetch the descriptors */
  for (i = 0; i < num; i++) {
    rte_prefetch0(desc[i]);
  }

  /* Allocate entries on CTX queue */
  for (i = 0; i < num; i++) {
    actx_id = desc[i]->arx.msg.connupdate.db_id;
    actx = &fp_state->appctx[0][actx_id];

    if (actx->rx_avail == 0) {
      fprintf(stderr, "%s: no space in app rx queue\n");
      abort();
    }

    rte_compiler_barrier();

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
    *arx_entries[i] = desc[i];
  }

  /* Notify contexts */
  for (i = 0; i < num; i++) {
    actx_id = desc[i]->arx.msg.connupdate.db_id;
    actx = &fp_state->appctx[0][actx_id];
    notify_appctx(actx, tsc);
  }

  return num;
}