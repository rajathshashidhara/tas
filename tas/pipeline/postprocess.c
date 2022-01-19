#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <rte_config.h>
#include <rte_branch_prediction.h>
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_ip.h>

#include "dma.h"
#include "utils.h"
#include "pipeline.h"
#include "packet_defs.h"
#include "tas.h"

#define BATCH_SIZE 16

struct postproc_thread_conf {

};

struct postproc_ctx {
  struct appctx_desc_t *rx_descs[BATCH_SIZE];
  unsigned rx_desc_avail;
  unsigned rx_desc_used;

  struct appctx_desc_t *tx_descs[BATCH_SIZE];
  unsigned tx_desc_freed;

  struct workptr_t dma_wptrs[BATCH_SIZE];
  unsigned num_dma;
};


static void postprocess_ac(struct postproc_ctx *ctx,
                           struct workptr_t wptr)
{
  struct work_t *work = (struct work_t *) BUF_FROM_PTR(wptr);

  /* Free AC descriptor */
  ctx->tx_descs[ctx->tx_desc_freed++] = (struct appctx_desc_t *) work;
}

static inline void tcp_checksums(struct rte_mbuf *mb, struct pkt_tcp_ts *p)
{
  p->ip.chksum = 0;
  if (config.fp_xsumoffload) {
    mb->tx_offload = 0;
    mb->l2_len = sizeof(struct eth_hdr);
    mb->l3_len = sizeof(struct ip_hdr);
    mb->l4_len = sizeof(struct pkt_tcp_ts) - offsetof(struct pkt_tcp_ts, tcp);
    mb->ol_flags = PKT_TX_IPV4 | PKT_TX_IP_CKSUM | PKT_TX_TCP_CKSUM;

    p->tcp.chksum = rte_ipv4_phdr_cksum((void *) &p->ip, mb->ol_flags);
  } else {
    p->tcp.chksum = 0;
    p->ip.chksum = rte_ipv4_cksum((void *) &p->ip);
    p->tcp.chksum = rte_ipv4_udptcp_cksum((void *) &p->ip, (void *) &p->tcp);
  }
}

static void prepare_tcp_header(struct work_t *w,
                               struct pkt_tcp_ts *p,
                               struct flextcp_pl_flowst_mem_t *conn)
{
  uint8_t tcp_flags;
  struct rte_mbuf *mb = (struct rte_mbuf *) w->mbuf;

  rte_pktmbuf_pkt_len(mb)  = sizeof(struct pkt_tcp_ts) + w->len;
  rte_pktmbuf_data_len(mb) = sizeof(struct pkt_tcp_ts) + w->len;
  p->ip.len = t_beui16(sizeof(struct pkt_tcp_ts) - offsetof(struct pkt_tcp_ts, ip) + w->len);

  tcp_flags = (w->len > 0 ? TCP_PSH : 0) | TCP_ACK | w->tcp_flags;
  p->tcp.seqno = t_beui32(w->seq + conn->seq_delta);
  p->tcp.ackno = t_beui32(w->ack + conn->ack_delta);
  TCPH_HDRLEN_FLAGS_SET(&p->tcp, (sizeof(struct pkt_tcp_ts) - offsetof(struct pkt_tcp_ts, tcp)) / 4, tcp_flags);
  p->tcp.wnd = t_beui16(w->win);
  p->tcp.chksum = 0;
  p->tcp.urgp = t_beui16(0);
  p->ts_opt.ts_val = t_beui32(w->ts_val);
  p->ts_opt.ts_ecr = t_beui32(w->ts_ecr);

  /* Setup IP/TCP checksum */
  tcp_checksums(mb, p);
}

static void prepare_tx_dma_descriptor(
                struct work_t *work,
                struct flextcp_pl_flowst_mem_t *conn)
{
  uint32_t part;
  struct rte_mbuf *mb;
  struct dma_cmd_t cmd;

  mb = (struct rte_mbuf *) work->mbuf;
  memset(&cmd, 0, sizeof(struct dma_cmd_t));

  if (likely(work->dma_pos + work->dma_len <= conn->tx_len)) {
    cmd.len0 = work->dma_len;
    cmd.src_addr0 = (uintptr_t) dma_pointer(conn->tx_base + work->dma_pos, work->dma_len);
    cmd.dst_addr0 = (uintptr_t) rte_pktmbuf_mtod_offset(mb, void *, sizeof(struct pkt_tcp_ts) + work->dma_off); 
  }
  else {
    part = conn->tx_len - work->dma_pos;
  
    cmd.len0 = part;
    cmd.src_addr0 = (uintptr_t) dma_pointer(conn->tx_base + work->dma_pos, part);
    cmd.dst_addr0 = (uintptr_t) rte_pktmbuf_mtod_offset(mb, void *, sizeof(struct pkt_tcp_ts) + work->dma_off);
    cmd.len1 = work->dma_len - part;
    cmd.src_addr1 = (uintptr_t) dma_pointer(conn->tx_base, work->dma_len - part);
    cmd.dst_addr1 = (uintptr_t) rte_pktmbuf_mtod_offset(mb, void *, sizeof(struct pkt_tcp_ts) + work->dma_off + part);
  }

