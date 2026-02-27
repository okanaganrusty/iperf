/*
 * iperf, Copyright (c) 2014-2025, The Regents of the University of
 * California, through Lawrence Berkeley National Laboratory (subject
 * to receipt of any required approvals from the U.S. Dept. of
 * Energy).  All rights reserved.
 *
 * DPDK networking layer implementation for iperf3
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>

#include "dpdk_net.h"

/* Global DPDK state */
struct dpdk_state *g_dpdk_state = NULL;

/* Static helper functions */
static struct dpdk_connection *dpdk_alloc_connection(int fd);
static void dpdk_handle_tcp_packet(struct dpdk_connection *conn, struct rte_mbuf *mbuf);
static void dpdk_handle_udp_packet(struct dpdk_connection *conn, struct rte_mbuf *mbuf);
static int dpdk_send_tcp_syn(struct dpdk_connection *conn);
static int dpdk_send_tcp_ack(struct dpdk_connection *conn);

/* Initialize DPDK */
int dpdk_net_init(int argc, char **argv, uint16_t port_id, const char *ip_addr)
{
    int ret;
    char pool_name[32];
    char hash_name[32];
    struct rte_hash_parameters hash_params = {0};

    /* Initialize DPDK EAL */
    ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "DPDK EAL initialization failed\n");
        return -1;
    }

    /* Allocate global state */
    g_dpdk_state = calloc(1, sizeof(struct dpdk_state));
    if (!g_dpdk_state) {
        fprintf(stderr, "Failed to allocate DPDK state\n");
        return -1;
    }

    g_dpdk_state->port_id = port_id;
    g_dpdk_state->next_fd = 100; /* Start from 100 to avoid conflicts */

    /* Create packet buffer pool */
    snprintf(pool_name, sizeof(pool_name), "mbuf_pool_%u", port_id);
    g_dpdk_state->mbuf_pool = rte_pktmbuf_pool_create(pool_name,
                                                        DPDK_NUM_MBUFS,
                                                        DPDK_MBUF_CACHE_SIZE,
                                                        0,
                                                        RTE_MBUF_DEFAULT_BUF_SIZE,
                                                        rte_socket_id());
    if (!g_dpdk_state->mbuf_pool) {
        fprintf(stderr, "Failed to create mbuf pool\n");
        free(g_dpdk_state);
        return -1;
    }

    /* Create connection hash table */
    snprintf(hash_name, sizeof(hash_name), "conn_hash_%u", port_id);
    hash_params.name = hash_name;
    hash_params.entries = DPDK_MAX_CONNECTIONS;
    hash_params.key_len = sizeof(int);
    hash_params.hash_func = rte_jhash;
    hash_params.hash_func_init_val = 0;
    hash_params.socket_id = rte_socket_id();

    g_dpdk_state->conn_hash = rte_hash_create(&hash_params);
    if (!g_dpdk_state->conn_hash) {
        fprintf(stderr, "Failed to create connection hash table\n");
        rte_mempool_free(g_dpdk_state->mbuf_pool);
        free(g_dpdk_state);
        return -1;
    }

    /* Configure the port */
    if (dpdk_net_configure_port(port_id) < 0) {
        fprintf(stderr, "Failed to configure DPDK port %u\n", port_id);
        rte_hash_free(g_dpdk_state->conn_hash);
        rte_mempool_free(g_dpdk_state->mbuf_pool);
        free(g_dpdk_state);
        return -1;
    }

    /* Parse and set IP address */
    if (ip_addr) {
        if (inet_pton(AF_INET, ip_addr, &g_dpdk_state->ipv4_addr) != 1) {
            fprintf(stderr, "Invalid IP address: %s\n", ip_addr);
            return -1;
        }
    }

    /* Get MAC address */
    rte_eth_macaddr_get(port_id, &g_dpdk_state->mac_addr);

    printf("DPDK initialized on port %u, MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           port_id,
           g_dpdk_state->mac_addr.addr_bytes[0],
           g_dpdk_state->mac_addr.addr_bytes[1],
           g_dpdk_state->mac_addr.addr_bytes[2],
           g_dpdk_state->mac_addr.addr_bytes[3],
           g_dpdk_state->mac_addr.addr_bytes[4],
           g_dpdk_state->mac_addr.addr_bytes[5]);

    return 0;
}

