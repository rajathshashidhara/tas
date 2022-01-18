#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <rte_config.h>
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_mbuf_ptype.h>
#include <rte_hash.h>

#include "utils.h"
#include "pipeline.h"
#include "packet_defs.h"
#include "tas.h"

#define BATCH_SIZE 16

struct preproc_thread_conf {

};

#define VALID_MBUF_PTYPE   (RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_TCP)
#define VALID_MBUF_OFLAGS  (PKT_RX_IP_CKSUM_GOOD | PKT_RX_L4_CKSUM_GOOD)
#define VALID_TCP_HDRSIZE  ((sizeof(struct pkt_tcp_ts) - offsetof(struct pkt_tcp_ts, tcp)) / 4)
#define VALID_TCP_FLAGS    (TCP_ACK | TCP_PSH | TCP_ECE | TCP_CWR | TCP_FIN)
#define VALID_TCPOPT_KIND  (TCP_OPT_TIMESTAMP)
#define VALID_TCPOPT_SIZE  (sizeof(struct tcp_timestamp_opt))

static int validate_pkt_header(struct rte_mbuf *buf)
{
  uint16_t len;

  struct pkt_tcp_ts *pkt = rte_pktmbuf_mtod(buf, struct pkt_tcp_ts *);
  len = rte_pktmbuf_data_len(buf);

  int cond =
    ((buf->packet_type & VALID_MBUF_PTYPE) != VALID_MBUF_PTYPE)     |
    ((buf->ol_flags & VALID_MBUF_OFLAGS) != VALID_MBUF_OFLAGS)      |
    (TCPH_HDRLEN(&pkt->tcp) != VALID_TCP_HDRSIZE)                   |
    ((TCPH_FLAGS(&pkt->tcp) & (~VALID_TCP_FLAGS)) != 0)             |
    ((len < f_beui16(pkt->ip.len) + sizeof(pkt->eth)))              |
    (pkt->ts_opt.kind != VALID_TCPOPT_KIND)                         | 
    (pkt->ts_opt.length != VALID_TCPOPT_SIZE);

  return cond;
}

static void prepare_rx_work(struct rte_mbuf *pkt,
            struct flextcp_pl_flowst_conn_t *conn,
            uint16_t seqn)
{
  struct work_t *w = (struct work_t *) pkt->buf_addr; // Use the headroom to store metadata
  struct pkt_tcp_ts *p = rte_pktmbuf_mtod(pkt, struct pkt_tcp_ts *);

  memset(w, 0, sizeof(struct work_t));

  w->type = WORK_TYPE_RX;
  w->flags = 0;
  w->len = rte_pktmbuf_data_len(pkt) - sizeof(struct pkt_tcp_ts); // Should be positive after validate
  w->flow_id = conn->flow_id;
  w->flow_grp = conn->flow_grp;
  w->reorder_seqn = seqn;
  w->mbuf = pkt;

  w->seq = f_beui32(p->tcp.seqno) - conn->ack_delta;
  w->ack = f_beui32(p->tcp.ackno) - conn->seq_delta;
  w->tcp_flags = TCPH_FLAGS(&p->tcp);
  w->win = f_beui16(p->tcp.wnd);
  w->ts_val = f_beui32(p->ts_opt.ts_val);
  w->ts_ecr = f_beui32(p->ts_opt.ts_ecr);

  if (IPH_ECN(&p->ip) == IP_ECN_CE) {
    w->flags |= WORK_FLAG_IP_ECE;
  }
}

static void prepare_ack_header(struct pkt_tcp_ts *p)
{
  struct eth_addr eth;
  ip_addr_t ip;
  beui16_t port;

  /* swap addresses */
  eth = p->eth.src;
  p->eth.src = p->eth.dest;
  p->eth.dest = eth;
  ip = p->ip.src;
  p->ip.src = p->ip.dest;
  p->ip.dest = ip;
  port = p->tcp.src;
  p->tcp.src = p->tcp.dest;
  p->tcp.dest = port;

  /* mark ACKs as ECN in-capable */
  IPH_ECN_SET(&p->ip, IP_ECN_NONE);

  p->ip.len = t_beui16(sizeof(struct pkt_tcp_ts) - offsetof(struct pkt_tcp_ts, ip));
  p->ip.ttl = 0xff;
}

