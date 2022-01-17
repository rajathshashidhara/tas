#include <stdint.h>
#include <stdlib.h>
#include <rte_config.h>
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_mbuf_ptype.h>
#include <rte_hash.h>

#include "utils.h"
#include "pipeline.h"
#include "packet_defs.h"

#define MAX_NB_RX       NUM_FLOWGRPS
#define BATCH_SIZE      8

extern struct rte_ring *nbi_rx_queues[MAX_NB_RX];
extern struct rte_ring *preproc_queues[NUM_FLOWGRPS + 1];    // NOTE: 1 additional queue for slowpath
extern struct rte_ring *sched_queues[MAX_NB_TX];

extern struct rte_mempool *tx_mbuf_pool;

extern struct rte_hash *flow_lookup_table;

#if RTE_VER_YEAR < 19
  struct ether_addr eth_addr;
#else
  struct rte_ether_addr eth_addr;
#endif

struct preproc_thread_conf {
  uint16_t nb_rx;
};

#define VALID_MBUF_PTYPE   (RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_TCP)
#define VALID_MBUF_OFLAGS  (PKT_RX_IP_CKSUM_GOOD | PKT_RX_L4_CKSUM_GOOD)
#define VALID_TCP_HDRSIZE  ((sizeof(struct pkt_tcp_ts) - offsetof(struct pkt_tcp_ts, tcp)) / 4)
#define VALID_TCP_FLAGS    (TCP_ACK | TCP_PSH | TCP_ECE | TCP_CWR | TCP_FIN)
#define VALID_TCPOPT_KIND  (TCP_OPT_TIMESTAMP)
#define VALID_TCPOPT_SIZE  (sizeof(struct tcp_timestamp_opt))

static int validate_pkt_header(struct rte_mbuf *buf)
{
  struct pkt_tcp_ts *pkt = (struct pkt_tcp_ts *) rte_pktmbuf_mtod(buf);
  int cond =
    ((buf->packet_type & VALID_MBUF_PTYPE) != VALID_MBUF_PTYPE)     |
    ((buf->ol_flags & VALID_MBUF_OFLAGS) != VALID_MBUF_OFLAGS)      |
    (TCPH_HDRLEN(&pkt->tcp) != VALID_TCP_HDRSIZE)                   |
    ((TCPH_FLAGS(&pkt->tcp) & (~VALID_TCP_FLAGS)) != 0)             |
    (pkt->ts_opt.kind != VALID_TCPOPT_KIND)                         | 
    (pkt->ts_opt.length != VALID_TCPOPT_SIZE));

  return cond;
}

static void generate_pkt_summary(struct rte_mbuf *pkt,
            struct flextcp_pl_flowst_conn_t *conn_info)
{
  struct work_t *w = (struct work_t *) pkt->buf_addr; // Use the headroom to store metadata
  struct pkt_tcp_ts *p = (struct pkt_tcp_ts *) rte_pktmbuf_mtod(pkt);

  w->seq = f_beui32(p->tcp.seqno) - conn_info->ack_delta;
  w->ack = f_beui32(p->tcp.ackno) - conn_info->seq_delta;
  w->tcp_flags = TCPH_FLAGS(&p->tcp);
  w->win = f_beui16(p->tcp.wnd);
  w->ts_val = f_beui32(p->ts_opt.ts_val);
  w->ts_ecr = f_beui32(p->ts_opt.ts_ecr);

  if (IPH_ECN(&p->ip) == IP_ECN_CE) {
    w->flags |= WORK_FLAG_IP_ECE;
  }
}