/* Configure DPDK port */
int dpdk_net_configure_port(uint16_t port_id)
{
    struct rte_eth_conf port_conf = {0};
    struct rte_eth_dev_info dev_info;
    int ret;
    uint16_t nb_rxd = DPDK_RX_RING_SIZE;
    uint16_t nb_txd = DPDK_TX_RING_SIZE;

    /* Check if port is valid */
    if (!rte_eth_dev_is_valid_port(port_id)) {
        fprintf(stderr, "Invalid port %u\n", port_id);
        return -1;
    }

    /* Get device info */
    ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0) {
        fprintf(stderr, "Error getting device info: %s\n", rte_strerror(-ret));
        return -1;
    }

    /* Configure the device */
    port_conf.rxmode.max_lro_pkt_size = RTE_ETHER_MAX_LEN;
    port_conf.txmode.offloads = RTE_ETH_TX_OFFLOAD_MULTI_SEGS;

    ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
    if (ret < 0) {
        fprintf(stderr, "Failed to configure port %u: %s\n", port_id, rte_strerror(-ret));
        return -1;
    }

    /* Allocate and set up RX queue */
    ret = rte_eth_rx_queue_setup(port_id, 0, nb_rxd,
                                  rte_eth_dev_socket_id(port_id),
                                  NULL,
                                  g_dpdk_state->mbuf_pool);
    if (ret < 0) {
        fprintf(stderr, "Failed to setup RX queue: %s\n", rte_strerror(-ret));
        return -1;
    }

    /* Allocate and set up TX queue */
    ret = rte_eth_tx_queue_setup(port_id, 0, nb_txd,
                                  rte_eth_dev_socket_id(port_id),
                                  NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to setup TX queue: %s\n", rte_strerror(-ret));
        return -1;
    }

    /* Start the device */
    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        fprintf(stderr, "Failed to start port %u: %s\n", port_id, rte_strerror(-ret));
        return -1;
    }

    /* Enable promiscuous mode */
    ret = rte_eth_promiscuous_enable(port_id);
    if (ret != 0) {
        fprintf(stderr, "Failed to enable promiscuous mode: %s\n", rte_strerror(-ret));
    }

    printf("Port %u configured successfully\n", port_id);
    return 0;
}

/* Cleanup DPDK resources */
int dpdk_net_cleanup(void)
{
    int i;

    if (!g_dpdk_state) {
        return 0;
    }

    /* Close all connections */
    for (i = 0; i < DPDK_MAX_CONNECTIONS; i++) {
        if (g_dpdk_state->connections[i]) {
            dpdk_free_connection(g_dpdk_state->connections[i]->fd);
        }
    }

    /* Stop and close the port */
    rte_eth_dev_stop(g_dpdk_state->port_id);
    rte_eth_dev_close(g_dpdk_state->port_id);

    /* Free resources */
    if (g_dpdk_state->conn_hash) {
        rte_hash_free(g_dpdk_state->conn_hash);
    }

    if (g_dpdk_state->mbuf_pool) {
        rte_mempool_free(g_dpdk_state->mbuf_pool);
    }

    free(g_dpdk_state);
    g_dpdk_state = NULL;

    rte_eal_cleanup();

    return 0;
}

/* Allocate a new file descriptor */
int dpdk_allocate_fd(void)
{
    if (!g_dpdk_state) {
        return -1;
    }
    return g_dpdk_state->next_fd++;
}

/* Allocate and initialize a connection structure */
static struct dpdk_connection *dpdk_alloc_connection(int fd)
{
    struct dpdk_connection *conn;
    char ring_name[64];

    conn = calloc(1, sizeof(struct dpdk_connection));
    if (!conn) {
        return NULL;
    }

    conn->fd = fd;
    conn->state = DPDK_CONN_STATE_CLOSED;
    conn->window_size = 65535;
    conn->port_id = g_dpdk_state->port_id;

