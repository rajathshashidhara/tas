#ifndef UTILS_REORDER_H_
#define UTILS_REORDER_H_

#include <stdint.h>

/* A generic circular buffer */
struct cir_buffer {
	unsigned int size;   /**< Number of entries that can be stored */
	unsigned int mask;   /**< [buffer_size - 1]: used for wrap-around */
	unsigned int head;   /**< insertion point in buffer */
	unsigned int tail;   /**< extraction point in buffer */
	void **entries;
} __attribute__((aligned(64)));

/* The reorder buffer data structure itself */
struct utils_reorder_buffer {
	uint32_t min_seqn;  /**< Lowest seq. number that can be in the buffer */
	unsigned int memsize; /**< memory area size of reorder buffer */
	struct cir_buffer ready_buf; /**< temp buffer for dequeued entries */
	struct cir_buffer order_buf; /**< buffer used to reorder entries */
} __attribute__((aligned(64)));

struct utils_reorder_buffer *
utils_reorder_init(
        unsigned socket_id, unsigned int size);

int
utils_reorder_insert(struct utils_reorder_buffer *rob,
        void *buf, uint32_t seq);

unsigned int 
utils_reorder_drain(struct utils_reorder_buffer *rob,
        void **bufs, unsigned int max_bufs);

#endif /* UTILS_REORDER_H_ */