#include <stdio.h>
#include <assert.h>

#include <rte_config.h>
#include <rte_memcpy.h>
#include <rte_malloc.h>
#include <rte_lcore.h>
#include <rte_hash.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_ring.h>
#include <rte_version.h>
#include <rte_spinlock.h>
#include <rte_hash_crc.h>

#include <utils.h>
#include <utils_rng.h>
#include <tas_memif.h>
#include <pipeline.h>
#include <tas.h>

#define RING_SIZE   2048
#define ARX_DESCRIPTORS  2048
#define ATX_DESCRIPTORS  2048
#define DESC_SIZE   (RTE_CACHE_LINE_SIZE)

struct rte_ring *nbi_rx_queue;
struct rte_ring *nbi_tx_queue;
struct rte_ring *protocol_workqueues[NUM_FLOWGRPS];
struct rte_ring *postproc_workqueue;
struct rte_ring *dma_cmd_ring;
struct rte_hash *flow_lookup_table;
struct rte_ring *sp_rx_ring;
struct rte_ring *sched_tx_queue;
struct rte_ring *sched_bump_queue;
struct rte_ring *atx_ring;
struct rte_ring *arx_ring;
struct rte_mempool *arx_desc_pool;
struct rte_mempool *atx_desc_pool;

static inline uint32_t
flow_hash_crc(const void *data, __rte_unused uint32_t data_len,
        __rte_unused uint32_t init_val)
{
  return crc32c_sse42_u32(((uint32_t *) data)[2], crc32c_sse42_u64(((uint64_t *) data)[0], 0));
}

static struct rte_hash_parameters hash_conf = {
  .name = NULL,
  .entries = FLEXNIC_PL_FLOWST_NUM,
  .key_len = sizeof(ip_addr_t) + sizeof(ip_addr_t) + sizeof(in_port_t) + sizeof(in_port_t),
  .hash_func = flow_hash_crc,
};

int pipeline_init()
{
  char name[64];
  unsigned i;

  /* Init NBI RX queue */
  snprintf(name, 64, "nbi_rx_");
  nbi_rx_queue = rte_ring_create(name, NUM_SEQ_CTXS * RING_SIZE, rte_socket_id(),
          RING_F_SP_ENQ);

  if (nbi_rx_queue == NULL) {
    fprintf(stderr, "%s: %d\n", __func__, __LINE__);
    return -1;
  }

  /* Init NBI TX queue */
  snprintf(name, 64, "nbi_tx_");
  nbi_tx_queue = rte_ring_create(name, RING_SIZE, rte_socket_id(),
          RING_F_SC_DEQ);

  if (nbi_tx_queue == NULL) {
    fprintf(stderr, "%s: %d\n", __func__, __LINE__);
    return -1;
  }

  /* Init SP RX queue */
  snprintf(name, 64, "sp_rx_");
  sp_rx_ring = rte_ring_create(name, RING_SIZE, rte_socket_id(),
          RING_F_SC_DEQ);

  if (sp_rx_ring == NULL) {
    fprintf(stderr, "%s: %d\n", __func__, __LINE__);
    return -1;
  }

  /* Init Flow lookup table */
  snprintf(name, 64, "flow_hash_");
  hash_conf.name = name;
  hash_conf.socket_id = rte_socket_id();
  hash_conf.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF;
  flow_lookup_table = rte_hash_create(&hash_conf);
  
  if (flow_lookup_table == NULL) {
    fprintf(stderr, "%s: %d\n", __func__, __LINE__);
    return -1; 
  }

  /* Init Protocol workqueues */
  for (i = 0; i < NUM_FLOWGRPS; i++) {
    snprintf(name, 64, "protocol_wq_%u", i);
    protocol_workqueues[i] = rte_ring_create(name, 4 * RING_SIZE, rte_socket_id(),
            RING_F_SC_DEQ);   // 4 -> (RX + TX + AC + RETX)
    
    if (protocol_workqueues[i] == NULL) {
      fprintf(stderr, "%s: %d\n", __func__, __LINE__);
      return -1;
    }
  }

  /* Init postproc workqueue */
  snprintf(name, 64, "postproc_wq_");
  postproc_workqueue = rte_ring_create(name, 4 * NUM_FLOWGRPS * RING_SIZE, rte_socket_id(),
            0);   // 4 -> (RX + TX + AC + RETX) * NUM_FLOWGRPS
  if (postproc_workqueue == NULL) {
    fprintf(stderr, "%s: %d\n", __func__, __LINE__);
    return -1;
  }

  /* Init DMA workqueue */
  snprintf(name, 64, "dma_cmd_ring_");
  dma_cmd_ring = rte_ring_create(name, 4 * NUM_FLOWGRPS * RING_SIZE, rte_socket_id(),
            0);   // 4 -> (RX + TX + AC + RETX) * NUM_FLOWGRPS
  if (dma_cmd_ring == NULL) {
    fprintf(stderr, "%s: %d\n", __func__, __LINE__);
    return -1;
  }

  /* Init scheduler queue */
  snprintf(name, 64, "sched_wq_");
  sched_tx_queue = rte_ring_create(name, NUM_SEQ_CTXS * RING_SIZE, rte_socket_id(),
          RING_F_SP_ENQ);
  
  if (sched_tx_queue == NULL) {
    fprintf(stderr, "%s: %d\n", __func__, __LINE__);
    return -1;
  }

  snprintf(name, 64, "sched_bump_");
  sched_bump_queue = rte_ring_create(name, NUM_SEQ_CTXS * RING_SIZE, rte_socket_id(),
          RING_F_SC_DEQ);
  
  if (sched_bump_queue == NULL) {
    fprintf(stderr, "%s: %d\n", __func__, __LINE__);
    return -1;
  }

  /* Init ARX desc pool */
  snprintf(name, 32, "arx_desc_pool");
  arx_desc_pool = rte_mempool_create(name, ARX_DESCRIPTORS, DESC_SIZE, 64,
          0, NULL, NULL, NULL, NULL, rte_socket_id(), MEMPOOL_F_SP_PUT);
  if (arx_desc_pool == NULL) {
    fprintf(stderr, "network_thread_init: rte_mempool_create failed\n");
    return -1; 
  }

  /* Init ATX desc pool */
  snprintf(name, 32, "atx_desc_pool");
  atx_desc_pool = rte_mempool_create(name, ATX_DESCRIPTORS, DESC_SIZE, 64,
          0, NULL, NULL, NULL, NULL, rte_socket_id(), MEMPOOL_F_SC_GET);
  if (atx_desc_pool == NULL) {
    fprintf(stderr, "network_thread_init: rte_mempool_create failed\n");
    return -1; 
  }

  /* Init ARX queue */
  snprintf(name, 64, "appctx_rx_");
  arx_ring = rte_ring_create(name, RING_SIZE, rte_socket_id(),
          RING_F_SC_DEQ);

  if (arx_ring == NULL) {
    fprintf(stderr, "%s: %d\n", __func__, __LINE__);
    return -1;
  }

  /* Init ATX queue */
  snprintf(name, 64, "appctx_tx_");
  atx_ring = rte_ring_create(name, RING_SIZE, rte_socket_id(),
          RING_F_SP_ENQ);

  if (atx_ring == NULL) {
    fprintf(stderr, "%s: %d\n", __func__, __LINE__);
    return -1;
  }

  return 0;
}

void pipeline_cleanup(void) {
  // TODO: ?
}