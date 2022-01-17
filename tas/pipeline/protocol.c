#include <stdint.h>
#include <stdlib.h>
#include <rte_config.h>
#include <rte_branch_prediction.h>
#include <rte_cycles.h>
#include <rte_ring.h>
#include <rte_mbuf.h>

#include "utils_reorder.h"

#include "tas_memif.h"
#include "pipeline.h"

#define TCP_MAX_RTT 100000

#define BATCH_SIZE 32
extern struct utils_reorder_buffer *rob[MAX_FLOWGRPS];
extern struct rte_ring *protocol_workqueues[NUM_FLOWGRPS];
extern struct rte_ring *postproc_workqueue;

struct protocol_thread_conf {
  uint16_t flow_grp_start;
  uint16_t nb_flow_grp;
};

//#define SKIP_ACK 1

static uint32_t tcp_txavail(struct flextcp_pl_flowst_tcp_t *fs,
                            uint32_t bump)
{
  uint32_t buf_avail, fc_avail;

  buf_avail = bump + fs->tx_avail;
  fc_avail  = fs->tx_remote_avail - fs->tx_sent;

  return MIN(buf_avail, fc_avail);
}

static int tcp_valid_rxack(struct flextcp_pl_flowst_tcp_t *fs,
                           uint32_t ack, uint32_t *bump)
{
  uint32_t next_ack, tx_sent, hole;

  next_ack = fs->tx_next_seq - fs->tx_sent;
  tx_sent = fs->tx_sent + fs->tx_avail;

  hole = ack - next_ack;
  if (hole > next_ack)
    return -1;

  *bump = hole;
  return 0;
}

static int tcp_rxseq_inwindow(struct flextcp_pl_flowst_tcp_t *fs,
                              uint32_t pkt_seq)
{
  uint32_t trim, next_seq, avail;

  next_seq = fs->rx_next_seq;
  avail = fs->rx_avail;

  trim = pkt_seq - next_seq;

  if (trim > avail)
    return -1;

  return 0;
}

static int tcp_trim_rxbuf(struct flextcp_pl_flowst_tcp_t *fs,
                uint32_t pkt_seq, uint32_t pkt_bytes,
                uint32_t *trim_start, uint32_t *trim_end)
{
  uint32_t trim, next_seq, avail;

  next_seq = fs->rx_next_seq;
  avail = fs->rx_avail;

  trim = next_seq - pkt_seq;
  if (trim <= pkt_bytes) {
    *trim_start = trim;
    trim = pkt_bytes - trim;
    *trim_end = (trim <= avail) ? 0 : (trim - avail);

    return 0;
  }

  trim = -trim;
  if (trim >= avail)
    return -1;

  *trim_start = 0;
  *trim_end = ((avail - trim) >= pkt_bytes) ? 0 : (pkt_bytes - (avail - trim));
  return 0;
}

static int tcp_valid_rxseq(struct flextcp_pl_flowst_tcp_t *fs,
                uint32_t pkt_seq, uint32_t pkt_bytes,
                uint32_t *trim_start, uint32_t *trim_end)
{
  uint32_t trim, next_seq, avail;

  next_seq = fs->rx_next_seq;
  avail = fs->rx_avail;

  trim = next_seq - pkt_seq;
  if (trim > pkt_bytes)
    return -1;

  *trim_start = trim;

  trim = pkt_bytes - trim;
  *trim_end = (trim <= avail) ? 0 : (trim - avail);
  return 0;
}