    /* Create RX ring */
    snprintf(ring_name, sizeof(ring_name), "rx_ring_%d", fd);
    conn->rx_ring = rte_ring_create(ring_name, 1024, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!conn->rx_ring) {
        free(conn);
        return NULL;
    }

    /* Create TX ring */
    snprintf(ring_name, sizeof(ring_name), "tx_ring_%d", fd);
    conn->tx_ring = rte_ring_create(ring_name, 1024, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!conn->tx_ring) {
        rte_ring_free(conn->rx_ring);
        free(conn);
        return NULL;
    }

    /* Allocate RX buffer */
    conn->rx_buffer = malloc(DPDK_RX_BUFFER_SIZE);
    if (!conn->rx_buffer) {
        rte_ring_free(conn->tx_ring);
        rte_ring_free(conn->rx_ring);
        free(conn);
        return NULL;
    }
    conn->rx_buffer_size = DPDK_RX_BUFFER_SIZE;

    return conn;
}

/* Get connection by file descriptor */
struct dpdk_connection *dpdk_get_connection(int sockfd)
{
    int idx;

    if (!g_dpdk_state) {
        return NULL;
    }

    /* Simple linear search - could be optimized with hash table */
    for (idx = 0; idx < DPDK_MAX_CONNECTIONS; idx++) {
        if (g_dpdk_state->connections[idx] &&
            g_dpdk_state->connections[idx]->fd == sockfd) {
            return g_dpdk_state->connections[idx];
        }
    }

    return NULL;
}

/* Free a connection */
void dpdk_free_connection(int sockfd)
{
    struct dpdk_connection *conn;
    int idx;

    if (!g_dpdk_state) {
        return;
    }

    for (idx = 0; idx < DPDK_MAX_CONNECTIONS; idx++) {
        if (g_dpdk_state->connections[idx] &&
            g_dpdk_state->connections[idx]->fd == sockfd) {
            conn = g_dpdk_state->connections[idx];

            if (conn->rx_ring) {
                rte_ring_free(conn->rx_ring);
            }
            if (conn->tx_ring) {
                rte_ring_free(conn->tx_ring);
            }
            if (conn->rx_buffer) {
                free(conn->rx_buffer);
            }

            free(conn);
            g_dpdk_state->connections[idx] = NULL;
            break;
        }
    }
}

/* Create a socket */
int dpdk_socket(int domain, int type, int protocol)
{
    struct dpdk_connection *conn;
    int fd;
    int idx;

    if (!g_dpdk_state) {
        errno = ENOSYS;
        return -1;
    }

    /* Find a free slot */
    for (idx = 0; idx < DPDK_MAX_CONNECTIONS; idx++) {
        if (g_dpdk_state->connections[idx] == NULL) {
            break;
        }
    }

    if (idx >= DPDK_MAX_CONNECTIONS) {
        errno = EMFILE;
        return -1;
    }

    fd = dpdk_allocate_fd();
    conn = dpdk_alloc_connection(fd);
    if (!conn) {
        errno = ENOMEM;
        return -1;
    }

    /* Set protocol */
    if (type == SOCK_STREAM) {
        conn->protocol = DPDK_PROTO_TCP;
    } else if (type == SOCK_DGRAM) {
        conn->protocol = DPDK_PROTO_UDP;
    } else {
        dpdk_free_connection(fd);
        errno = EPROTONOSUPPORT;
        return -1;
    }

    g_dpdk_state->connections[idx] = conn;

    return fd;
}

/* Bind a socket to an address */
int dpdk_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    struct dpdk_connection *conn;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        errno = EBADF;
        return -1;
    }

    memcpy(&conn->local_addr, addr, addrlen);
    conn->local_addr_len = addrlen;

    return 0;
}

/* Listen for connections */
int dpdk_listen(int sockfd, int backlog)
{
    struct dpdk_connection *conn;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        errno = EBADF;
        return -1;
    }

    if (conn->protocol != DPDK_PROTO_TCP) {
        errno = EOPNOTSUPP;
        return -1;
    }

    conn->listening = 1;
    conn->state = DPDK_CONN_STATE_LISTEN;

    return 0;
}