static unsigned preprocess_rx(uint16_t rxq)
{
  unsigned i, ret;
  unsigned num_rx, num_sp, num_proc;
  struct nbi_pkt_t nbi_pkts[BATCH_SIZE];    /*> Packets desc. from RX queue */
  struct rte_mbuf *pkts[BATCH_SIZE];        /*> Packet buffers */
  struct workptr_t sp_pkts[BATCH_SIZE];     /*> Packets filtered out to slowpath */
  struct workptr_t proc_pkts[BATCH_SIZE];   /*> Packets for protocol stage */
  struct flextcp_pl_flowst_conn_t *conn;

  const void *keys_4tuple[BATCH_SIZE];
  int32_t flow_ids[BATCH_SIZE];

  num_rx = rte_ring_mc_dequeue_burst(nbi_rx_queues[rxq], (void **) nbi_pkts, BATCH_SIZE, NULL);
  if (num_rx == 0)
    return 0;

  /* Prefetch mbuf metadata */
  for (i = 0; i < num_rx; i++) {
    pkts[i] = BUF_FROM_PTR(nbi_pkts[i]);
    rte_mbuf_prefetch_part1(pkts[i]);
  }

  /* Prefetch packet contents (1 cache line) */
  for (i = 0; i < num_rx; i++) {
    rte_prefetch0(rte_pktmbuf_mtod(pkts[i], void *));
    keys_4tuple[i] = rte_pktmbuf_mtod_offset(pkts[i], const void *, offsetof(struct pkt_tcp, ip.src));
  }

  rte_hash_lookup_bulk(flow_lookup_table, keys_4tuple, num_rx, flow_ids);

  num_sp = 0;
  for (i = 0; i < num_rx; i++) {
    /* Filter failed lookups to slowpath */
    if (flow_ids[i] == -ENOENT)
      goto slowpath;

    /* Prefetch connection state */
    rte_prefetch0(&fp_state->flows_conn_info[flow_ids[i]]);

    /* Validate packet header */
    if (validate_pkt_header(pkts[i]))
      goto slowpath;

    continue;

slowpath:
    sp_pkts[num_sp].__rawptr = 0;
    sp_pkts[num_sp].type = WORK_TYPE_RX;
    sp_pkts[num_sp].flow_grp = nbi_pkts[i].seqr;
    sp_pkts[num_sp].addr = BUF_TO_PTR(pkts[i]);
    num_sp++;

    pkts[i] = NULL;
  }

  num_proc = 0;
  for (i = 0; i < num_rx; i++) {
    if (pkts[i] == NULL)
      continue;

    conn = &fp_state->flows_conn_info[flow_ids[i]];

    proc_pkts[num_proc].__rawptr = 0;
    proc_pkts[num_proc].type = WORK_TYPE_RX;
    proc_pkts[num_proc].flow_id = flow_ids[i];
    proc_pkts[num_proc].flow_grp = nbi_pkts[i].seqr;
    proc_pkts[num_proc].addr = BUF_TO_PTR(pkts[i]);

    /* Generate packet summary for protocol state */
    prepare_rx_work(pkts[i], conn, nbi_pkts[i].seqno);

    /* Swap TCP headers in anticipation of sending an ACK */
    prepare_ack_header(rte_pktmbuf_mtod(pkts[i], struct pkt_tcp_ts *));

    num_proc++;
  }

  if (num_sp > 0) {
    ret = rte_ring_mp_enqueue_burst(sp_rx_ring, (void **) sp_pkts, num_sp, NULL);

    for (i = ret; i < num_sp; i++)
      rte_pktmbuf_free_seg(BUF_FROM_PTR(sp_pkts[i]));     // NOTE: We do not handle chained mbufs here!
  }

  if (num_proc > 0) {
    ret = rte_ring_mp_enqueue_burst(protocol_workqueues[rxq], (void **) proc_pkts, num_proc, NULL);

    for (i = ret; i < num_proc; i++)
      rte_pktmbuf_free_seg(BUF_FROM_PTR(proc_pkts[i]));   // NOTE: We do not handle chained mbufs here!
  }

  return num_rx;
}

static void prepare_tx_work(struct rte_mbuf   *pkt,
                            struct sched_tx_t tx)
{
  struct work_t *w = (struct work_t *) pkt->buf_addr; // Use the headroom to store metadata

  rte_pktmbuf_data_len(pkt) = rte_pktmbuf_pkt_len(pkt) = sizeof(struct pkt_tcp_ts);
  memset(w, 0, sizeof(struct work_t));

  w->type = WORK_TYPE_TX;
  w->flags = ((tx.flags & SCHED_FLAG_TX_FORCE) == 0 ? 0 : WORK_FLAG_QM_FORCE);
  w->len = tx.len;
  w->flow_id = tx.flow_id;
  w->flow_grp = tx.flow_grp;
  w->reorder_seqn = 0;
  w->mbuf = pkt;
}

