#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_branch_prediction.h>
#include <rte_ring.h>

#include "utils.h"
#include "utils_rng.h"
#include "tas_memif.h"
#include "pipeline.h"
#include "tas.h"


#define BATCH_SIZE    32
#define TCP_MSS       1448

#define FLAG_INSKIPLIST 1
#define FLAG_INNOLIMITL 2

/** Skiplist: bits per level */
#define SKIPLIST_BITS 3
/** Index list: invalid index */
#define IDXLIST_INVAL (-1U)

#define RNG_SEED 0x12345678
#define TIMESTAMP_BITS 32
#define TIMESTAMP_MASK 0xFFFFFFFF

/** Skiplist: #levels */
#define QMAN_SKIPLIST_LEVELS 4

/** Queue state */
struct queue {
  /** Assigned Rate */
  uint32_t rate;
  /** Number of entries in queue */
  uint32_t avail;
  /** Time stamp */
  uint32_t next_ts;
  /** Maximum chunk size when de-queueing */
  uint16_t max_chunk;
  /** Flags: FLAG_INSKIPLIST, FLAG_INNOLIMITL */
  uint16_t flags;
  /** Next pointers for levels in skip list */
  uint32_t next_idxs[QMAN_SKIPLIST_LEVELS];
} __attribute__((packed));
STATIC_ASSERT(sizeof(struct queue) == sizeof(struct flextcp_pl_flowst_cc_t), queue_size);
STATIC_ASSERT(offsetof(struct queue, rate) == offsetof(struct flextcp_pl_flowst_cc_t, tx_rate), queue_rate_offset);
STATIC_ASSERT(offsetof(struct queue, avail) == offsetof(struct flextcp_pl_flowst_cc_t, tx_avail), queue_avail_offset);