/* Accept a connection */
int dpdk_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    struct dpdk_connection *conn;
    struct rte_mbuf *mbuf;
    int new_fd;
    struct dpdk_connection *new_conn;
    int idx;

    conn = dpdk_get_connection(sockfd);
    if (!conn || !conn->listening) {
        errno = EINVAL;
        return -1;
    }

    /* Poll for packets - simplified version */
    /* In a real implementation, this would wait for SYN packets */
    dpdk_process_packets();

    /* Check if there are any pending connections in the RX ring */
    if (rte_ring_dequeue(conn->rx_ring, (void **)&mbuf) == 0) {
        /* Create new connection for accepted socket */
        for (idx = 0; idx < DPDK_MAX_CONNECTIONS; idx++) {
            if (g_dpdk_state->connections[idx] == NULL) {
                break;
            }
        }

        if (idx >= DPDK_MAX_CONNECTIONS) {
            rte_pktmbuf_free(mbuf);
            errno = EMFILE;
            return -1;
        }

        new_fd = dpdk_allocate_fd();
        new_conn = dpdk_alloc_connection(new_fd);
        if (!new_conn) {
            rte_pktmbuf_free(mbuf);
            errno = ENOMEM;
            return -1;
        }

        new_conn->protocol = DPDK_PROTO_TCP;
        new_conn->state = DPDK_CONN_STATE_ESTABLISHED;
        new_conn->connected = 1;
        new_conn->parent_fd = sockfd;

        /* Copy local address from listener */
        memcpy(&new_conn->local_addr, &conn->local_addr, conn->local_addr_len);
        new_conn->local_addr_len = conn->local_addr_len;

        /* Extract remote address from packet - simplified */
        /* In real implementation, parse IP header */

        g_dpdk_state->connections[idx] = new_conn;

        if (addr && addrlen) {
            memcpy(addr, &new_conn->remote_addr, *addrlen);
        }

        rte_pktmbuf_free(mbuf);
        return new_fd;
    }

    /* No connection available */
    if (conn->nonblocking) {
        errno = EAGAIN;
        return -1;
    }

    /* In blocking mode, would wait here */
    errno = EAGAIN;
    return -1;
}

/* Connect to a remote address */
int dpdk_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    struct dpdk_connection *conn;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        errno = EBADF;
        return -1;
    }

    memcpy(&conn->remote_addr, addr, addrlen);
    conn->remote_addr_len = addrlen;

    if (conn->protocol == DPDK_PROTO_TCP) {
        /* Send SYN packet */
        conn->state = DPDK_CONN_STATE_SYN_SENT;
        dpdk_send_tcp_syn(conn);

        /* In a real implementation, wait for SYN-ACK */
        conn->state = DPDK_CONN_STATE_ESTABLISHED;
        conn->connected = 1;
    } else {
        /* UDP - just mark as connected */
        conn->connected = 1;
    }

    return 0;
}

/* Send data */
ssize_t dpdk_send(int sockfd, const void *buf, size_t len, int flags)
{
    struct dpdk_connection *conn;
    struct rte_mbuf *mbuf;
    int ret;

    conn = dpdk_get_connection(sockfd);
    if (!conn || !conn->connected) {
        errno = ENOTCONN;
        return -1;
    }

    /* Allocate mbuf */
    mbuf = rte_pktmbuf_alloc(g_dpdk_state->mbuf_pool);
    if (!mbuf) {
        errno = ENOMEM;
        return -1;
    }

    /* Create packet */
    if (conn->protocol == DPDK_PROTO_TCP) {
        ret = dpdk_create_tcp_packet(conn, mbuf, buf, len, DPDK_TCP_FLAG_PSH | DPDK_TCP_FLAG_ACK);
    } else {
        ret = dpdk_create_udp_packet(conn, mbuf, buf, len);
    }

    if (ret < 0) {
        rte_pktmbuf_free(mbuf);
        errno = EINVAL;
        return -1;
    }

    /* Queue packet for transmission */
    if (rte_ring_enqueue(conn->tx_ring, mbuf) < 0) {
        rte_pktmbuf_free(mbuf);
        errno = EAGAIN;
        return -1;
    }

    /* Trigger TX burst */
    dpdk_tx_burst(conn->port_id);

    return len;
}

