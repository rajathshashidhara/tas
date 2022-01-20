#include <stdlib.h>
#include <string.h>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_malloc.h>

#include <utils_reorder.h>

struct utils_reorder_buffer *
utils_reorder_init(
        unsigned socket_id, unsigned size)
{
  struct utils_reorder_buffer *rob = NULL;
  const unsigned bufsize = sizeof(struct utils_reorder_buffer)
          + (2 * size * sizeof(void *));

  if (!rte_is_power_of_2(size)) {
    fprintf(stderr, "%s %d\n", __func__, __LINE__);
    return NULL;
  }

  rob = rte_zmalloc_socket(NULL, bufsize, 0, socket_id);
  if (rob == NULL) {
    return NULL;
  }

  memset(rob, 0, bufsize);
  rob->memsize = bufsize;
  rob->order_buf.size = rob->ready_buf.size = size;
  rob->order_buf.mask = rob->ready_buf.mask = size - 1;
  rob->ready_buf.entries = (void *) &rob[1];
  rob->order_buf.entries = RTE_PTR_ADD(&rob[1],
			size * sizeof(rob->ready_buf.entries[0]));
  rob->min_seqn = 0;

  return rob;
}

static unsigned
reorder_fill_overflow(struct utils_reorder_buffer *b, unsigned n)
{
	/*
	 * 1. Move all ready entries that fit to the ready_buf
	 * 2. check if we meet the minimum needed (n).
	 * 3. If not, then skip any gaps and keep moving.
	 * 4. If at any point the ready buffer is full, stop
	 * 5. Return the number of positions the order_buf head has moved
	 */

	struct cir_buffer *order_buf = &b->order_buf,
			*ready_buf = &b->ready_buf;

	unsigned order_head_adv = 0;

	/*
	 * move at least n packets to ready buffer, assuming ready buffer
	 * has room for those packets.
	 */
	while (order_head_adv < n &&
			((ready_buf->head + 1) & ready_buf->mask) != ready_buf->tail) {

		/* if we are blocked waiting on a packet, skip it */
		if (order_buf->entries[order_buf->head] == NULL) {
			order_buf->head = (order_buf->head + 1) & order_buf->mask;
			order_head_adv++;
		}

		/* Move all ready entries that fit to the ready_buf */
		while (order_buf->entries[order_buf->head] != NULL) {
			ready_buf->entries[ready_buf->head] =
					order_buf->entries[order_buf->head];

			order_buf->entries[order_buf->head] = NULL;
			order_head_adv++;

			order_buf->head = (order_buf->head + 1) & order_buf->mask;

			if (((ready_buf->head + 1) & ready_buf->mask) == ready_buf->tail)
				break;

			ready_buf->head = (ready_buf->head + 1) & ready_buf->mask;
		}
	}

	b->min_seqn += order_head_adv;
	/* Return the number of positions the order_buf head has moved */
	return order_head_adv;
}

int
utils_reorder_insert(
      struct utils_reorder_buffer *b, void *buf, uint16_t seq)
{
	uint16_t offset, position;
	struct cir_buffer *order_buf;

	if (b == NULL || buf == NULL) {
		return -EINVAL;
	}

  order_buf = &b->order_buf;

	/*
	 * calculate the offset from the head pointer we need to go.
	 * The subtraction takes care of the sequence number wrapping.
	 * For example (using 16-bit for brevity):
	 *	min_seqn  = 0xFFFD
	 *	mbuf_seqn = 0x0010
	 *	offset    = 0x0010 - 0xFFFD = 0x13
	 */
	offset = seq - b->min_seqn;

	/*
	 * action to take depends on offset.
	 * offset < buffer->size: the mbuf fits within the current window of
	 *    sequence numbers we can reorder. EXPECTED CASE.
	 * offset > buffer->size: the mbuf is outside the current window. There
	 *    are a number of cases to consider:
	 *    1. The packet sequence is just outside the window, then we need
	 *       to see about shifting the head pointer and taking any ready
	 *       to return packets out of the ring. If there was a delayed
	 *       or dropped packet preventing drains from shifting the window
	 *       this case will skip over the dropped packet instead, and any
	 *       packets dequeued here will be returned on the next drain call.
	 *    2. The packet sequence number is vastly outside our window, taken
	 *       here as having offset greater than twice the buffer size. In
	 *       this case, the packet is probably an old or late packet that
	 *       was previously skipped, so just enqueue the packet for
	 *       immediate return on the next drain call, or else return error.
	 */
	if (offset < b->order_buf.size) {
		position = (order_buf->head + offset) & order_buf->mask;
		order_buf->entries[position] = buf;
	} else if (offset < 2 * b->order_buf.size) {
		if (reorder_fill_overflow(b, offset + 1 - order_buf->size)
				< (offset + 1 - order_buf->size)) {
			/* Put in handling for enqueue straight to output */
			return -ENOSPC;
		}
		offset = seq - b->min_seqn;
		position = (order_buf->head + offset) & order_buf->mask;
		order_buf->entries[position] = buf;
	} else {
		return -ERANGE;
	}
	return 0;
}

unsigned
utils_reorder_drain(struct utils_reorder_buffer *b, void **bufs,
		unsigned max_bufs)
{
  unsigned drain_cnt = 0;

	struct cir_buffer *order_buf = &b->order_buf,
			*ready_buf = &b->ready_buf;

	/* Try to fetch requested number of mbufs from ready buffer */
	while ((drain_cnt < max_bufs) && (ready_buf->tail != ready_buf->head)) {
		bufs[drain_cnt++] = ready_buf->entries[ready_buf->tail];
		ready_buf->tail = (ready_buf->tail + 1) & ready_buf->mask;
	}

	/*
	 * If requested number of buffers not fetched from ready buffer, fetch
	 * remaining buffers from order buffer
	 */
	while ((drain_cnt < max_bufs) &&
			(order_buf->entries[order_buf->head] != NULL)) {
		bufs[drain_cnt++] = order_buf->entries[order_buf->head];
		order_buf->entries[order_buf->head] = NULL;
		b->min_seqn++;
		order_buf->head = (order_buf->head + 1) & order_buf->mask;
	}

	return drain_cnt;
}