static void prepare_seg_header(struct pkt_tcp_ts *p,
                               struct flextcp_pl_flowst_conn_t *conn)
{
  p->eth.dest = conn->remote_mac;
  memcpy(&p->eth.src, &eth_addr, ETH_ADDR_LEN);
  p->eth.type = t_beui16(ETH_TYPE_IP);

  IPH_VHL_SET(&p->ip, 4, 5);
  p->ip._tos = 0;
  p->ip.len = t_beui16(sizeof(struct pkt_tcp_ts) - offsetof(struct pkt_tcp_ts, ip));    /*> correct value incl. pyld. filled by postproc */
  p->ip.id = t_beui16(3); /* TODO: not sure why we have 3 here */
  p->ip.offset = t_beui16(0);
  p->ip.ttl = 0xff;
  p->ip.proto = IP_PROTO_TCP;
  p->ip.chksum = 0;
  p->ip.src = conn->local_ip;
  p->ip.dest = conn->remote_ip;

  /* mark as ECN capable if flow marked so */
  if ((conn->flags & FLEXNIC_PL_FLOWST_ECN) == FLEXNIC_PL_FLOWST_ECN) {
    IPH_ECN_SET(&p->ip, IP_ECN_ECT0);
  }

  p->tcp.src = conn->local_port;
  p->tcp.dest = conn->remote_port;

  p->ts_opt.kind = TCP_OPT_TIMESTAMP;
  p->ts_opt.length = sizeof(struct tcp_timestamp_opt);
  p->pad = 0;
}

static unsigned preprocess_tx(uint16_t txq)
{
  unsigned i, num_tx, num_enq;
  
  struct sched_tx_t tx[BATCH_SIZE];
  struct rte_mbuf *pkts[BATCH_SIZE];
  struct flextcp_pl_flowst_conn_t *conns[BATCH_SIZE];
  struct workptr_t proc_pkts[BATCH_SIZE];

  num_tx = rte_ring_mc_dequeue_burst(sched_tx_queues[txq], (void **) tx, BATCH_SIZE, NULL);
  if (num_tx == 0)
    return 0;

  /* Prefetch connection state */
  for (i = 0; i < num_tx; i++) {
    conns[i] = &fp_state->flows_conn_info[tx[i].flow_id];
    rte_prefetch0(conns[i]);
  }

  /* Allocate mbufs */
  if (rte_pktmbuf_alloc_bulk(tx_pkt_mempool, pkts, num_tx) != 0)
    return 0; // FIXME: we discard the scheduled entries here!

  /* Prefetch mbufs */
  for (i = 0; i < num_tx; i++) {
    rte_prefetch0(pkts[i]);
    rte_prefetch0((((uint8_t *) pkts[i]) + sizeof(struct rte_mbuf)));
    rte_prefetch0((((uint8_t *) pkts[i]) + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM));
  }
  
  for (i = 0; i < num_tx; i++) {
    /* Prepare work descriptor */
    prepare_tx_work(pkts[i], tx[i]);

    /* Prepare segment header */
    prepare_seg_header(rte_pktmbuf_mtod(pkts[i], struct pkt_tcp_ts *), conns[i]);

    proc_pkts[i].__rawptr = 0;
    proc_pkts[i].type = WORK_TYPE_TX;
    proc_pkts[i].flow_id = tx[i].flow_id;
    proc_pkts[i].flow_grp = tx[i].flow_grp;
    proc_pkts[i].addr = BUF_TO_PTR(pkts[i]);
  }

  num_enq = rte_ring_mp_enqueue_burst(protocol_workqueues[txq], (void **) proc_pkts, num_tx, NULL);

  for (i = num_enq; i < num_tx; i++) {
    rte_pktmbuf_free_seg(pkts[i]);   // NOTE: We do not handle chained mbufs here! 
  }

  return num_tx;
}

void preproc_thread_init(struct preproc_thread_conf *conf) {
  (void) conf;

  // TODO: ?
}

int preproc_thread(void *args) {
  uint16_t q;
  struct preproc_thread_conf *conf = (struct preproc_thread_conf *) args;

  preproc_thread_init(conf);

  q = 0;
  while (1) {
    preprocess_rx(q);
    preprocess_tx(q);

    q++;
    if (q == NUM_FLOWGRPS)
      q = 0;
  }

  return EXIT_SUCCESS;
}

