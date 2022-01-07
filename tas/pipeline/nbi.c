#include <stdint.h>
#include <stdlib.h>
#include <rte_config.h>
#include <rte_ring.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include "pipeline.h"

#define MAX_NB_RX   NUM_FLOWGRPS
#define MAX_NB_TX   NUM_FLOWGRPS
#define BATCH_SIZE  32

extern struct rte_ring *nbi_rx_queues[MAX_NB_RX];
extern struct rte_ring *nbi_tx_queues[MAX_NB_TX];

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
    /* Poll for RX packets */
    n = rte_eth_rx_burst(port_id, rxq, rx_pkts, BATCH_SIZE);

    if (n == 0)
      goto next_rx_queue;

    /* Assign sequence number to each packet on ingress */
    for (i = 0; i < n; i++) {
      rx_pkts[i]->seqn = rx_seqn[rxq]++;
    }

    m = rte_ring_sp_enqueue_burst(nbi_rx_queues[rxq], rx_pkts, n, NULL);

    /* Free packets on enqueue failure */
    for (i = m; i < n; i++) {
      rte_pktmbuf_free_seg(rx_pkts[i]);   // NOTE: We do not handle chained mbufs for efficiency!
    }

next_rx_queue:
    if (++rxq == rxq_end)
      rxq = rxq_start;

    /* Poll for TX packets */
    n = rte_ring_sc_dequeue_burst(nbi_tx_queues[txq], tx_pkts, BATCH_SIZE, NULL);

    if (n == 0)
      goto next_tx_queue;
    
    m = rte_eth_tx_burst(port_id, txq, tx_pkts, n);
    
    /* Free packets on transmit failure */
    for (i = m; i < n; i++) {
      rte_pktmbuf_free_seg(tx_pkts[i]);   // NOTE: We do not handle chained mbufs for efficiency!
    }

next_tx_queue:
    if (++txq == txq_end)
      txq = txq_start;
  }

  return EXIT_SUCCESS;
}