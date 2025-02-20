#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_random.h>
#include <rte_ring.h>

#include "internal.h"
#include "fastemu.h"

uint64_t cnt_ring_drop = 0;
uint64_t cnt_tail_drop = 0;

static unsigned poll_rx(struct network_context *ctx,
  uint32_t ts, uint64_t tsc);
static unsigned poll_sched(struct network_context *ctx,
  uint32_t ts, uint64_t tsc);
static unsigned poll_tx(struct network_context *ctx,
  uint32_t ts, uint64_t tsc);

void network_context_destroy(struct network_context *ctx)
{
}
  
void network_loop(struct network_context *ctx)
{
  uint32_t ts;
  uint64_t cyc;

  while (!exited) {
    unsigned n = 0;

    cyc = rte_get_tsc_cycles();
    ts = qman_timestamp(cyc);

    n += poll_rx(ctx, ts, cyc);
    n += poll_sched(ctx, ts, cyc);
    n += poll_tx(ctx, ts, cyc);
  }
}

static int network_sched_packet(
  struct network_context    *ctx,
  struct network_buf_handle *bh,
  uint32_t ts)
{
  uint32_t max_ts;
  struct sched_thread *sched = &ctx->sched;
  struct pkt_ip *pi = (struct pkt_ip *) network_buf_bufoff(bh);

  if (sched->q.num >= sched->q.len)
    goto tail_drop;

  /* ecn marking */
  if (f_beui16(pi->eth.type) == ETH_TYPE_IP
        && sched->q.num >= sched->q.k_thresh
        && IPH_ECN(&pi->ip) != IP_ECN_NONE) {
    IPH_ECN_SET(&pi->ip, IP_ECN_CE);  /* mark ECN */
  }

  /* add to queue */
  sched->q.bufs[sched->q.head] = bh;
  sched->q.num++;
  if (++sched->q.head >= sched->q.len)
    sched->q.head -= sched->q.len;

  /* if queue was formerly empty, ensure it has a reasonable ts */
  if (sched->q.num == 1) {
    /* make sure queue has a reasonable next_ts:
     *  - not in the past
     *  - not more than if it just sent max_chunk at the current rate
     */
    max_ts = queue_new_ts(sched->ts_virtual, sched->q.rate, BUFFER_SIZE);
    if (timestamp_lessthaneq(sched->ts_virtual, sched->q.ts, sched->ts_virtual)) {
      sched->q.ts = sched->ts_virtual;
    } else if (!timestamp_lessthaneq(sched->ts_virtual, sched->q.ts, max_ts)) {
      sched->q.ts = max_ts;
    }
  }

  return 0;

tail_drop:
  cnt_tail_drop++;
  return -1;
}

static unsigned poll_rx(struct network_context *ctx,
  uint32_t ts, uint64_t tsc)
{
  int ret;
  unsigned i, n, k;
  struct network_buf_handle *bhs[BATCH_SIZE];
  struct network_buf_handle *free_bhs[BATCH_SIZE] = {0};

  /* receive packets */
  ret = network_poll(&ctx->net, BATCH_SIZE, bhs);
  if (ret <= 0) {
    return 0;
  }
  n = ret;

  /* prefetch packet contents (1st cache line) */
  for (i = 0; i < n; i++) {
    rte_prefetch0(network_buf_bufoff(bhs[i]));
  }

  /* Enqueue to scheduler. */
  k = 0;
  for (i = 0; i < n; i++) {
    ret = network_sched_packet(ctx, bhs[i], ts);

    if (ret != 0) {
      /* free received buffer. */
      free_bhs[k++] = bhs[i];
    }
  }

  /* Free network buffers. */
  if (k > 0)
    network_free(k, free_bhs);

  return n;
}

static unsigned poll_sched(struct network_context *ctx,
  uint32_t ts, uint64_t tsc)
{
  uint32_t max_vts, cur_ts;
  struct network_buf_handle *bhs[BATCH_SIZE];
  struct sched_thread *sched = &ctx->sched;
  unsigned n, k, pos;

  cur_ts = timestamp();
  max_vts = sched->ts_virtual + (cur_ts - sched->ts_real);

  n = 0;
  while (n < BATCH_SIZE) {
    if (sched->q.num == 0 || !timestamp_lessthaneq(sched->ts_virtual, sched->q.ts, max_vts)) {
      sched->ts_virtual = max_vts;
      break;
    }

    if (sched->q.num <= sched->q.head)
      pos = sched->q.head - sched->q.num;
    else
      pos = sched->q.len + sched->q.head - sched->q.num;

    bhs[n++] = sched->q.bufs[pos];
    sched->q.num--;

    sched->ts_virtual = sched->q.ts;
    sched->q.ts = queue_new_ts(sched->ts_virtual, sched->q.rate,
                      network_buf_len(sched->q.bufs[pos]));
  }

  /* Update virtual timestamp correctly. */
  if (n == BATCH_SIZE) {
    if (timestamp_lessthaneq(sched->ts_virtual, sched->q.ts, max_vts))
      sched->ts_virtual = sched->q.ts;
    else
      sched->ts_virtual = max_vts;
  }
  sched->ts_real = cur_ts;

  if (n == 0)
    goto exit;

  /* Post the bhs to rx_ring */
  cnt_sched += n;
  k = rte_ring_enqueue_burst(net_rx_ring, (void **) bhs, n, NULL);
  if (UNLIKELY(k < n)) {
    /* Not enough space! */
    network_free(n - k, &bhs[k]);
    cnt_ring_drop += n - k;
  }

exit:
  return n;
}

static unsigned poll_tx(struct network_context *ctx,
  uint32_t ts, uint64_t tsc)
{
  unsigned n, k;
  struct network_buf_handle *bhs[BATCH_SIZE];
 
  n = rte_ring_dequeue_burst(net_tx_ring, (void **) bhs, BATCH_SIZE, NULL);
  if (n == 0)
    return 0;

  /* Send out the packets. */
  k = network_send(&ctx->net, n, bhs);
  if (UNLIKELY(k < n)) {
    /* Free unset packets. */
    network_free(n - k, &bhs[k]);
  }

  return n;
}