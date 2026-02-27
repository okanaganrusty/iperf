/*
 * iperf, Copyright (c) 2014-2025, The Regents of the University of
 * California, through Lawrence Berkeley National Laboratory (subject
 * to receipt of any required approvals from the U.S. Dept. of
 * Energy).  All rights reserved.
 *
 * DPDK networking layer for iperf3
 */
#ifndef __DPDK_NET_H
#define __DPDK_NET_H

#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <rte_common.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>

/* DPDK Configuration */
#define DPDK_MAX_RX_BURST 256
#define DPDK_MAX_TX_BURST 256
#define DPDK_MAX_PKT_PER_CONN_PER_BURST 64  /* Fair share per connection per TX burst */
#define DPDK_RX_RING_SIZE 32768
#define DPDK_TX_RING_SIZE 4096
#define DPDK_NUM_MBUFS 65535
#define DPDK_MBUF_CACHE_SIZE 512
#define DPDK_MAX_CONNECTIONS 1024
#define DPDK_RX_BUFFER_SIZE 1048576

/* Connection states */
#define DPDK_CONN_STATE_CLOSED 0
#define DPDK_CONN_STATE_LISTEN 1
#define DPDK_CONN_STATE_SYN_SENT 2
#define DPDK_CONN_STATE_SYN_RECEIVED 3
#define DPDK_CONN_STATE_ESTABLISHED 4
#define DPDK_CONN_STATE_FIN_WAIT_1 5
#define DPDK_CONN_STATE_FIN_WAIT_2 6
#define DPDK_CONN_STATE_CLOSE_WAIT 7
#define DPDK_CONN_STATE_CLOSING 8
#define DPDK_CONN_STATE_LAST_ACK 9
#define DPDK_CONN_STATE_TIME_WAIT 10

/* Protocol types */
#define DPDK_PROTO_TCP 6
#define DPDK_PROTO_UDP 17

/* Connection structure to track TCP/UDP state */
struct dpdk_connection {
    int fd;                          /* File descriptor (virtual) */
    uint8_t state;                   /* Connection state */
    uint8_t protocol;                /* DPDK_PROTO_TCP or DPDK_PROTO_UDP */

    /* Local and remote addresses */
    struct sockaddr_storage local_addr;
    struct sockaddr_storage remote_addr;
    socklen_t local_addr_len;
    socklen_t remote_addr_len;

    /* TCP state */
    uint32_t seq_num;                /* Sequence number */
    uint32_t ack_num;                /* Acknowledgment number */
    uint32_t last_ack_sent;          /* Last ACK value transmitted */
    uint64_t last_ack_tsc;           /* TSC at last ACK transmission */
    uint16_t window_size;            /* Window size */

    /* Transmit and receive buffers */
    struct rte_ring *rx_ring;        /* Received packets */
    struct rte_ring *tx_ring;        /* Packets to transmit */
    char *rx_buffer;                 /* Reassembly buffer */
    uint32_t rx_buffer_size;
    uint32_t rx_buffer_offset;

    /* Timestamps */
    uint64_t last_activity;

    /* Flags */
    int listening;                   /* Is this a listening socket? */
    int connected;                   /* Is this connected? */
    int nonblocking;                 /* Non-blocking mode */

    /* Parent listener (for accepted connections) */
    int parent_fd;

    /* DPDK port */
    uint16_t port_id;

    /* Learned remote MAC address */
    struct rte_ether_addr remote_mac;
    int remote_mac_valid;
};

/* Global DPDK state */
struct dpdk_state {
    uint16_t port_id;                /* DPDK port ID */
    struct rte_mempool *mbuf_pool;   /* Packet buffer pool */
    struct rte_hash *conn_hash;      /* Connection hash table */
    struct dpdk_connection *connections[DPDK_MAX_CONNECTIONS];
    int next_fd;                     /* Next available file descriptor */

    /* MAC and IP configuration */
    struct rte_ether_addr mac_addr;
    uint32_t ipv4_addr;
    uint32_t ipv4_netmask;
    uint32_t ipv4_gateway;
    struct in6_addr ipv6_addr;

    /* Statistics */
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_errors;
    uint64_t tx_errors;

    /* Debug flags */
    int debug;                       /* Enable debug output */
    int packet_dump;                 /* Enable packet dumping */

    /* Fast-path threading */
    int fast_path_enabled;
    volatile int fast_path_running;
    int fast_path_rx_started;
    int fast_path_tx_started;
    pthread_t rx_thread;
    pthread_t tx_thread;
};

/* DPDK initialization and cleanup */
int dpdk_net_init(int argc, char **argv, uint16_t port_id, const char *ip_addr, const char *netmask, int debug);
int dpdk_net_cleanup(void);
int dpdk_net_configure_port(uint16_t port_id);

/* Socket-like API functions */
int dpdk_socket(int domain, int type, int protocol);
int dpdk_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int dpdk_listen(int sockfd, int backlog);
int dpdk_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int dpdk_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t dpdk_send(int sockfd, const void *buf, size_t len, int flags);
ssize_t dpdk_recv(int sockfd, void *buf, size_t len, int flags);
ssize_t dpdk_sendto(int sockfd, const void *buf, size_t len, int flags,
                    const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t dpdk_recvfrom(int sockfd, void *buf, size_t len, int flags,
                      struct sockaddr *src_addr, socklen_t *addrlen);
int dpdk_close(int sockfd);
int dpdk_setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
int dpdk_getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
int dpdk_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int dpdk_getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int dpdk_fcntl(int sockfd, int cmd, ... /* arg */);

/* Packet processing */
int dpdk_rx_burst(uint16_t port_id);
int dpdk_tx_burst(uint16_t port_id);
int dpdk_process_packets(void);

/* Connection management */
struct dpdk_connection *dpdk_get_connection(int sockfd);
int dpdk_allocate_fd(void);
void dpdk_free_connection(int sockfd);

/* Utility functions */
uint16_t dpdk_checksum(const void *buf, size_t len);
int dpdk_parse_ip_addr(const char *ip_str, uint32_t *ip_addr);
int dpdk_create_tcp_packet(struct dpdk_connection *conn, struct rte_mbuf *mbuf,
                           const void *data, size_t len, uint8_t flags);
int dpdk_create_udp_packet(struct dpdk_connection *conn, struct rte_mbuf *mbuf,
                           const void *data, size_t len);
void dpdk_dump_packet(const char *prefix, struct rte_mbuf *mbuf);
void dpdk_enable_packet_dump(int enable);

/* Helper macros */
#define DPDK_TCP_FLAG_FIN 0x01
#define DPDK_TCP_FLAG_SYN 0x02
#define DPDK_TCP_FLAG_RST 0x04
#define DPDK_TCP_FLAG_PSH 0x08
#define DPDK_TCP_FLAG_ACK 0x10
#define DPDK_TCP_FLAG_URG 0x20

/* Global DPDK state (extern) */
extern struct dpdk_state *g_dpdk_state;

/* Feature flags */
#define DPDK_FEATURE_ENABLED 1

#endif /* __DPDK_NET_H */