struct sched_ctx {
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

static unsigned qman_poll(struct sched_ctx *ctx, unsigned num);
static void qman_set(struct sched_ctx *ctx, uint32_t flow_id, uint32_t avail);

/** Add queue to the no limit list */
static inline void queue_activate_nolimit(struct sched_ctx *ctx, uint32_t flow_id);
static inline unsigned poll_nolimit(struct sched_ctx *ctx, uint32_t cur_ts,
    unsigned num, struct sched_tx_t *tx_events);

/** Add queue to the skip list list */
static inline void queue_activate_skiplist(struct sched_ctx *ctx, uint32_t flow_id);
static inline unsigned poll_skiplist(struct sched_ctx *ctx, uint32_t cur_ts,
    unsigned num, struct sched_tx_t *tx_events);
static inline uint8_t queue_level(struct sched_ctx *ctx);

static inline struct sched_tx_t queue_fire(struct sched_ctx *ctx, uint32_t flow_id);
static inline void queue_activate(struct sched_ctx *t, uint32_t flow_id);
static inline uint32_t timestamp(void);
static inline int timestamp_lessthaneq(struct sched_ctx *t, uint32_t a,
    uint32_t b);
static inline int64_t rel_time(uint32_t cur_ts, uint32_t ts_in);

static inline uint32_t queue_new_ts(struct sched_ctx *ctx, struct queue *q,
    uint32_t bytes)
{
  return ctx->ts_virtual + ((uint64_t) bytes * 8 * 1000000) / q->rate;
}

static inline struct sched_tx_t queue_fire(struct sched_ctx *ctx, uint32_t flow_id)
{
  uint32_t bytes;
  struct queue *q = &ctx->queues[flow_id];
  struct sched_tx_t tx_event;

  assert(q->avail > 0);

  bytes = (q->avail <= TCP_MSS ? q->avail : TCP_MSS);
  q->avail -= bytes;

  if (q->rate > 0) {
    q->next_ts = queue_new_ts(ctx, q, bytes);
  }

  if (q->avail > 0) {
    queue_activate(ctx, flow_id);
  }

  tx_event.__raw = NULL;
  tx_event.len = bytes;
  tx_event.flow_id = flow_id;
  // TODO: Set flow_grp
  return tx_event;
}

/** Poll skiplist queues */
static inline unsigned poll_skiplist(struct sched_ctx *ctx, uint32_t cur_ts,
    unsigned num, struct sched_tx_t *tx_events)
{
  unsigned cnt;
  uint32_t idx, max_vts;
  int8_t l;
  struct queue *q;

  /* maximum virtual time stamp that can be reached */
  max_vts = ctx->ts_virtual + (cur_ts - ctx->ts_real);

  for (cnt = 0; cnt < num;) {
    idx = ctx->head_idx[0];

    /* no more queues */
    if (idx == IDXLIST_INVAL) {
      ctx->ts_virtual = max_vts;
      break;
    }

    q = &ctx->queues[idx];

    /* beyond max_vts */
    if (!timestamp_lessthaneq(ctx, q->next_ts, max_vts)) {
      ctx->ts_virtual = max_vts;
      break;
    }

    /* remove queue from skiplist */
    for (l = 0; l < QMAN_SKIPLIST_LEVELS && ctx->head_idx[l] == idx; l++) {
      ctx->head_idx[l] = q->next_idxs[l];
    }
    assert((q->flags & FLAG_INSKIPLIST) != 0);
    q->flags &= ~FLAG_INSKIPLIST;

    /* advance virtual timestamp */
    ctx->ts_virtual = q->next_ts;

    if (q->avail > 0) {
      tx_events[cnt++] = queue_fire(ctx, idx);
    }
  }

  /* if we reached the limit, update the virtual timestamp correctly */
  if (cnt == num) {
    idx = ctx->head_idx[0];
    if (idx != IDXLIST_INVAL &&
        timestamp_lessthaneq(ctx, ctx->queues[idx].next_ts, max_vts))
    {
      ctx->ts_virtual = ctx->queues[idx].next_ts;
    } else {
      ctx->ts_virtual = max_vts;
    }
  }

  ctx->ts_real = cur_ts;
  return cnt;
}

static inline uint8_t queue_level(struct sched_ctx *ctx)
{
  uint8_t x = (__builtin_ffs(utils_rng_gen32(&ctx->rng)) - 1) / SKIPLIST_BITS;
  return (x < QMAN_SKIPLIST_LEVELS ? x : QMAN_SKIPLIST_LEVELS - 1);
}

/** Add queue to the skip list list */
static inline void queue_activate_skiplist(struct sched_ctx *ctx, uint32_t q_idx)
{
  uint8_t level;
  int8_t l;
  uint32_t preds[QMAN_SKIPLIST_LEVELS];
  uint32_t pred, idx, ts, max_ts;
  struct queue *q = &ctx->queues[q_idx];

  assert((q->flags & (FLAG_INSKIPLIST | FLAG_INNOLIMITL)) == 0);


  /* make sure queue has a reasonable next_ts:
   *  - not in the past
   *  - not more than if it just sent max_chunk at the current rate
   */
  ts = q->next_ts;
  max_ts = queue_new_ts(ctx, q, TCP_MSS);
  if (timestamp_lessthaneq(ctx, ts, ctx->ts_virtual)) {
    ts = q->next_ts = ctx->ts_virtual;
  } else if (!timestamp_lessthaneq(ctx, ts, max_ts)) {
    ts = q->next_ts = max_ts;
  }
  q->next_ts = ts;

  /* find predecessors at all levels top-down */
  pred = IDXLIST_INVAL;
  for (l = QMAN_SKIPLIST_LEVELS - 1; l >= 0; l--) {
    idx = (pred != IDXLIST_INVAL ? pred : ctx->head_idx[l]);
    while (idx != IDXLIST_INVAL &&
        timestamp_lessthaneq(ctx, ctx->queues[idx].next_ts, ts))
    {
      pred = idx;
      idx = ctx->queues[idx].next_idxs[l];
    }
    preds[l] = pred;
  }

  /* determine level for this queue */
  level = queue_level(ctx);

  /* insert into skip-list */
  for (l = QMAN_SKIPLIST_LEVELS - 1; l >= 0; l--) {
    if (l > level) {
      q->next_idxs[l] = IDXLIST_INVAL;
    } else {
      idx = preds[l];
      if (idx != IDXLIST_INVAL) {
        q->next_idxs[l] = ctx->queues[idx].next_idxs[l];
        ctx->queues[idx].next_idxs[l] = q_idx;
      } else {
        q->next_idxs[l] = ctx->head_idx[l];
        ctx->head_idx[l] = q_idx;
      }
    }
  }

  q->flags |= FLAG_INSKIPLIST;
}

static inline void queue_activate_nolimit(struct sched_ctx *ctx, uint32_t flow_id)
{
  struct queue *q_tail;
  struct queue *q = &ctx->queues[flow_id];

  assert((q->flags & (FLAG_INSKIPLIST | FLAG_INNOLIMITL)) == 0);

  q->flags |= FLAG_INNOLIMITL;
  q->next_idxs[0] = IDXLIST_INVAL;
  if (ctx->nolimit_tail_idx == IDXLIST_INVAL) {
    ctx->nolimit_head_idx = ctx->nolimit_tail_idx = flow_id;
    return;
  }

  q_tail = &ctx->queues[ctx->nolimit_tail_idx];
  q_tail->next_idxs[0] = flow_id;
  ctx->nolimit_tail_idx = flow_id;
}

static inline unsigned poll_nolimit(struct sched_ctx *ctx, uint32_t cur_ts,
    unsigned num, struct sched_tx_t *tx_events)
{
  unsigned cnt;
  struct queue *q;
  uint32_t idx;

  for (cnt = 0; cnt < num && ctx->nolimit_head_idx != IDXLIST_INVAL;) {
    idx = ctx->nolimit_head_idx;
    q = &ctx->queues[idx];

    ctx->nolimit_head_idx = q->next_idxs[0];
    if (q->next_idxs[0] == IDXLIST_INVAL)
      ctx->nolimit_tail_idx = IDXLIST_INVAL;

    q->flags &= ~FLAG_INNOLIMITL;
    if (q->avail > 0) {
      tx_events[cnt++] = queue_fire(ctx, idx);
    }
  }

  return cnt;
}

static inline void queue_activate(struct sched_ctx *ctx, uint32_t flow_id)
{
  struct queue *q = &ctx->queues[flow_id];

  if (q->rate == 0) {
    queue_activate_nolimit(ctx, flow_id);
  }
  else {
    queue_activate_skiplist(ctx, flow_id);
  }
}

static inline void qman_set(struct sched_ctx *ctx, uint32_t flow_id, uint32_t avail)
{
  struct queue *q = &ctx->queues[flow_id];

  q->avail += avail;

  if (q->avail > 0 && ((q->flags & (FLAG_INSKIPLIST | FLAG_INNOLIMITL)) == 0)) {
    queue_activate(ctx, flow_id);
  }
}

static unsigned qman_bump(struct sched_ctx *ctx, unsigned num)
{
  struct sched_bump_t bump_events[BATCH_SIZE];
  unsigned i, cnt;

  cnt = rte_ring_sc_dequeue_burst(sched_bump_queue, (void **) bump_events, num, NULL);
  if (cnt == 0)
    return 0;

  /* Prefetch state */
  for (i = 0; i < cnt; i++) {
    rte_prefetch0(&ctx->queues[bump_events[i].flow_id]);
  }

  /* Qman set */
  for (i = 0; i < cnt; i++) {
    qman_set(ctx, bump_events[i].flow_id, bump_events[i].bump);
  }

  return cnt;
}

static unsigned qman_poll(struct sched_ctx *ctx, unsigned num)
{
  struct sched_tx_t tx_events[BATCH_SIZE];
  unsigned enq_cnt, tx_cnt;
  
  uint32_t ts = timestamp();

  tx_cnt = 0;
  /* poll nolimit list and skiplist alternating the order between */
  if (ctx->nolimit_first) {
    tx_cnt += poll_nolimit(ctx, ts, num, tx_events);
    tx_cnt += poll_skiplist(ctx, ts, num - tx_cnt, tx_events + tx_cnt);
  } else {
    tx_cnt += poll_skiplist(ctx, ts, num, tx_events);
    tx_cnt += poll_nolimit(ctx, ts, num - tx_cnt, tx_events + tx_cnt);
  }
  ctx->nolimit_first = !ctx->nolimit_first;

  if (tx_cnt == 0)
    return 0;

  enq_cnt = rte_ring_sp_enqueue_burst(sched_tx_queue, (void **) tx_events, tx_cnt, NULL);
  if (enq_cnt < tx_cnt) {
    fprintf(stderr, "%s:%d\n", __func__, __LINE__);
    abort();
  }

  return tx_cnt;
}

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

static inline int timestamp_lessthaneq(struct sched_ctx *ctx, uint32_t a, uint32_t b)
{
  return rel_time(ctx->ts_virtual, a) <= rel_time(ctx->ts_virtual, b);
}

static void sched_thread_init(struct sched_ctx *ctx)
{
  unsigned i;

  ctx->queues = (void *) &fp_state->flows_cc_info[0];
  for (i = 0; i < QMAN_SKIPLIST_LEVELS; i++) {
    ctx->head_idx[i] = IDXLIST_INVAL;
  }
  ctx->nolimit_head_idx = ctx->nolimit_tail_idx = IDXLIST_INVAL;
  utils_rng_init(&ctx->rng, RNG_SEED * 1 + 1);  // FIXME

  ctx->ts_virtual = 0;
  ctx->ts_real = timestamp();
}

int scheduler_thread(void *args)
{
  unsigned x, y;
  struct sched_ctx ctx;

  (void) args;

  sched_thread_init(&ctx);
  dataplane_stats_coreinit(SCHED_CORE_ID);

  while (1) {
    x = qman_bump(&ctx, BATCH_SIZE);
    dataplane_stats_record(SCHED_CORE_ID, x);
    y = qman_poll(&ctx, BATCH_SIZE);
    dataplane_stats_record(SCHED_CORE_ID, y);
  }

  return EXIT_SUCCESS;
}
