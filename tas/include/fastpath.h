/*
 * Copyright 2019 University of Washington, Max Planck Institute for
 * Software Systems, and The University of Texas at Austin
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef FASTPATH_H_
#define FASTPATH_H_

#include <stdbool.h>
#include <stdint.h>

#include <rte_interrupts.h>
#include <rte_ring.h>

#include <tas_memif.h>
#include <utils_rng.h>

#define BATCH_SIZE 64
#define BUFCACHE_SIZE 1024
#define TXBUF_SIZE (2 * BATCH_SIZE)

extern volatile uint32_t start_done;

extern struct rte_ring *net_rx_ring;
extern struct rte_ring *net_tx_ring;

struct network_thread {
  struct rte_mempool *pool;
  uint16_t queue_id;
};

/** Skiplist: #levels */
#define QMAN_SKIPLIST_LEVELS 4

struct qman_thread {
  /************************************/
  /* read-only */
  struct queue *queues;

  /************************************/
  /* modified by owner thread */
  uint32_t head_idx[QMAN_SKIPLIST_LEVELS];
  uint32_t nolimit_head_idx;
  uint32_t nolimit_tail_idx;
  uint32_t ts_real;
  uint32_t ts_virtual;
  struct utils_rng rng;
  bool nolimit_first;
};

struct sched_thread {
  struct {
    struct network_buf_handle **bufs;
    uint32_t head;
    uint32_t num;
    uint32_t len;

    uint32_t ts;
    uint32_t rate;
    uint32_t k_thresh;
  } q;

  struct utils_rng rng;
  uint32_t ts_virtual;
  uint32_t ts_real;
};

struct network_context {
  struct network_thread net;
  struct sched_thread sched;
  uint16_t id;
};

struct dataplane_context {
  struct network_thread net;
  struct qman_thread qman;
  struct rte_ring *qman_fwd_ring;
  uint16_t id;
  int evfd;
  struct rte_epoll_event ev;

  /********************************************************/
  /* arx cache */
  struct flextcp_pl_arx arx_cache[BATCH_SIZE];
  uint16_t arx_ctx[BATCH_SIZE];
  uint16_t arx_num;

  /********************************************************/
  /* send buffer */
  struct network_buf_handle *tx_handles[TXBUF_SIZE];
  uint16_t tx_num;

  /********************************************************/
  /* polling queues */
  uint32_t poll_next_ctx;

  /********************************************************/
  /* pre-allocated buffers for polling doorbells and queue manager */
  struct network_buf_handle *bufcache_handles[BUFCACHE_SIZE];
  uint16_t bufcache_num;
  uint16_t bufcache_head;

  uint64_t loadmon_cyc_busy;

  uint64_t kernel_drop;
#ifdef DATAPLANE_STATS
  /********************************************************/
  /* Stats */
  uint64_t stat_qm_poll;
  uint64_t stat_qm_empty;
  uint64_t stat_qm_total;

  uint64_t stat_rx_poll;
  uint64_t stat_rx_empty;
  uint64_t stat_rx_total;

  uint64_t stat_qs_poll;
  uint64_t stat_qs_empty;
  uint64_t stat_qs_total;

  uint64_t stat_cyc_db;
  uint64_t stat_cyc_qm;
  uint64_t stat_cyc_rx;
  uint64_t stat_cyc_qs;
#endif
};

extern struct dataplane_context **ctxs;

int dataplane_init(void);
int dataplane_context_init(struct dataplane_context *ctx);
void dataplane_context_destroy(struct dataplane_context *ctx);
void dataplane_loop(struct dataplane_context *ctx);
#ifdef DATAPLANE_STATS
void dataplane_dump_stats(void);
#endif
int network_thread_init(struct network_context *ctx);


#define TIMESTAMP_BITS 32
#define TIMESTAMP_MASK 0xFFFFFFFF

static inline uint32_t timestamp(void)
{
  static uint64_t freq = 0;
  uint64_t cycles = rte_get_tsc_cycles();

  if (freq == 0)
    freq = rte_get_tsc_hz();

  cycles *= 1000000000ULL;
  cycles /= freq;
  return cycles;
}

static inline uint32_t queue_new_ts(uint32_t ts_virtual, uint32_t rate,
    uint32_t bytes)
{
  return ts_virtual + ((uint64_t) bytes * 8 * 1000000) / rate;
}

/** Relative timestamp, ignoring wrap-arounds */
static inline int64_t rel_time(uint32_t cur_ts, uint32_t ts_in)
{
  uint64_t ts = ts_in;
  const uint64_t middle = (1ULL << (TIMESTAMP_BITS - 1));
  uint64_t start, end;

  if (cur_ts < middle) {
    /* negative interval is split in half */
    start = (cur_ts - middle) & TIMESTAMP_MASK;
    end = (1ULL << TIMESTAMP_BITS);
    if (start <= ts && ts < end) {
      /* in first half of negative interval, smallest timestamps */
      return ts - start - middle;
    } else {
      /* in second half or in positive interval */
      return ts - cur_ts;
    }
  } else if (cur_ts == middle) {
    /* intervals not split */
    return ts - cur_ts;
  } else {
    /* higher interval is split */
    start = 0;
    end = ((cur_ts + middle) & TIMESTAMP_MASK) + 1;
    if (start <= cur_ts && ts < end) {
      /* in second half of positive interval, largest timestamps */
      return ts + ((1ULL << TIMESTAMP_BITS) - cur_ts);
    } else {
      /* in negative interval or first half of positive interval */
      return ts - cur_ts;
    }
  }
}

static inline int timestamp_lessthaneq(uint32_t ts_virtual, uint32_t a,
    uint32_t b)
{
  return rel_time(ts_virtual, a) <= rel_time(ts_virtual, b);
}

#endif /* ndef FASTPATH_H_ */