  cmd.desc.__rawptr = 0;

  cmd.buf.dir = ((work->flags & WORK_FLAG_TX) != 0) ? NBI_DIR_TX : NBI_DIR_FREE;
  cmd.buf.seqno = work->reorder_seqn;
  cmd.buf.seqr = work->flow_grp;
  cmd.buf.mbuf = BUF_TO_PTR(mb);

  rte_memcpy(work, &cmd, sizeof(struct dma_cmd_t));
}

static void prepare_rx_dma_descriptor(struct work_t *work,
                struct flextcp_pl_flowst_mem_t *conn,
                struct actxptr_t descptr)
{
  uint32_t part;
  struct rte_mbuf *mb;
  struct dma_cmd_t cmd;

  mb = (struct rte_mbuf *) work->mbuf;
  memset(&cmd, 0, sizeof(struct dma_cmd_t));

  if (likely(work->dma_pos + work->dma_len <= conn->rx_len)) {
    cmd.len0 = work->dma_len;
    cmd.dst_addr0 = (uintptr_t) dma_pointer(conn->rx_base + work->dma_pos, work->dma_len);
    cmd.src_addr0 = (uintptr_t) rte_pktmbuf_mtod_offset(mb, void *, sizeof(struct pkt_tcp_ts) + work->dma_off); 
  }
  else {
    part = conn->rx_len - work->dma_pos;
  
    cmd.len0 = part;
    cmd.dst_addr0 = (uintptr_t) dma_pointer(conn->rx_base + work->dma_pos, part);
    cmd.src_addr0 = (uintptr_t) rte_pktmbuf_mtod_offset(mb, void *, sizeof(struct pkt_tcp_ts) + work->dma_off);
    cmd.len1 = work->dma_len - part;
    cmd.dst_addr1 = (uintptr_t) dma_pointer(conn->rx_base, work->dma_len - part);
    cmd.src_addr1 = (uintptr_t) rte_pktmbuf_mtod_offset(mb, void *, sizeof(struct pkt_tcp_ts) + work->dma_off + part);
  }

  cmd.desc.__rawptr = descptr.__rawptr;

  cmd.buf.dir = ((work->flags & WORK_FLAG_TX) != 0) ? NBI_DIR_TX : NBI_DIR_FREE;
  cmd.buf.seqno = work->reorder_seqn;
  cmd.buf.seqr = work->flow_grp;
  cmd.buf.mbuf = BUF_TO_PTR(mb);

  rte_memcpy(work, &cmd, sizeof(struct dma_cmd_t));
}

static void postprocess_tx(struct postproc_ctx *ctx,
                           struct workptr_t wptr)
{
  struct work_t *work = (struct work_t *) BUF_FROM_PTR(wptr);
  struct rte_mbuf *mb = (struct rte_mbuf *) work->mbuf;
  struct flextcp_pl_flowst_mem_t *conn = &fp_state->flows_mem_info[wptr.flow_id];
  struct pkt_tcp_ts *pkt = rte_pktmbuf_mtod(mb, struct pkt_tcp_ts *);

  /* Prepare TCP header */
  if (work->flags & WORK_FLAG_TX) {
    prepare_tcp_header(work, pkt, conn);
  }

  /* Prepare DMA descriptor */
  prepare_tx_dma_descriptor(work, conn);

  /* Forward to DMA engine */
  ctx->dma_wptrs[ctx->num_dma++] = wptr;
}

static struct actxptr_t prepare_arx_desc(struct work_t *work,
                             struct flextcp_pl_flowst_mem_t *conn,
                             struct appctx_desc_t *desc)
{
  struct actxptr_t descptr;

  desc->arx.type = FLEXTCP_PL_ARX_INVALID;  /*> Set as invalid to ensure that descriptor is not partially visible. */
  desc->arx.msg.connupdate.rx_bump = work->rx_bump;
  desc->arx.msg.connupdate.tx_bump = work->tx_bump;
  desc->arx.msg.connupdate.flags = (work->flags & WORK_FLAG_FIN);
  desc->arx.msg.connupdate.db_id = conn->db_id;
  desc->arx.msg.connupdate.opaque = conn->opaque;

  descptr.__rawptr = 0;
  descptr.db_id = conn->db_id;
  descptr.dir = ACTX_DIR_RX;
  descptr.seqno = 0;  /*> FIXME */
  descptr.desc = BUF_TO_PTR(desc);

  return descptr;
}

static void postprocess_rx(struct postproc_ctx *ctx,
                           struct workptr_t wptr)
{
  struct work_t *work = (struct work_t *) BUF_FROM_PTR(wptr);
  struct rte_mbuf *mb = (struct rte_mbuf *) work->mbuf;
  struct flextcp_pl_flowst_mem_t *conn = &fp_state->flows_mem_info[wptr.flow_id];
  struct pkt_tcp_ts *pkt = rte_pktmbuf_mtod(mb, struct pkt_tcp_ts *);
  struct actxptr_t descptr;

  descptr.__rawptr = 0;

