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

#include <stdio.h>
#include <assert.h>

#include <rte_config.h>
#include <rte_memcpy.h>
#include <rte_malloc.h>
#include <rte_lcore.h>
#include <rte_hash.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_version.h>
#include <rte_spinlock.h>

#include <utils.h>
#include <utils_rng.h>
#include <tas_memif.h>
#include <pipeline.h>
#include <tas.h>

#define BUFFER_SIZE 2048
#define RX_PERSEQR_MBUFS 2048
#define TX_MBUFS         4096
#define SLOWPATH_MBUFS   256
#define MBUF_SIZE   (BUFFER_SIZE + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define RX_DESCRIPTORS 2048
#define TX_DESCRIPTORS 2048
#define MAX_ETH_RXQ    NUM_SEQ_CTXS

uint8_t net_port_id = 0;
static struct rte_eth_conf port_conf = {
    .rxmode = {
      .mq_mode = ETH_MQ_RX_RSS,
      .offloads = 0,
#if RTE_VER_YEAR < 18
      .ignore_offload_bitfield = 1,
#endif
    },
    .txmode = {
      .mq_mode = ETH_MQ_TX_NONE,
      .offloads = 0,
    },
    .rx_adv_conf = {
      .rss_conf = {
        .rss_hf = ETH_RSS_NONFRAG_IPV4_TCP,
      },
    },
    .intr_conf = {
      .rxq = 1,
    },
  };

static struct rte_eth_dev_info eth_devinfo;
#if RTE_VER_YEAR < 19
  struct ether_addr eth_addr;
#else
  struct rte_ether_addr eth_addr;
#endif

struct rte_mempool *rx_pkt_mempools[NUM_SEQ_CTXS];
struct rte_mempool *tx_pkt_mempool;
struct rte_mempool *sp_pkt_mempool;

