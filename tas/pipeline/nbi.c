#include <stdint.h>
#include <stdlib.h>
#include <rte_config.h>
#include <rte_atomic.h>
#include <rte_ring.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_reorder.h>

#include "pipeline.h"

#define MAX_NB_RX   NUM_FLOWGRPS
#define MAX_NB_TX   NUM_FLOWGRPS
#define BATCH_SIZE  32

extern struct rte_ring *nbi_rx_queues[MAX_NB_RX];
extern struct rte_ring *nbi_tx_queues[MAX_NB_TX];
extern struct rte_reorder_buffer *tx_sequencers[MAX_NB_TX];

struct nbi_thread_conf {
  uint16_t port_id;
  
  /* Handles RX from [rx_queueid_start, rx_queueid_start + nb_rx) */
  uint16_t nb_rx;
  uint16_t rx_queueid_start;

  /* Handles TX to   [tx_queueid_start, tx_queueid_start + nb_tx) */
  /* Typically, each flow group has its own TX queue */
  uint16_t nb_tx;
  uint16_t tx_queueid_start;
};

static uint32_t rx_seqn[MAX_NB_RX];

static unsigned poll_rx(uint16_t port_id, uint16_t rxq)
{
  unsigned n, m, i;
  struct rte_mbuf *rx_pkts[BATCH_SIZE];

  /* Poll for RX packets */
  n = rte_eth_rx_burst(port_id, rxq, rx_pkts, BATCH_SIZE);

  if (n == 0)
    return 0;

  for (i = 0; i < n; i++) {
    rte_mbuf_prefetch_part2(rx_pkts[i]);
  }

  for (i = 0; i < n; i++) {
    rx_pkts[i]->seqn = rx_seqn[rxq]++;
  }

  rte_compiler_barrier();

  m = rte_ring_sp_enqueue_burst(nbi_rx_queues[rxq], rx_pkts, n, NULL);

  /* Free packets on enqueue failure */
  for (i = m; i < n; i++) {
    rte_pktmbuf_free_seg(rx_pkts[i]);    // NOTE: We do not handle chained mbufs for efficiency!
  }

  return n;
}

static unsigned poll_tx(uint16_t txq)
{
  unsigned n, m, i;
  struct rte_mbuf *tx_pkts[BATCH_SIZE];
  struct rte_mbuf *free_pkts[BATCH_SIZE];

  /* Poll for TX packets */
  n = rte_ring_sc_dequeue_burst(nbi_tx_queues[txq], tx_pkts, BATCH_SIZE, NULL);

  if (n == 0)
    return 0;

  /* Prefetch mbuf meta */
  for (i = 0; i < n; i++) {
    rte_mbuf_prefetch_part1(tx_pkts[i]);
    rte_mbuf_prefetch_part2(tx_pkts[i]);
  }

  /* Insert packets into sequencer */
  for (i = 0, m = 0; i < n; i++) {
    if (rte_reorder_insert(tx_sequencers[txq], tx_pkts[i]) != 0) {
      free_pkts[m++] = tx_pkts[i];
    }
  }

  /* Free error packets */
  for (i = 0; i < m; i++) {
    rte_pktmbuf_free_seg(free_pkts[i]);    // NOTE: We do not handle chained mbufs for efficiency!
  }

  return n;
}

static unsigned poll_sequencers(uint16_t port_id, uint16_t txq)
{
  unsigned n, m, i;
  struct rte_mbuf *tx_pkts[BATCH_SIZE];

  /* Poll for ordered packets */
  n = rte_reorder_drain(tx_sequencers[txq], tx_pkts, BATCH_SIZE);

  if (n == 0)
    return 0;

  m = rte_eth_tx_burst(port_id, txq, tx_pkts, n);

  /* Free untransmitted packets */
  for (i = m; i < n; i++) {
    rte_pktmbuf_free_seg(tx_pkts[i]);    // NOTE: We do not handle chained mbufs for efficiency!
  }
}

int nbi_thread(void *args)
{
  uint16_t port_id, rxq, txq;
  uint16_t rxq_start, rxq_end, txq_start, txq_end;

  uint32_t i, n, m;
  struct rte_mbuf *rx_pkts[BATCH_SIZE];
  struct rte_mbuf *tx_pkts[BATCH_SIZE];
  
  struct nbi_thread_conf *conf = (struct nbi_thread_conf *) args;

  port_id = conf->port_id;
  rxq_start = rxq = conf->rx_queueid_start;
  rxq_end = rxq_start + conf->nb_rx;
  txq_start = txq = conf->tx_queueid_start;
  txq_end = txq_start + conf->nb_tx;

  while (1) {
    n = poll_rx(port_id, rxq);
    if (++rxq == rxq_end)
      rxq = rxq_start;

    n = poll_tx(txq);

    n = poll_sequencers(port_id, txq);
    if (++txq == txq_end)
      txq = txq_start;
  }

  return EXIT_SUCCESS;
}