static void prepare_ack_header(struct rte_mbuf *pkt)
{
  struct eth_addr eth;
  ip_addr_t ip;
  beui16_t port;
  struct pkt_tcp_ts *p = (struct pkt_tcp_ts *) rte_pktmbuf_mtod(pkt);

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

static void prepare_seg_header(struct rte_mbuf *pkt,
                               struct flextcp_pl_flowst_conn_t *conn_info)
{
  struct pkt_tcp_ts *p = (struct pkt_tcp_ts *) rte_pktmbuf_mtod(pkt);

  p->eth.dest = conn_info->remote_mac;
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
  p->ip.src = fs->local_ip;
  p->ip.dest = fs->remote_ip;

  /* mark as ECN capable if flow marked so */
  if ((conn_info->flags & FLEXNIC_PL_FLOWST_ECN) == FLEXNIC_PL_FLOWST_ECN) {
    IPH_ECN_SET(&p->ip, IP_ECN_ECT0);
  }

  p->tcp.src = fs->local_port;
  p->tcp.dest = fs->remote_port;

  p->ts_opt.kind = TCP_OPT_TIMESTAMP;
  p->ts_opt.length = sizeof(struct tcp_timestamp_opt);
  p->pad = 0;
}

static void prepare_tx_work_desc(struct sched_tx_t *tx,
                                 struct work_t *pkt)
{
  // TODO: Not yet implemented!
} 

static unsigned preprocess_tx(uint16_t txq)
{
  unsigned i, num, ret;
  
  struct sched_tx_t tx[BATCH_SIZE];
  struct rte_mbuf *pkts[BATCH_SIZE];
  struct flextcp_pl_flowst_conn_t *conns[BATCH_SIZE];
  struct work_t *work[BATCH_SIZE];

  num = rte_ring_mc_dequeue_burst(sched_queues[txq], (void **) tx, BATCH_SIZE, NULL);
  if (num == 0)
    return 0;

  /* Prefetch connection state */
  for (i = 0; i < num; i++) {
    conns[i] = &fp_state->flows_conn_info[tx[i].flow_id];
    rte_prefetch0(conns[i]);
  }

  /* Allocate mbufs */
  if (rte_pktmbuf_alloc_bulk(tx_mbuf_pool, pkts, num) != 0)
    return 0; // FIXME: we discard the scheduled entries here!

  /* Prefetch mbufs */
  for (i = 0; i < num; i++) {
    rte_prefetch0(pkts[i]);
    rte_prefetch0((((uint8_t *) pkts[i]) + sizeof(struct rte_mbuf)));
    rte_prefetch0((((uint8_t *) pkts[i]) + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM));

    work_t[i] = (((uint8_t *) pkts[i]) + sizeof(struct rte_mbuf));
  }

  /* Prepare segment header */
  for (i = 0; i < num; i++) {
    prepare_seg_header(tx[i], pkts[i], conns[i]);
    prepare_tx_work_desc(tx[i], work[i]);
  }

  ret = rte_ring_mp_enqueue_burst(preproc_queues[txq], work, num, NULL);

  for (i = ret; i < num; i++) {
    rte_pktmbuf_free_seg(sp_pkts[i]);   // NOTE: We do not handle chained mbufs here! 
  }

  return num;
}

static unsigned preprocess_rx(uint16_t rxq)
{
  unsigned i, ret;
  unsigned num_rx, num_sp, num_proc;
  struct rte_mbuf *pkts[BATCH_SIZE];        /*> Packets input from RX queue */
  struct rte_mbuf *sp_pkts[BATCH_SIZE];     /*> Packets filtered out to slowpath */
  struct rte_mbuf *proc_pkts[BATCH_SIZE];   /*> Packets for protocol stage */

  void *keys_4tuple[BATCH_SIZE];
  int32_t flow_ids[BATCH_SIZE];

  num_rx = rte_ring_mc_dequeue_burst(nbi_rx_queues[rxq], pkts, BATCH_SIZE, NULL);
  if (num_rx == 0)
    return 0;

  /* Prefetch mbuf metadata */
  for (i = 0; i < num_rx; i++) {
    rte_mbuf_prefetch_part1(pkts[i]);
  }

  /* Prefetch packet contents (1 cache line) */
  for (i = 0; i < num_rx; i++) {
    rte_prefetch0(rte_pktmbuf_mtod(pkts[i]));
    keys_4tuple[i] = (void *) rte_pktmbuf_mtod_offset(pkts[i], offsetof(struct pkt_tcp, ip.src));
  }

  rte_hash_lookup_bulk(flow_lookup_table, keys_4tuple, num_rx, flow_ids);

  num_sp = 0;
  for (i = 0; i < num_rx; i++) {
    /* Filter failed lookups to slowpath */
    if (flow_ids[i] == -ENOENT) {
      sp_pkts[num_sp++] = pkts[i];
      continue;
    }

    /* Prefetch connection state */
    rte_prefetch0(&fp_state->flows_conn_info[flow_ids[i]]);

    /* Validate packet header */
    if (validate_pkt_header(pkts[i])) {
      sp_pkts[num_sp++] = pkts[i];
      continue;
    }

    /* Generate packet summary for protocol state */
    generate_pkt_summary(pkts[i]);

    /* Swap TCP headers in anticipation of sending an ACK */
    prepare_ack_header(pkts[i]);

    proc_pkts[num_proc++] = pkts[i];
  }

  if (num_sp > 0) {
    ret = rte_ring_mp_enqueue_burst(preproc_queues[NUM_FLOWGRPS], sp_pkts, num_sp, NULL);

    for (i = ret; i < num_sp; i++)
      rte_pktmbuf_free_seg(sp_pkts[i]);     // NOTE: We do not handle chained mbufs here!
    }
  }

  if (num_proc > 0) {
    ret = rte_ring_mp_enqueue_burst(preproc_queues[rxq], proc_pkts, num_proc, NULL);

    for (i = ret; i < num_sp; i++)
      rte_pktmbuf_free_seg(proc_pkts[i]);   // NOTE: We do not handle chained mbufs here!
    }
  }

  return num_rx; 
}

