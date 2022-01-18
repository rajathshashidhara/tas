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

#define RING_SIZE   4096

struct rte_ring *nbi_rx_queues[NUM_SEQ_CTXS];
struct rte_ring *nbi_tx_queues[NUM_SEQ_CTXS];
struct rte_ring *protocol_workqueues[NUM_FLOWGRPS];
struct rte_hash *flow_lookup_table;
struct rte_ring *sp_rx_ring;

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

  /* Init NBI RX queues */
  for (i = 0; i < NUM_SEQ_CTXS; i++) {
    snprintf(name, 64, "nbi_rx_%u", i);
    nbi_rx_queues[i] = rte_ring_create(name, RING_SIZE, rte_socket_id(),
            RING_F_SP_ENQ);

    if (nbi_rx_queues[i] == NULL) {
      fprintf(stderr, "%s: %d\n", __func__, __LINE__);
      return -1;
    }
  }

  /* Init NBI TX queues */
  for (i = 0; i < NUM_SEQ_CTXS; i++) {
    snprintf(name, 64, "nbi_tx_%u", i);
    nbi_tx_queues[i] = rte_ring_create(name, RING_SIZE, rte_socket_id(),
            RING_F_SC_DEQ);

    if (nbi_tx_queues[i] == NULL) {
      fprintf(stderr, "%s: %d\n", __func__, __LINE__);
      return -1;
    }
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

  return 0;
}

void pipeline_cleanup(void) {
  // TODO: ?
}