/* Receive data */
ssize_t dpdk_recv(int sockfd, void *buf, size_t len, int flags)
{
    struct dpdk_connection *conn;
    ssize_t copied = 0;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        errno = EBADF;
        return -1;
    }

    /* Process incoming packets */
    dpdk_process_packets();

    /* Copy data from RX buffer */
    if (conn->rx_buffer_offset > 0) {
        copied = (conn->rx_buffer_offset < len) ? conn->rx_buffer_offset : len;
        memcpy(buf, conn->rx_buffer, copied);

        /* Shift remaining data */
        if (copied < conn->rx_buffer_offset) {
            memmove(conn->rx_buffer, conn->rx_buffer + copied,
                    conn->rx_buffer_offset - copied);
        }
        conn->rx_buffer_offset -= copied;

        return copied;
    }

    /* No data available */
    if (conn->nonblocking) {
        errno = EAGAIN;
        return -1;
    }

    errno = EAGAIN;
    return -1;
}

/* Send data to a specific address (UDP) */
ssize_t dpdk_sendto(int sockfd, const void *buf, size_t len, int flags,
                    const struct sockaddr *dest_addr, socklen_t addrlen)
{
    struct dpdk_connection *conn;
    struct sockaddr_storage saved_addr;
    socklen_t saved_len;
    ssize_t ret;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        errno = EBADF;
        return -1;
    }

    /* Temporarily set destination */
    saved_addr = conn->remote_addr;
    saved_len = conn->remote_addr_len;

    memcpy(&conn->remote_addr, dest_addr, addrlen);
    conn->remote_addr_len = addrlen;
    conn->connected = 1;

    ret = dpdk_send(sockfd, buf, len, flags);

    /* Restore */
    conn->remote_addr = saved_addr;
    conn->remote_addr_len = saved_len;

    return ret;
}

/* Receive data from a specific address (UDP) */
ssize_t dpdk_recvfrom(int sockfd, void *buf, size_t len, int flags,
                      struct sockaddr *src_addr, socklen_t *addrlen)
{
    struct dpdk_connection *conn;
    ssize_t ret;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        errno = EBADF;
        return -1;
    }

    ret = dpdk_recv(sockfd, buf, len, flags);

    if (ret > 0 && src_addr && addrlen) {
        memcpy(src_addr, &conn->remote_addr, *addrlen);
    }

    return ret;
}

/* Close socket */
int dpdk_close(int sockfd)
{
    struct dpdk_connection *conn;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        errno = EBADF;
        return -1;
    }

    if (conn->protocol == DPDK_PROTO_TCP && conn->connected) {
        /* Send FIN packet - simplified */
        conn->state = DPDK_CONN_STATE_FIN_WAIT_1;
    }

    dpdk_free_connection(sockfd);
    return 0;
}

/* Set socket options */
int dpdk_setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    struct dpdk_connection *conn;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        errno = EBADF;
        return -1;
    }

    /* Handle common socket options - simplified */
    return 0;
}

/* Get socket options */
int dpdk_getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
    struct dpdk_connection *conn;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        errno = EBADF;
        return -1;
    }

    return 0;
}

/* Get socket name */
int dpdk_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    struct dpdk_connection *conn;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        errno = EBADF;
        return -1;
    }

    if (*addrlen < conn->local_addr_len) {
        errno = EINVAL;
        return -1;
    }

    memcpy(addr, &conn->local_addr, conn->local_addr_len);
    *addrlen = conn->local_addr_len;

    return 0;
}

/* Get peer name */
int dpdk_getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    struct dpdk_connection *conn;

    conn = dpdk_get_connection(sockfd);
    if (!conn || !conn->connected) {
        errno = ENOTCONN;
        return -1;
    }

    if (*addrlen < conn->remote_addr_len) {
        errno = EINVAL;
        return -1;
    }

    memcpy(addr, &conn->remote_addr, conn->remote_addr_len);
    *addrlen = conn->remote_addr_len;

    return 0;
}