int network_init()
{
  uint8_t count;
  int ret;
  uint16_t p;
  unsigned i;
  unsigned n_rx, n_tx;
  char name[32];

  n_rx = NUM_SEQ_CTXS;
  n_tx = NUM_SEQ_CTXS + 1;    // 1 extra for slowpath

  /* make sure there is only one port */
#if RTE_VER_YEAR < 18
  count = rte_eth_dev_count();
#else
  count = rte_eth_dev_count_avail();
#endif
  if (count == 0) {
    fprintf(stderr, "No ethernet devices\n");
    goto error_exit;
  } else if (count > 1) {
    fprintf(stderr, "Multiple ethernet devices\n");
    goto error_exit;
  }

  RTE_ETH_FOREACH_DEV(p) {
    net_port_id = p;
  }

  /* get mac address and device info */
  rte_eth_macaddr_get(net_port_id, &eth_addr);
  rte_eth_dev_info_get(net_port_id, &eth_devinfo);

  if (eth_devinfo.max_rx_queues < n_rx ||
      eth_devinfo.max_tx_queues < n_tx)
  {
    fprintf(stderr, "Error: NIC does not support enough hw queues (rx=%u tx=%u)"
        " for the requested number of cores (%u)\n", eth_devinfo.max_rx_queues,
        eth_devinfo.max_tx_queues, n_rx);
    goto error_exit;
  }

  /* mask unsupported RSS hash functions */
  if ((port_conf.rx_adv_conf.rss_conf.rss_hf &
       eth_devinfo.flow_type_rss_offloads) !=
      port_conf.rx_adv_conf.rss_conf.rss_hf)
  {
    fprintf(stderr, "Warning: NIC does not support all requested RSS "
        "hash functions.\n");
    port_conf.rx_adv_conf.rss_conf.rss_hf &= eth_devinfo.flow_type_rss_offloads;
  }

  /* enable per port checksum offload if requested */
  if (config.fp_xsumoffload)
    port_conf.txmode.offloads =
      DEV_TX_OFFLOAD_IPV4_CKSUM | DEV_TX_OFFLOAD_TCP_CKSUM;

  /* disable rx interrupts */
  port_conf.intr_conf.rxq = 0;

  /* initialize port */
  ret = rte_eth_dev_configure(net_port_id, n_rx, n_tx, &port_conf);
  if (ret < 0) {
    fprintf(stderr, "rte_eth_dev_configure failed\n");
    goto error_exit;
  }

#if RTE_VER_YEAR < 18
  eth_devinfo.default_txconf.txq_flags = ETH_TXQ_FLAGS_IGNORE;
#endif
  eth_devinfo.default_rxconf.offloads = 0;

  /* enable per-queue checksum offload if requested */
  eth_devinfo.default_txconf.offloads = 0;
  if (config.fp_xsumoffload)
    eth_devinfo.default_txconf.offloads =
      DEV_TX_OFFLOAD_IPV4_CKSUM | DEV_TX_OFFLOAD_TCP_CKSUM;

  memcpy(&tas_info->mac_address, &eth_addr, 6);

  /* Init RX queues */
  for (i = 0; i < n_rx; i++) {
    snprintf(name, 32, "rx_pktmbuf_pool_%u\n", i);
    rx_pkt_mempools[i] = rte_mempool_create(name, RX_PERSEQR_MBUFS, MBUF_SIZE, 32,
          sizeof(struct rte_pktmbuf_pool_private), rte_pktmbuf_pool_init, NULL,
          rte_pktmbuf_init, NULL, rte_socket_id(), 0);
    if (rx_pkt_mempools[i] == NULL) {
      fprintf(stderr, "network_thread_init: rte_mempool_create failed\n");
      return -1;
    }

    ret = rte_eth_rx_queue_setup(net_port_id, i, RX_DESCRIPTORS,
            rte_socket_id(), &eth_devinfo.default_rxconf, rx_pkt_mempools[i]);
    if (ret != 0) {
      fprintf(stderr, "network_thread_init: rte_eth_rx_queue_setup failed\n");
      return -1;
    }
  }

  /* Init TX queues */
  for (i = 0; i < n_tx; i++) {
    ret = rte_eth_tx_queue_setup(net_port_id, i, TX_DESCRIPTORS,
            rte_socket_id(), &eth_devinfo.default_txconf);
    if (ret != 0) {
      fprintf(stderr, "network_thread_init: rte_eth_tx_queue_setup failed\n");
      return -1;
    }
  }

  /* Init SP PKT mempool */
  snprintf(name, 32, "sp_pktmbuf_pool");
  sp_pkt_mempool = rte_mempool_create(name, SLOWPATH_MBUFS, MBUF_SIZE, 0,
          sizeof(struct rte_pktmbuf_pool_private), rte_pktmbuf_pool_init, NULL,
          rte_pktmbuf_init, NULL, rte_socket_id(), 0);
  if (sp_pkt_mempool == NULL) {
    fprintf(stderr, "network_thread_init: rte_mempool_create failed\n");
    return -1; 
  }

  /* Init TX PKT mempool */
  snprintf(name, 32, "tx_pktmbuf_pool");
  tx_pkt_mempool = rte_mempool_create(name, TX_MBUFS, MBUF_SIZE, 32,
          sizeof(struct rte_pktmbuf_pool_private), rte_pktmbuf_pool_init, NULL,
          rte_pktmbuf_init, NULL, rte_socket_id(), 0);
  if (tx_pkt_mempool == NULL) {
    fprintf(stderr, "network_thread_init: rte_mempool_create failed\n");
    return -1; 
  }

  /* Start device */
  if (rte_eth_dev_start(net_port_id) != 0) {
    fprintf(stderr, "rte_eth_dev_start failed\n");
    return -1;
  }

  /* enable vlan stripping if configured */
  if (config.fp_vlan_strip) {
    ret = rte_eth_dev_get_vlan_offload(net_port_id);
    ret |= ETH_VLAN_STRIP_OFFLOAD;
    if (rte_eth_dev_set_vlan_offload(net_port_id, ret)) {
      fprintf(stderr, "network_thread_init: vlan off set failed\n");
      return -1;
    }
  }

  return 0;

error_exit:
  return -1;
}

void network_cleanup(void)
{
  rte_eth_dev_stop(net_port_id);
}

void network_dump_stats(void)
{
  struct rte_eth_stats stats;
  if (rte_eth_stats_get(0, &stats) == 0) {
    fprintf(stderr, "network stats: ipackets=%"PRIu64" opackets=%"PRIu64
        " ibytes=%"PRIu64" obytes=%"PRIu64" imissed=%"PRIu64" ierrors=%"PRIu64
        " oerrors=%"PRIu64" rx_nombuf=%"PRIu64"\n", stats.ipackets,
        stats.opackets, stats.ibytes, stats.obytes, stats.imissed,
        stats.ierrors, stats.oerrors, stats.rx_nombuf);
  } else {
    fprintf(stderr, "failed to get stats\n");
  }
}