  /* Prepare TCP header */
  if (work->flags & WORK_FLAG_TX) {
    prepare_tcp_header(work, pkt, conn);
  }

  /* Prepare ARX descriptor */
  if (work->flags & WORK_FLAG_DMA_ACDESC) {
    descptr = prepare_arx_desc(work, conn, ctx->rx_descs[ctx->rx_desc_used++]);
  }

  /* Prepare DMA descriptor */
  prepare_rx_dma_descriptor(work, conn, descptr);

  /* Forward to DMA engine */
  ctx->dma_wptrs[ctx->num_dma++] = wptr;
}

static void postproc_thread_init(struct postproc_thread_conf *conf)
{
  (void) conf;

  // TODO: ?
}

static void prefetch_buffers(struct workptr_t wptr)
{
  struct work_t *work;
  struct rte_mbuf *mb;
  struct flextcp_pl_flowst_mem_t *conn;
  struct pkt_tcp_ts *pkt;

  switch (wptr.type) {
  case WORK_TYPE_RX:
  case WORK_TYPE_TX:
    work = (struct work_t *) RTE_PTR_ADD(BUF_FROM_PTR(wptr), sizeof(struct rte_mbuf));
    mb = (struct rte_mbuf *) BUF_FROM_PTR(wptr);
    conn = &fp_state->flows_mem_info[wptr.flow_id];
    pkt = (struct pkt_tcp_ts *) RTE_PTR_ADD(BUF_FROM_PTR(wptr), sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM);
    rte_prefetch0(work);
    rte_mbuf_prefetch_part1(mb);
    rte_mbuf_prefetch_part2(mb);
    rte_prefetch0(conn);
    rte_prefetch0(pkt);
    break;
  
  case WORK_TYPE_AC:
    work = (struct work_t *) BUF_FROM_PTR(wptr);
    conn = &fp_state->flows_mem_info[wptr.flow_id];
    rte_prefetch0(work);
    rte_prefetch0(conn);
    break;
  
  default:
    fprintf(stderr, "%s:%d\n", __func__, __LINE__);
    abort();
  }
}

static void postprocess(struct postproc_ctx *ctx, struct workptr_t wptr)
{
  switch (wptr.type) {
  case WORK_TYPE_RX:
    postprocess_rx(ctx, wptr);
    break;
  
  case WORK_FLAG_TX:
    postprocess_tx(ctx, wptr);
    break;
  
  case WORK_TYPE_AC:
    postprocess_ac(ctx, wptr);
    break;
  
  default:
    fprintf(stderr, "%s:%d\n", __func__, __LINE__);
    abort();
  }
}

static void move_to_front(void **arr, unsigned s, unsigned n)
{
  unsigned i;

  i = 0;
  while (s < n) {
    arr[i++] = arr[s++];
  }
}

int postproc_thread(void *args)
{
  int ret;
  unsigned i, num, num_enq;
  struct postproc_ctx ctx;
  struct postproc_thread_conf *conf = (struct postproc_thread_conf *) args;
  struct workptr_t wptr[BATCH_SIZE];

  postproc_thread_init(conf);

  memset(&ctx, 0, sizeof(struct postproc_ctx));

  while (1) {
    /* Replenish ARX descriptors */
    if (ctx.rx_desc_avail < BATCH_SIZE) {
      ret = rte_mempool_get_bulk(arx_desc_pool, (void **) &ctx.rx_descs[ctx.rx_desc_avail], BATCH_SIZE - ctx.rx_desc_avail);
      if (ret < 0) {
        fprintf(stderr, "%s:%d\n", __func__, __LINE__);
        abort();
      }
      ctx.rx_desc_avail = BATCH_SIZE;
    }

    num = rte_ring_mc_dequeue_burst(postproc_workqueue, (void **) wptr, BATCH_SIZE, NULL);
    if (num == 0)
      continue;

    /* Issue prefetch */
    for (i = 0; i < num; i++) {
      prefetch_buffers(wptr[i]);
    }

    ctx.rx_desc_used = 0;
    ctx.tx_desc_freed = 0;
    ctx.num_dma = 0;
    for (i = 0; i < num; i++) {
      postprocess(&ctx, wptr[i]);
    }

    /* Return ATX descriptors to pool */
    if (ctx.tx_desc_freed > 0) {
      rte_mempool_put_bulk(atx_desc_pool, (void **) ctx.tx_descs, ctx.tx_desc_freed);
    }

    /* Forward to DMA engine */
    if (ctx.num_dma > 0) {
      num_enq = rte_ring_mp_enqueue_burst(dma_cmd_ring, (void **) ctx.dma_wptrs, ctx.num_dma, NULL);
      if (num_enq < ctx.num_dma) {
        fprintf(stderr, "%s:%d\n", __func__, __LINE__);
        abort();
      }
    }

    /* Move unused arx descriptors forward */
    move_to_front((void **) ctx.rx_descs, ctx.rx_desc_used, ctx.rx_desc_avail);
    ctx.rx_desc_avail -= ctx.rx_desc_used;
  }

  return EXIT_SUCCESS;
}
