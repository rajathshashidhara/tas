#ifndef TAS_PIPELINE_H_
#define TAS_PIPELINE_H_

#include <stdint.h>
#include <rte_config.h>

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

struct workptr_t {
  union {
    struct {
      uint64_t type:2;
      uint64_t flow_grp:8;
      uint64_t flags:6;           /* Flags are interface specific */
      uint64_t addr:48;
    } __attribute__ ((packed));
    uintptr_t __rawptr;
  };
};
STATIC_ASSERT(sizeof(work_ptr_t) == sizeof(uintptr_t), workptr_size);

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
    } __attribute__ ((packed));
    
    uint32_t __raw[16];
  };
};
STATIC_ASSERT(sizeof(work_t) == RTE_CACHE_LINE_SIZE, work_size);

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
STATIC_ASSERT(sizeof(dma_cmd_t) == RTE_CACHE_LINE_SIZE, dma_cmd_size);

#define SCHED_FLAG_TX_FORCE   (1 << 0)

static sched_tx_t {
  union {
    struct {
      uint8_t  type;
      uint8_t  flags;
      uint16_t len;
      uint32_t flow_id:24;
      uint32_t flow_grp:8;
    } __attribute__((packed));
    uint64_t __raw;
  };
} __attribute__((packed));
STATIC_ASSERT(sizeof(sched_tx_t) == sizeof(uintptr_t), sched_tx_size);
#endif /* TAS_PIPELINE_H_ */