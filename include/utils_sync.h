/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* spinlock implementation from DPDK (copied to remove dependencies on dpdk
 * includes everywhere) */

#ifndef UTILS_SYNC_H_
#define UTILS_SYNC_H_

#include <stdint.h>

static inline void util_spin_lock(volatile uint32_t *sl)
{
  uint32_t exp = 0;

  while (!__atomic_compare_exchange_n(sl, &exp, 1, 0,
                          __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    while (__atomic_load_n(sl, __ATOMIC_RELAXED))
      asm volatile("yield" ::: "memory");
    exp = 0;
  }
}

static inline void util_spin_unlock(volatile uint32_t *sl)
{
  __atomic_store_n(sl, 0, __ATOMIC_RELEASE);
}

static inline int util_spin_trylock(volatile uint32_t *sl)
{
  uint32_t exp = 0;
  return __atomic_compare_exchange_n(sl, &exp, 1,
                          0, /* disallow spurious failure */
                          __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
#endif /* ndef UTILS_SYNC_H_ */