static void flows_tx(struct flextcp_pl_flowst_tcp_t *fs,
                     struct work_t *work,
                     uint32_t ts)
{
  uint8_t fin;
  uint16_t payload_len;
  uint32_t avail;

  avail = tcp_txavail(fs, 0);

  /* If no data available and is not forced */
  if (avail == 0 && work->flags == 0) {
    work->flags = 0;
    return;
  }

  payload_len = MIN(avail, TCP_MSS);
  fin = ((fs->flags & FLEXNIC_PL_FLOWST_TXFIN) != 0) && (fs->tx_avail == 0);

  work->seq = fs->tx_next_seq;
  work->ack = fs->rx_next_seq;
  work->win = MIN(0xffff, fs->rx_avail);
  work->ts_val = ts;
  work->ts_ecr = fs->tx_next_ts;
  work->tcp_flags = (fin ? TCP_FIN : 0);
  work->dma_pos = fs->tx_next_seq;
  work->len = payload_len;
  work->dma_len = payload_len - (fin ? 1 : 0);  /*> Don't send dummy byte in case of FIN */

  fs->tx_next_seq += payload_len;
  fs->tx_sent += payload_len;
  fs->tx_avail -= payload_len;

  work->flags = (WORK_FLAG_TX | WORK_FLAG_DMA_PAYLOAD);
}

static void flows_ac(struct flextcp_pl_flowst_tcp_t *fs,
                     struct work_t *work)
{
  uint32_t old_avail, new_avail;
  uint32_t rx_avail_prev, rx_avail;

  old_avail = tcp_txavail(fs, 0);
  new_avail = tcp_txavail(fs, work->tx_bump);

  if ((work->flags & FLEXTCP_PL_ATX_FLTXDONE) == FLEXTCP_PL_ATX_FLTXDONE) {
    fs->flags |= FLEXNIC_PL_FLOWST_TXFIN;
  }

  rx_avail_prev = fs->rx_avail;
  fs->rx_avail += work->rx_bump;
  fs->tx_avail += work->tx_bump;
  rx_avail = fs->rx_avail;

  work->qm_bump = (old_avail < new_avail) ? (new_avail - old_avail) : 0;

  /* receive buffer freed up from empty, need to send out a window update, if
   * we're not sending anyways. */
  if (new_avail == 0 && rx_avail_prev == 0 && rx_avail != 0) {
    work->flags = WORK_FLAG_QM_FORCE;
  }
}

static void flows_gobackN_retransmit(struct flextcp_pl_flowst_tcp_t *fs)
{
  uint32_t x;

  fs->dupack_cnt = 0;
  fs->tx_next_seq -= fs->tx_sent;
  fs->tx_avail += fs->tx_sent;
  fs->tx_remote_avail += fs->tx_sent;
  fs->tx_sent = 0;

  fs->cnt_tx_drops++;
}

static void flows_retx(struct flextcp_pl_flowst_tcp_t *fs,
                       struct work_t *work)
{
  uint32_t old_avail, new_avail;

  old_avail = tcp_txavail(fs, 0);
  flows_gobackN_retransmit(fs);
  new_avail = tcp_txavail(fs, 0);

  work->qm_bump = (old_avail < new_avail) ? (new_avail - old_avail) : 0;
}