/* File control */
int dpdk_fcntl(int sockfd, int cmd, ...)
{
    struct dpdk_connection *conn;
    va_list args;
    int flags;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        errno = EBADF;
        return -1;
    }

    va_start(args, cmd);

    switch (cmd) {
        case F_GETFL:
            va_end(args);
            return conn->nonblocking ? O_NONBLOCK : 0;

        case F_SETFL:
            flags = va_arg(args, int);
            conn->nonblocking = (flags & O_NONBLOCK) != 0;
            va_end(args);
            return 0;

        default:
            va_end(args);
            errno = EINVAL;
            return -1;
    }
}

/* Receive burst of packets */
int dpdk_rx_burst(uint16_t port_id)
{
    struct rte_mbuf *bufs[DPDK_MAX_RX_BURST];
    uint16_t nb_rx;
    int i;

    nb_rx = rte_eth_rx_burst(port_id, 0, bufs, DPDK_MAX_RX_BURST);

    g_dpdk_state->rx_packets += nb_rx;

    for (i = 0; i < nb_rx; i++) {
        g_dpdk_state->rx_bytes += rte_pktmbuf_pkt_len(bufs[i]);

        /* Process packet - simplified */
        /* In real implementation: parse Ethernet, IP, TCP/UDP headers */
        /* and route to appropriate connection */

        rte_pktmbuf_free(bufs[i]);
    }

    return nb_rx;
}

/* Transmit burst of packets */
int dpdk_tx_burst(uint16_t port_id)
{
    struct rte_mbuf *bufs[DPDK_MAX_TX_BURST];
    uint16_t nb_tx = 0;
    int i, idx;

    /* Collect packets from all connections */
    for (idx = 0; idx < DPDK_MAX_CONNECTIONS && nb_tx < DPDK_MAX_TX_BURST; idx++) {
        struct dpdk_connection *conn = g_dpdk_state->connections[idx];
        if (conn && conn->tx_ring) {
            while (nb_tx < DPDK_MAX_TX_BURST &&
                   rte_ring_dequeue(conn->tx_ring, (void **)&bufs[nb_tx]) == 0) {
                nb_tx++;
            }
        }
    }

    if (nb_tx > 0) {
        uint16_t sent = rte_eth_tx_burst(port_id, 0, bufs, nb_tx);

        g_dpdk_state->tx_packets += sent;

        for (i = 0; i < sent; i++) {
            g_dpdk_state->tx_bytes += rte_pktmbuf_pkt_len(bufs[i]);
        }

        /* Free unsent packets */
        for (i = sent; i < nb_tx; i++) {
            rte_pktmbuf_free(bufs[i]);
        }

        return sent;
    }

    return 0;
}

/* Process packets - main packet processing loop */
int dpdk_process_packets(void)
{
    if (!g_dpdk_state) {
        return -1;
    }

    /* Receive packets */
    dpdk_rx_burst(g_dpdk_state->port_id);

    /* Transmit packets */
    dpdk_tx_burst(g_dpdk_state->port_id);

    return 0;
}

/* Helper: Calculate checksum */
uint16_t dpdk_checksum(const void *buf, size_t len)
{
    const uint16_t *words = buf;
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i < len / 2; i++) {
        sum += words[i];
    }

    if (len & 1) {
        sum += ((uint8_t *)buf)[len - 1];
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

/* Helper: Parse IP address */
int dpdk_parse_ip_addr(const char *ip_str, uint32_t *ip_addr)
{
    return inet_pton(AF_INET, ip_str, ip_addr) == 1 ? 0 : -1;
}

/* Helper: Create TCP packet */
int dpdk_create_tcp_packet(struct dpdk_connection *conn, struct rte_mbuf *mbuf,
                           const void *data, size_t len, uint8_t flags)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_tcp_hdr *tcp_hdr;
    char *payload;

    /* Reserve space for headers */
    eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);
    payload = (char *)(tcp_hdr + 1);

    /* Copy payload */
    if (len > 0) {
        rte_memcpy(payload, data, len);
    }

    /* Set packet length */
    mbuf->data_len = sizeof(*eth_hdr) + sizeof(*ip_hdr) + sizeof(*tcp_hdr) + len;
    mbuf->pkt_len = mbuf->data_len;

    /* Fill Ethernet header - simplified */
    memcpy(&eth_hdr->src_addr, &g_dpdk_state->mac_addr, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    /* Fill IP header - simplified */
    ip_hdr->version_ihl = 0x45; /* IPv4, 20 byte header */
    ip_hdr->type_of_service = 0;
    ip_hdr->total_length = rte_cpu_to_be_16(sizeof(*ip_hdr) + sizeof(*tcp_hdr) + len);
    ip_hdr->packet_id = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = 64;
    ip_hdr->next_proto_id = DPDK_PROTO_TCP;
    ip_hdr->src_addr = g_dpdk_state->ipv4_addr;
    /* dst_addr would be filled from conn->remote_addr */

    /* Fill TCP header - simplified */
    tcp_hdr->data_off = 0x50; /* 20 byte header */
    tcp_hdr->tcp_flags = flags;
    tcp_hdr->rx_win = rte_cpu_to_be_16(conn->window_size);
    tcp_hdr->cksum = 0;
    tcp_hdr->tcp_urp = 0;

    conn->seq_num += len;

    return 0;
}

