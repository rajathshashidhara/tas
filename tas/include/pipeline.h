#ifndef TAS_PIPELINE_H_
#define TAS_PIPELINE_H_

#include <stdint.h>
#include <rte_config.h>
#include <rte_ring.h>

#include "utils.h"

#define NUM_SEQ_CTXS          2
#define NUM_FLOWGRPS          NUM_SEQ_CTXS
STATIC_ASSERT(NUM_FLOWGRPS <= 8, num_flowgrps);


#define BUF_FROM_PTR(WPTR)      ((void *) ((((intptr_t) (WPTR).__rawptr) << 22) >> 16))
#define BUF_TO_PTR(BUF)       ((((uintptr_t) (BUF)) >> 6) & ((0x1ull << 42) - 1))
STATIC_ASSERT(RTE_CACHE_LINE_SIZE == 64, cacheline_size);

/**
 * NBI - Network Block Interface.
 * 
 * Pipeline stage that interacts with the DPDK PMD (and consequently the NIC)
 * to receive and transmit packets.
 * 
 * - Assigns sequence numbers to packets on ingress.
 * - Reorders packets in the order of sequence numbers before egress.
 */

#define NBI_DIR_RX    0x0
#define NBI_DIR_TX    0x1

struct nbi_pkt_t {
  union {
    struct {
      uint64_t mbuf:42;       /*> Packet address      */
      uint64_t seqno:16;      /*> Sequence number     */
      uint64_t seqr:3;        /*> Sequencer context   */
      uint64_t rsvd:2;        /*> Reserved            */
      uint64_t dir:1;         /*> NBI_DIR_            */
    } __attribute__ ((packed));
    void *__rawptr;
  };
};
STATIC_ASSERT(sizeof(struct nbi_pkt_t) == sizeof(void *), nbipkt_size);

extern struct rte_ring *nbi_rx_queue;
extern struct rte_ring *nbi_tx_queue;

/******************************************************************/

enum {
  WORK_TYPE_RX   = 0,
  WORK_TYPE_TX   = 1,
  WORK_TYPE_AC   = 2,
  WORK_TYPE_RETX = 3,
};

#define  WORK_FLAG_TX                   (1 << 0)     /*> Send ACK/SEG on MAC               */
#define  WORK_FLAG_DMA_ACDESC           (1 << 1)     /*> DMA appctx descriptor to host     */
#define  WORK_FLAG_DMA_PAYLOAD          (1 << 2)     /*> DMA payload to/from host          */
#define  WORK_FLAG_FIN                  (1 << 3)     /*> Marked connection as FIN          */
#define  WORK_FLAG_QM_FORCE             (1 << 4)     /*> Force QM to schedule transmission */
#define  WORK_FLAG_IP_ECE               (1 << 5)     /*> CE notified in IP header          */
#define  WORK_FLAG_RESULT               (1 << 7)     /*> Processed work                    */

struct workptr_t {
  union {
    struct {
      uint64_t addr:42;           /*> Virtual address only uses 48 bits. Also, we use cache-aligned addresses further saving 6 bits. */
      uint64_t flow_id:17;
      uint64_t flow_grp:3;
      uint64_t type:2;
    } __attribute__ ((packed));
    uintptr_t __rawptr;
  };
};
STATIC_ASSERT(sizeof(struct workptr_t) == sizeof(uintptr_t), workptr_size);

struct work_t {
  union {
    struct {
      uint8_t  type;
      uint8_t  flags;
      uint16_t len;
      uint32_t flow_id:24;
      uint32_t flow_grp:8;
      uint32_t rx_bump;
      uint32_t tx_bump;
      uint32_t reorder_seqn;

      uint32_t seq;
      uint32_t ack;
      uint16_t tcp_flags;
      uint16_t win;
      uint32_t ts_val;
      uint32_t ts_ecr;

      uint32_t dma_pos;
      uint16_t dma_len;
      uint16_t dma_off;

      uint32_t qm_bump;
      struct rte_mbuf *mbuf;
    } __attribute__ ((packed));
    
    uint32_t __raw[16];
  };
};
STATIC_ASSERT(sizeof(struct work_t) == RTE_CACHE_LINE_SIZE, work_size);

struct dma_cmd_t {
  union {
    struct {
      uint16_t len0;
      uint16_t len1;

      uint32_t flow_id;
      uint16_t flow_grp;
      uint16_t actx_id;

      struct rte_mbuf *buf;
      struct appctx_desc_t *desc;
      
      uintptr_t src_addr0;
      uintptr_t dst_addr0;

      uintptr_t src_addr1;
      uintptr_t dst_addr1;
    } __attribute__((packed));

    uint32_t __raw[16];
  };
} __attribute__((packed));
STATIC_ASSERT(sizeof(struct dma_cmd_t) == RTE_CACHE_LINE_SIZE, dma_cmd_size);

#define SCHED_FLAG_TX_FORCE   (1 << 0)

struct sched_tx_t {
  union {
    struct {
      uint8_t  type;
      uint8_t  flags;
      uint16_t len;
      uint32_t flow_id:24;
      uint32_t flow_grp:8;
    } __attribute__((packed));
    void *__raw;
  };
} __attribute__((packed));
STATIC_ASSERT(sizeof(struct sched_tx_t) == sizeof(void *), sched_tx_size);

struct sched_bump_t {
  union {
    struct {
      uint32_t bump;
      uint32_t flow_id:24;
      uint32_t flow_grp:8;
    } __attribute__((packed));
    void *__raw;
  };
} __attribute__((packed));
STATIC_ASSERT(sizeof(struct sched_bump_t) == sizeof(void *), sched_bump_size);

#endif /* TAS_PIPELINE_H_ */