static void flows_ack(struct flextcp_pl_flowst_tcp_t *fs,
                      struct work_t *work,
                      uint32_t ts)
{
  uint8_t flags;
  uint32_t old_avail, new_avail;
  uint32_t tx_bump;
  uint32_t rtt;

  flags = 0;
  old_avail = tcp_txavail(fs, 0);

  fs->cnt_rx_acks++;

  if (tcp_rxseq_inwindow(fs, work->seq) != 0) {
    flags |= WORK_FLAG_TX;
    goto finalize;
  }

  if (tcp_valid_rxack(fs, work->ack, &tx_bump) == 0) {
    if (tx_bump != 0) {
      if (likely(tx_bump <= fs->tx_sent)) {
        fs->tx_sent -= tx_bump; 
      }
      else {
        fs->tx_next_seq += tx_bump - fs->tx_sent;
        fs->tx_avail -= tx_bump - fs->tx_sent;
        fs->tx_sent = 0;
      }

      fs->dupack_cnt = 0;

      fs->cnt_rx_ack_bytes += tx_bump;
      if (work->tcp_flags & TCP_ECE) {
        fs->cnt_rx_ecn_bytes += tx_bump;
      }

      flags |= WORK_FLAG_DMA_ACDESC;
    }
    else {
      if (++fs->dupack_cnt >= 3) {
        /* Fast retransmit */
        flows_reset_retransmit(fs);
        goto finalize;
      }
    }
  }

  if (fs->rx_next_seq != work->seq) {
    flags |= WORK_FLAG_TX;
    goto finalize;
  }

  fs->tx_remote_avail = work->win;
  fs->tx_next_ts = work->ts_val;
  if (work->ts_ecr != 0) {
    rtt = ts - work->ts_ecr;
    if (rtt < TCP_MAX_RTT) {
      fs->rtt_est = (((fs->rtt_est << 8) - fs->rtt_est) + rtt) >> 3;        //= (fs->rtt_est * 7 + rtt) / 8
    }
  }

  if (work->tcp_flags & TCP_FIN) {
    if (((fs->flags & FLEXNIC_PL_FLOWST_RXFIN) == 0) && (fs->rx_ooo_len == 0)) {
      fs->flags |= FLEXNIC_PL_FLOWST_RXFIN;
      fs->rx_next_seq += 1;

      flags |= (WORK_FLAG_FIN | WORK_FLAG_TX | WORK_FLAG_DMA_ACDESC);
    }
  }

finalize:
  new_avail = tcp_txavail(fs, 0);
  work->qm_bump = (old_avail < new_avail) ? (new_avail - old_avail) : 0;

  work->rx_bump = 0;
  work->tx_bump = tx_bump;

  if (flags & WORK_FLAG_TX) {
    work->len = 0;
    work->seq = fs->tx_next_seq;
    work->ack = fs->rx_next_seq;
    work->win = MIN(0xffff, fs->rx_avail);
    work->tcp_flags = TCP_ACK | (((work->flags & WORK_FLAG_IP_ECE) == WORK_FLAG_IP_ECE) ? TCP_ECE : 0);
    work->ts_val = ts;
    work->ts_ecr = fs->tx_next_ts;
  }

  work->flags = flags;
}