/* Helper: Create UDP packet */
int dpdk_create_udp_packet(struct dpdk_connection *conn, struct rte_mbuf *mbuf,
                           const void *data, size_t len)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_udp_hdr *udp_hdr;
    char *payload;

    /* Reserve space for headers */
    eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
    payload = (char *)(udp_hdr + 1);

    /* Copy payload */
    if (len > 0) {
        rte_memcpy(payload, data, len);
    }

    /* Set packet length */
    mbuf->data_len = sizeof(*eth_hdr) + sizeof(*ip_hdr) + sizeof(*udp_hdr) + len;
    mbuf->pkt_len = mbuf->data_len;

    /* Fill headers - simplified */
    memcpy(&eth_hdr->src_addr, &g_dpdk_state->mac_addr, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    ip_hdr->version_ihl = 0x45;
    ip_hdr->total_length = rte_cpu_to_be_16(sizeof(*ip_hdr) + sizeof(*udp_hdr) + len);
    ip_hdr->next_proto_id = DPDK_PROTO_UDP;
    ip_hdr->src_addr = g_dpdk_state->ipv4_addr;

    udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(*udp_hdr) + len);
    udp_hdr->dgram_cksum = 0;

    return 0;
}

/* Helper: Send TCP SYN */
static int dpdk_send_tcp_syn(struct dpdk_connection *conn)
{
    struct rte_mbuf *mbuf;

    mbuf = rte_pktmbuf_alloc(g_dpdk_state->mbuf_pool);
    if (!mbuf) {
        return -1;
    }

    dpdk_create_tcp_packet(conn, mbuf, NULL, 0, DPDK_TCP_FLAG_SYN);
    rte_ring_enqueue(conn->tx_ring, mbuf);
    dpdk_tx_burst(conn->port_id);

    return 0;
}

/* Helper: Send TCP ACK */
__attribute__((unused))
static int dpdk_send_tcp_ack(struct dpdk_connection *conn)
{
    struct rte_mbuf *mbuf;

    mbuf = rte_pktmbuf_alloc(g_dpdk_state->mbuf_pool);
    if (!mbuf) {
        return -1;
    }

    dpdk_create_tcp_packet(conn, mbuf, NULL, 0, DPDK_TCP_FLAG_ACK);
    rte_ring_enqueue(conn->tx_ring, mbuf);
    dpdk_tx_burst(conn->port_id);

    return 0;
}

/* Placeholder packet handlers */
__attribute__((unused))
static void dpdk_handle_tcp_packet(struct dpdk_connection *conn, struct rte_mbuf *mbuf)
{
    /* Parse TCP packet and update connection state */
    /* Extract payload and add to RX buffer */
    (void)conn;  /* Suppress unused parameter warning */
    (void)mbuf;  /* Suppress unused parameter warning */
}

__attribute__((unused))
static void dpdk_handle_udp_packet(struct dpdk_connection *conn, struct rte_mbuf *mbuf)
{
    /* Parse UDP packet and extract payload */
    /* Add to RX buffer */
    (void)conn;  /* Suppress unused parameter warning */
    (void)mbuf;  /* Suppress unused parameter warning */
}
