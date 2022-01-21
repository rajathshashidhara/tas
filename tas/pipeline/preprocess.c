#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
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

struct preproc_ctx {
  struct workptr_t proc_work[NUM_FLOWGRPS][BATCH_SIZE];
  unsigned num_proc[NUM_FLOWGRPS];

  struct workptr_t sp_work[BATCH_SIZE];
  unsigned num_sp;
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

static void prepare_sp_work(struct preproc_ctx *ctx,
            struct rte_mbuf *pkt,
            struct nbi_pkt_t ndesc)
{
  unsigned idx, fgp;
  idx = ctx->num_sp;

  ctx->sp_work[idx].__rawptr = 0;
  ctx->sp_work[idx].type = WORK_TYPE_RX;
  ctx->sp_work[idx].flow_grp = ndesc.seqr;
  ctx->sp_work[idx].addr = BUF_TO_PTR(pkt);
  ctx->num_sp++;

  /* Add seq skip step to protocol block */
  fgp = ndesc.seqr;
  idx = ctx->num_proc[fgp];
  ctx->proc_work[fgp][idx].__rawptr = 0;
  ctx->proc_work[fgp][idx].type = WORK_TYPE_RX;
  ctx->proc_work[fgp][idx].flow_id = INVALID_FLOWID;
  ctx->proc_work[fgp][idx].flow_grp = fgp;
  ctx->proc_work[fgp][idx].addr = ndesc.seqno;
  ctx->num_proc[fgp]++;
}

static void prepare_rx_work(struct preproc_ctx *ctx,
            struct rte_mbuf *pkt,
            struct flextcp_pl_flowst_conn_t *conn,
            struct nbi_pkt_t ndesc)
{
  unsigned fgp, idx;
  struct work_t *w = (struct work_t *) pkt->buf_addr; // Use the headroom to store metadata
  struct pkt_tcp_ts *p = rte_pktmbuf_mtod(pkt, struct pkt_tcp_ts *);

  memset(w, 0, sizeof(struct work_t));

  w->type = WORK_TYPE_RX;
  w->flags = 0;
  w->len = rte_pktmbuf_data_len(pkt) - sizeof(struct pkt_tcp_ts); // Should be positive after validate
  w->flow_id = conn->flow_id;
  w->flow_grp = ndesc.seqr;
  w->reorder_seqn = ndesc.seqno;
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

  fgp = ndesc.seqr;
  idx = ctx->num_proc[fgp];
  ctx->proc_work[fgp][idx].__rawptr = 0;
  ctx->proc_work[fgp][idx].type = WORK_TYPE_RX;
  ctx->proc_work[fgp][idx].flow_id = w->flow_id;
  ctx->proc_work[fgp][idx].flow_grp = ndesc.seqr;
  // ctx->proc_work[fgp][idx].addr = BUF_TO_PTR(pkt);
  ctx->proc_work[fgp][idx].addr = BUF_TO_PTR(w);

  ctx->num_proc[fgp]++;
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

static unsigned preprocess_rx(struct preproc_ctx *ctx, unsigned max_num)
{
  unsigned i;
  unsigned num_rx;
  struct nbi_pkt_t nbi_pkts[BATCH_SIZE];    /*> Packets desc. from RX queue */
  struct rte_mbuf *pkts[BATCH_SIZE];        /*> Packet buffers */
  struct flextcp_pl_flowst_conn_t *conn;

  const void *keys_4tuple[BATCH_SIZE];
  int32_t flow_ids[BATCH_SIZE];

  num_rx = rte_ring_mc_dequeue_burst(nbi_rx_queue, (void **) nbi_pkts, max_num, NULL);
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
    prepare_sp_work(ctx, pkts[i], nbi_pkts[i]);
    pkts[i] = NULL;
  }

  for (i = 0; i < num_rx; i++) {
    if (pkts[i] == NULL)
      continue;

    conn = &fp_state->flows_conn_info[flow_ids[i]];

    /* Generate packet summary for protocol state */
    prepare_rx_work(ctx, pkts[i], conn, nbi_pkts[i]);

    /* Swap TCP headers in anticipation of sending an ACK */
    prepare_ack_header(rte_pktmbuf_mtod(pkts[i], struct pkt_tcp_ts *));
  }

  return num_rx;
}

static void prepare_tx_work(struct preproc_ctx *ctx,
                            struct rte_mbuf   *pkt,
                            struct sched_tx_t tx)
{
  unsigned fgp, idx;
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

  fgp = tx.flow_grp;
  idx = ctx->num_proc[fgp];
  ctx->proc_work[fgp][idx].__rawptr = 0;
  ctx->proc_work[fgp][idx].type = WORK_TYPE_TX;
  ctx->proc_work[fgp][idx].flow_id = tx.flow_id;
  ctx->proc_work[fgp][idx].flow_grp = tx.flow_grp;
  // ctx->proc_work[fgp][idx].addr = BUF_TO_PTR(pkt);
  ctx->proc_work[fgp][idx].addr = BUF_TO_PTR(w);

