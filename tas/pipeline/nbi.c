#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <rte_config.h>
#include <rte_atomic.h>
#include <rte_lcore.h>
#include <rte_ring.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include "utils_reorder.h"
#include "pipeline.h"

#define REORDER_BUFFER_SIZE   2048
#define BATCH_SIZE 32

struct utils_reorder_buffer *tx_sequencers[NUM_FLOWGRPS];
static uint16_t rx_seqn[NUM_FLOWGRPS];

struct nbi_thread_conf {
};

static unsigned poll_rx(uint16_t rxq)
{
  unsigned n, m, i;
  struct rte_mbuf *rx_pkts[BATCH_SIZE];
  struct nbi_pkt_t nbi_pkts[BATCH_SIZE];

  /* Poll for RX packets */
  n = rte_eth_rx_burst(net_port_id, rxq, rx_pkts, BATCH_SIZE);

  if (n == 0)
    return 0;

  for (i = 0; i < n; i++) {
    nbi_pkts[i].seqno = rx_seqn[rxq]++;
    nbi_pkts[i].seqr = rxq;
    nbi_pkts[i].dir = NBI_DIR_RX;
    nbi_pkts[i].mbuf = BUF_TO_PTR(rx_pkts[i]);
  }

  m = rte_ring_sp_enqueue_burst(nbi_rx_queue, (void **) nbi_pkts, n, NULL);

  /* Free packets on enqueue failure */
  for (i = m; i < n; i++) {
    rte_pktmbuf_free_seg(rx_pkts[i]);    // NOTE: We do not handle chained mbufs for efficiency!
  }

  return n;
}

static unsigned poll_tx()
{
  unsigned n, m, i;
  uint16_t seqr, seq;
  struct nbi_pkt_t nbi_pkts[BATCH_SIZE];
  struct rte_mbuf *tx_pkts[BATCH_SIZE];
  struct rte_mbuf *free_pkts[BATCH_SIZE];

  /* Poll for TX packets */
  n = rte_ring_sc_dequeue_burst(nbi_tx_queue, (void **) nbi_pkts, BATCH_SIZE, NULL);

  if (n == 0)
    return 0;

  /* Prefetch mbuf meta */
  for (i = 0; i < n; i++) {
    tx_pkts[i] = BUF_FROM_PTR(nbi_pkts[i]);
    rte_mbuf_prefetch_part1(tx_pkts[i]);
    rte_mbuf_prefetch_part2(tx_pkts[i]);
  }

  /* Insert packets into sequencer */
  for (i = 0, m = 0; i < n; i++) {
    seq = nbi_pkts[i].seqno;
    seqr = nbi_pkts[i].seqr;

    if (nbi_pkts[i].dir == NBI_DIR_FREE) {
      free_pkts[m++] = tx_pkts[i];
    }
    else {
      if (utils_reorder_insert(tx_sequencers[seqr], tx_pkts[i], seq) != 0) {
        free_pkts[m++] = tx_pkts[i];
      }
    }
  }

  /* Free error packets */
  for (i = 0; i < m; i++) {
    rte_pktmbuf_free_seg(free_pkts[i]);    // NOTE: We do not handle chained mbufs for efficiency!
  }

  return n;
}

static unsigned poll_sequencers(uint16_t txq)
{
  unsigned n, k, m, i;
  struct rte_mbuf *tx_pkts[BATCH_SIZE];

  /* Poll for ordered packets */
  n = utils_reorder_drain(tx_sequencers[txq], (void **) tx_pkts, BATCH_SIZE);

  if (n == 0)
    return 0;

  /* Remove NULL entries */
  for (i = 0, k = 0; i < n; i++) {
    if (tx_pkts[i] == NULL)
      continue;
    
    tx_pkts[k++] = tx_pkts[i];
  }

  if (k == 0)
    return n;

  m = rte_eth_tx_burst(net_port_id, txq, tx_pkts, k);

  /* Free untransmitted packets */
  for (i = m; i < k; i++) {
    rte_pktmbuf_free_seg(tx_pkts[i]);    // NOTE: We do not handle chained mbufs for efficiency!
  }

  return n;
}

/* Initialize NBI internal datastructures */
static void nbi_thread_init(struct nbi_thread_conf *conf)
{
  uint16_t q;
  (void) conf;

  for (q = 0; q < NUM_SEQ_CTXS; q++) {
    rx_seqn[q] = 0;
  }

  for (q = 0; q < NUM_SEQ_CTXS; q++) {
    tx_sequencers[q] = utils_reorder_init(rte_socket_id(), REORDER_BUFFER_SIZE);

    if (tx_sequencers[q] == NULL) {
      fprintf(stderr, "%s:%d\n", __func__, __LINE__);
      abort();
    }
  }
}

int nbi_thread(void *args)
{
  uint16_t q;
  struct nbi_thread_conf *conf = (struct nbi_thread_conf *) args;

  nbi_thread_init(conf);

  while (1) {

    for (q = 0; q < NUM_SEQ_CTXS; q++) {
      poll_rx(q);
      poll_tx();
      poll_sequencers(q);
    }    
  }

  return EXIT_SUCCESS;
}