static void flows_seg(struct flextcp_pl_flowst_tcp_t *fs,
                      struct work_t *work,
                      uint32_t ts)
{
  uint8_t  flags;
  uint32_t rx_bump, tx_bump;
  uint32_t old_avail, new_avail;
  uint32_t payload_bytes, orig_payload;
  uint32_t trim_start, trim_end;
  uint32_t seq;
  uint32_t rtt;

  tx_bump = rx_bump = 0;
#ifdef SKIP_ACK
  flags = 0;
#else
  flags = WORK_FLAG_TX;
#endif

  old_avail = tcp_txavail(fs, 0);
  payload_bytes = orig_payload = work->len;

  fs->cnt_rx_acks++;

  if (tcp_trim_rxbuf(fs, work->seq, payload_bytes, &trim_start, &trim_end) != 0) {
#ifdef SKIP_ACK
    flags |= WORK_FLAG_TX;
#endif
    goto finalize;
  }

  if (tcp_valid_rxack(fs, work->ack, &tx_bump) == 0) {
    if (tx_bump != 0) {
      if (likely(tx_bump <= fs->tx_sent)) {
        fs->tx_sent -= tx_bump;
      }
      else {
        fs->tx_next_seq += tx_bump - fs->tx_sent;
        fs->tx_avail -= tx_bump - fs->tx_sent;
        fs->tx_sent = 0;
      }

      fs->dupack_cnt = 0;

      fs->cnt_rx_ack_bytes += tx_bump;
      if (work->tcp_flags & TCP_ECE) {
        fs->cnt_rx_ecn_bytes += tx_bump;
      }

      flags |= WORK_FLAG_DMA_ACDESC;
    }
  }

  payload_bytes -= trim_start + trim_end;
  seq = work->seq + trim_start;

  if (seq != fs->rx_next_seq) {
#ifdef SKIP_ACK
    flags |= WORK_FLAG_TX;
#endif

    if (payload_bytes == 0) {
      goto finalize;
    }

    if (fs->rx_ooo_len == 0) {
      fs->rx_ooo_start = seq;
      fs->rx_ooo_len = payload_bytes;
      
      work->dma_pos = seq;
      work->dma_len = payload_bytes;
      work->dma_off = trim_start;
      flags |= WORK_FLAG_DMA_PAYLOAD;
    }
    else if (seq + payload_bytes == fs->rx_ooo_start) {
      fs->rx_ooo_start = seq;
      fs->rx_ooo_len += payload_bytes;

      work->dma_pos = seq;
      work->dma_len = payload_bytes;
      work->dma_off = trim_start;
      flags |= WORK_FLAG_DMA_PAYLOAD;
    }
    else if (fs->rx_ooo_start + fs->rx_ooo_len == seq) {
      fs->rx_ooo_len += payload_bytes;

      work->dma_pos = seq;
      work->dma_len = payload_bytes;
      work->dma_off = trim_start;
      flags |= WORK_FLAG_DMA_PAYLOAD;
    }
    else {
      /* FIXME */
    }

    goto finalize;
  }

  if (payload_bytes != 0) {
    work->dma_pos = fs->rx_next_seq;
    work->dma_len = payload_bytes;
    work->dma_off = trim_start;

    if (fs->rx_ooo_len != 0) {
      if (tcp_trim_rxbuf(fs, fs->rx_ooo_start, fs->rx_ooo_len, &trim_start, &trim_end) != 0) {
        fs->rx_ooo_len = 0;
      }
      else {
        fs->rx_ooo_start += trim_start;
        fs->rx_ooo_len   -= trim_start;
        fs->rx_ooo_len   -= trim_end;

        if (fs->rx_ooo_len > 0 && fs->rx_ooo_start == fs->rx_next_seq) {
          rx_bump += fs->rx_ooo_len;
          fs->rx_avail    -= fs->rx_ooo_len;
          fs->rx_next_seq += fs->rx_ooo_len;
          fs->rx_ooo_len   = 0;
        }
      }
    }

    work->rx_bump = rx_bump;
    flags |= (WORK_FLAG_DMA_PAYLOAD | WORK_FLAG_DMA_ACDESC);
  }

  fs->tx_remote_avail = work->win;
  fs->tx_next_ts = work->ts_val;
  if (work->ts_ecr != 0) {
    rtt = ts - work->ts_ecr;
    if (rtt < TCP_MAX_RTT) {
      fs->rtt_est = (((fs->rtt_est << 8) - fs->rtt_est) + rtt) >> 3;        //= (fs->rtt_est * 7 + rtt) / 8
    }
  }

  if (work->tcp_flags & TCP_FIN) {
    if (((fs->flags & FLEXNIC_PL_FLOWST_RXFIN) == 0) && (fs->rx_ooo_len == 0)) {
      fs->flags |= FLEXNIC_PL_FLOWST_RXFIN;
      fs->rx_next_seq += 1;

      flags |= (WORK_FLAG_FIN | WORK_FLAG_TX | WORK_FLAG_DMA_ACDESC);
    }
  }

finalize:
  new_avail = tcp_txavail(fs, 0);
  work->qm_bump = (old_avail < new_avail) ? (new_avail - old_avail) : 0;

  work->rx_bump = rx_bump;
  work->tx_bump = tx_bump;

  if (flags & WORK_FLAG_TX) {
    work->len = 0;
    work->seq = fs->tx_next_seq;
    work->ack = fs->rx_next_seq;
    work->win = MIN(0xffff, fs->rx_avail);
    work->tcp_flags = TCP_ACK | (((work->flags & WORK_FLAG_IP_ECE) == WORK_FLAG_IP_ECE) ? TCP_ECE : 0);
    work->ts_val = ts;
    work->ts_ecr = fs->tx_next_ts;
  }

  work->flags = flags;
}

static uint32_t generate_timestamp(uint64_t tsc) {
  static uint64_t freq = 0;

  if (freq == 0)
    freq = rte_get_tsc_hz();

  cycles *= 1000000ULL;
  cycles /= freq;
  return cycles;
}

