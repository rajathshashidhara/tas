#ifndef TAS_PIPELINE_H_
#define TAS_PIPELINE_H_

#include <stdint.h>

struct pkt_summary_t {
  union {
    struct {
      uint32_t seq;
      uint32_t ack;
      uint16_t flags;
      uint16_t win;
      uint32_t ts_val;
      uint32_t ts_ecr;
    } __attribute__ ((packed));
    uint32_t __raw[5];
  };
};
#endif /* TAS_PIPELINE_H_ */