  ctx->num_proc[fgp]++;
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

static unsigned preprocess_tx(struct preproc_ctx *ctx, unsigned max_num)
{
  unsigned i, num_tx;
  
  struct sched_tx_t tx[BATCH_SIZE];
  struct rte_mbuf *pkts[BATCH_SIZE];
  struct flextcp_pl_flowst_conn_t *conns[BATCH_SIZE];

  num_tx = rte_ring_mc_dequeue_burst(sched_tx_queue, (void **) tx, max_num, NULL);
  if (num_tx == 0)
    return 0;

  /* Prefetch connection state */
  for (i = 0; i < num_tx; i++) {
    conns[i] = &fp_state->flows_conn_info[tx[i].flow_id];
    rte_prefetch0(conns[i]);
  }

  /* Allocate mbufs */
  if (rte_pktmbuf_alloc_bulk(tx_pkt_mempool, pkts, num_tx) != 0) {
    DEBUG();
    return 0; // FIXME: we discard the scheduled entries here!
  }

  /* Prefetch mbufs */
  for (i = 0; i < num_tx; i++) {
    rte_prefetch0(pkts[i]);
    rte_prefetch0((((uint8_t *) pkts[i]) + sizeof(struct rte_mbuf)));
    rte_prefetch0((((uint8_t *) pkts[i]) + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM));
  }
  
  for (i = 0; i < num_tx; i++) {
    /* Prepare work descriptor */
    tx[i].flow_grp = conns[i]->flow_grp; // FIXME
    prepare_tx_work(ctx, pkts[i], tx[i]);

    /* Prepare segment header */
    prepare_seg_header(rte_pktmbuf_mtod(pkts[i], struct pkt_tcp_ts *), conns[i]);
  }

  return num_tx;
}

static void prepare_ac_work(struct preproc_ctx *ctx, struct actxptr_t dptr)
{
  unsigned fgp, idx;
  struct work_t w;
  struct appctx_desc_t *desc = (struct appctx_desc_t *) BUF_FROM_PTR(dptr);

  memset(&w, 0, sizeof(struct work_t));

  w.type = WORK_TYPE_AC;
  w.flags = ((desc->atx.msg.connupdate.flags & FLEXTCP_PL_ATX_FLTXDONE) == 0 ? 0 : WORK_FLAG_FIN);
  w.flow_id = desc->atx.msg.connupdate.flow_id;
  w.flow_grp = desc->atx.msg.connupdate.flow_grp;
  w.rx_bump = desc->atx.msg.connupdate.rx_bump;
  w.tx_bump = desc->atx.msg.connupdate.tx_bump;
  w.reorder_seqn = dptr.seqno;
  w.mbuf = desc;

  rte_memcpy(desc, &w, sizeof(struct work_t));

  fgp = w.flow_grp;
  idx = ctx->num_proc[fgp];
  
  ctx->proc_work[fgp][idx].__rawptr = 0;
  ctx->proc_work[fgp][idx].type = WORK_TYPE_AC;
  ctx->proc_work[fgp][idx].flow_id  = w.flow_id;
  ctx->proc_work[fgp][idx].flow_grp = w.flow_grp;
  ctx->proc_work[fgp][idx].addr = BUF_TO_PTR(desc);
  ctx->num_proc[fgp]++;
}

static unsigned preprocess_ac(struct preproc_ctx *ctx, unsigned max_num)
{
  unsigned i, num_ac;
  
  struct actxptr_t ac[BATCH_SIZE];
  struct appctx_desc_t *descs[BATCH_SIZE];

  num_ac = rte_ring_mc_dequeue_burst(atx_ring, (void **) ac, max_num, NULL);
  if (num_ac == 0)
    return 0;

  for (i = 0; i < num_ac; i++) {
    descs[i] = (struct appctx_desc_t *) BUF_FROM_PTR(ac[i]);
    rte_prefetch0(descs[i]);
  }

  for (i = 0; i < num_ac; i++) {
    /* Prepare work */
    prepare_ac_work(ctx, ac[i]);
  }

  return num_ac;
}

void preproc_thread_init(struct preproc_thread_conf *conf) {
  (void) conf;

  // TODO: ?
}

int preproc_thread(void *args) {
  unsigned q, n, x;
  struct preproc_thread_conf *conf = (struct preproc_thread_conf *) args;
  struct preproc_ctx ctx;

  preproc_thread_init(conf);
  dataplane_stats_coreinit(PREPROC_CORE_ID);

  n = 0;
  while (1) {
    /* Reset CTX */
    ctx.num_sp = 0;
    for (q = 0; q < NUM_FLOWGRPS; q++) {
      ctx.num_proc[q] = 0;
    }

    dataplane_stats_record(PREPROC_CORE_ID, n);
    n = 0;
    n += preprocess_rx(&ctx, BATCH_SIZE);
    n += preprocess_tx(&ctx, BATCH_SIZE - n);
    n += preprocess_ac(&ctx, BATCH_SIZE - n);

    if (n == 0)
      continue;

    /* Push descriptors out to protocol workqueues */
    for (q = 0; q < NUM_FLOWGRPS; q++) {
      if (ctx.num_proc[q] == 0)
        continue;

      x = rte_ring_mp_enqueue_burst(protocol_workqueues[q], (void **) ctx.proc_work[q], ctx.num_proc[q], NULL);
      if (x < ctx.num_proc[q]) {
        DEBUG();
      }
    }

    if (ctx.num_sp > 0) {
      x = rte_ring_mp_enqueue_burst(sp_rx_ring, (void **) ctx.sp_work, ctx.num_sp, NULL);
      if (x < ctx.num_sp) {
        DEBUG();
      }
    }
  }

  return EXIT_SUCCESS;
}