static unsigned poll_reorder_queue(unsigned flow_grp,
        struct workptr_t *results, unsigned max_num,
        unsigned ts)
{
  struct workptr_t workptrs[BATCH_SIZE];
  unsigned i, num;

  struct work_t *work;
  struct flextcp_pl_flowst_tcp_t *fs;

  num = utils_reorder_drain(rob[flow_grp], (void **) workptrs, max_num);
  if (num == 0)
    return 0;

  /* Prefetch flowstate */
  for (i = 0; i < num; i++) {
    work = (struct work_t *) workptrs[i].addr;
    fs = &fp_state->flowst_tcp_state[work->flow_id];
    rte_prefetch0(fs);
  }

  /* Process RX work */
  for (i = 0; i < num; i++) {
    work = (struct work_t *) workptrs[i].addr;
    fs = &fp_state->flowst_tcp_state[work->flow_id];

    if (work->len == 0) {
      flows_ack(fs, work, ts);
    }
    else {
      flows_seg(fs, work, ts);
    }

    results[i] = workptrs[i];
  }

  return num;
}

static unsigned poll_protocol_workqueues(unsigned flow_grp,
        struct workptr_t *results, unsigned max_num,
        unsigned ts)
{
  struct workptr_t workptrs[BATCH_SIZE];
  unsigned i, num, num_enq;
  uint32_t retx_result;
  int ret;

  struct work_t *work;
  struct flextcp_pl_flowst_tcp_t *fs;

  num = rte_ring_sc_dequeue_burst(protocol_workqueues[flow_grp], (void **) workptrs, max_num);
  if (num == 0)
    return 0;

  /* Prefetch */
  for (i = 0; i < num; i++) {
    work = (struct work_t *) BUF_FROM_PTR(workptrs[i]);
    fs = &fp_state->flowst_tcp_state[workptrs[i].flow_id];

    rte_prefetch0(work);
    rte_prefetch0(fs);
  }

  /* Handle work */
  for (i = 0; i < num; i++) {
    work = (struct work_t *) BUF_FROM_PTR(workptrs[i]);
    fs = &fp_state->flowst_tcp_state[workptrs[i].flow_id];

    switch (workptrs[i].type) {
    case WORK_TYPE_RX:
      ret = utils_reorder_insert(rob[workptrs[i].flow_grp], workptrs[i], work->reorder_seqn);
      if (ret != 0) {
        results[num_enq++] = workptrs[i];   /* Add to free */
      }
      break;

    case WORK_TYPE_TX:
      flows_tx(fs, work, ts);
      results[num_enq++] = workptrs[i];
      break;
    
    case WORK_TYPE_AC:
      flows_ac(fs, work);
      results[num_enq++] = workptrs[i];
      break;

    case WORK_TYPE_RETX:
      flows_retx(fs, &retx_result);
      results[num_enq++] = workptrs[i];
      break;      
    }
  }

  return num_enq;
}

int protocol_thread(void *args)
{
  unsigned i, num, num_enq;
  unsigned fgp, fgp_x, fgp_y;
  struct protocol_thread_conf *conf = (struct protocol_thread_conf *) args;
  struct workptr_t work[BATCH_SIZE];
  struct workptr_t result[BATCH_SIZE];

  struct flextcp_pl_flowst_tcp_t *fs;
  struct work_t *work;

  uint64_t cyc;
  uint32_t ts;

  fgp_x = conf->flow_grp_start;
  fgp_y = fgp_x + conf->nb_flow_grp;
  
  fgp = fgp_x;
  while (1) {
    cyc = rte_get_tsc_cycles();
    ts = qman_timestamp(cyc);
    num = 0;

    num += poll_reorder_queue(fgp, result, BATCH_SIZE, ts);
    num += poll_protocol_workqueues(fgp, &result[num], BATCH_SIZE - num, ts);

    num_enq = rte_ring_sp_enqueue_burst(postproc_workqueue, result, num);
    if (num < num_enq) {
      /* TODO:How to handle this? */
      fprintf(stderr, "%s:%d\n", __func__, __LINE__);
      abort();
    }
  }

  return EXIT_SUCCESS;
}