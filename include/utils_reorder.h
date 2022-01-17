#ifndef UTILS_REORDER_H_
#define UTILS_REORDER_H_

#include <stdint.h>

/* A generic circular buffer */
struct cir_buffer {
	unsigned size;   /**< Number of entries that can be stored */
	unsigned mask;   /**< [buffer_size - 1]: used for wrap-around */
	unsigned head;   /**< insertion point in buffer */
	unsigned tail;   /**< extraction point in buffer */
	void **entries;
} __attribute__((aligned(64)));

/* The reorder buffer data structure itself */
struct utils_reorder_buffer {
	struct cir_buffer ready_buf; /**< temp buffer for dequeued entries */
	struct cir_buffer order_buf; /**< buffer used to reorder entries */
	unsigned memsize; /**< memory area size of reorder buffer */
	uint16_t min_seqn;  /**< Lowest seq. number that can be in the buffer */
} __attribute__((aligned(64)));

struct utils_reorder_buffer *
utils_reorder_init(
        unsigned socket_id, unsigned size);

int
utils_reorder_insert(struct utils_reorder_buffer *rob,
        void *buf, uint16_t seq);

unsigned
utils_reorder_drain(struct utils_reorder_buffer *rob,
        void **bufs, unsigned max_bufs);

#endif /* UTILS_REORDER_H_ */