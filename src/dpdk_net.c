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
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
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
int dpdk_net_init(int argc, char **argv, uint16_t port_id, const char *ip_addr, const char *netmask, int debug)
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
    g_dpdk_state->debug = debug;
    g_dpdk_state->packet_dump = (debug > 0); /* Enable packet dump if debug enabled */

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

    /* Parse and set netmask (default to /24 if not specified) */
    if (netmask) {
        if (inet_pton(AF_INET, netmask, &g_dpdk_state->ipv4_netmask) != 1) {
            fprintf(stderr, "Invalid netmask: %s\n", netmask);
            return -1;
        }
    } else {
        /* Default to 255.255.255.0 (/24) */
        inet_pton(AF_INET, "255.255.255.0", &g_dpdk_state->ipv4_netmask);
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

    /* Log IP configuration */
    if (g_dpdk_state->ipv4_addr != 0) {
        char ip_str[INET_ADDRSTRLEN];
        char mask_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &g_dpdk_state->ipv4_addr, ip_str, INET_ADDRSTRLEN);
        inet_ntop(AF_INET, &g_dpdk_state->ipv4_netmask, mask_str, INET_ADDRSTRLEN);
        printf("DPDK IP Configuration: %s netmask %s\n", ip_str, mask_str);

        if (debug) {
            printf("DPDK Debug: Packet dumping enabled\n");
        }
    }

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
    conn->rx_ring = rte_ring_create(ring_name, DPDK_RX_RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!conn->rx_ring) {
        free(conn);
        return NULL;
    }

    /* Create TX ring */
    snprintf(ring_name, sizeof(ring_name), "tx_ring_%d", fd);
    conn->tx_ring = rte_ring_create(ring_name, DPDK_TX_RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
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
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_tcp_hdr *tcp_hdr;
    struct sockaddr_in remote_addr;

    conn = dpdk_get_connection(sockfd);
    if (!conn || !conn->listening) {
        errno = EINVAL;
        return -1;
    }

    /* Poll for packets a few times to ensure we catch everything */
    for (int i = 0; i < 5; i++) {
        dpdk_process_packets();
        usleep(1000); /* 1ms between polls */
    }

    /* Check if there are any pending connections in the RX ring */
    if (rte_ring_dequeue(conn->rx_ring, (void **)&mbuf) == 0) {
        /* Parse the packet to extract remote address */
        eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
        tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);

        /* Extract remote address from packet */
        memset(&remote_addr, 0, sizeof(remote_addr));
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_addr.s_addr = ip_hdr->src_addr;
        remote_addr.sin_port = tcp_hdr->src_port;

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
        new_conn->port_id = conn->port_id;

        /* Copy local address from listener */
        memcpy(&new_conn->local_addr, &conn->local_addr, conn->local_addr_len);
        new_conn->local_addr_len = conn->local_addr_len;

        /* Set remote address from packet */
        memcpy(&new_conn->remote_addr, &remote_addr, sizeof(remote_addr));
        new_conn->remote_addr_len = sizeof(remote_addr);

        g_dpdk_state->connections[idx] = new_conn;

        /* First, enqueue the initial packet (used to extract remote addr) to the new connection */
        /* But only if it has payload - skip SYN packets with no data */
        uint8_t tcp_hdr_len = (tcp_hdr->data_off >> 4) * 4;
        size_t total_hdr_len = sizeof(*eth_hdr) + sizeof(*ip_hdr) + tcp_hdr_len;
        size_t payload_len = rte_pktmbuf_pkt_len(mbuf) - total_hdr_len;

        if (payload_len > 0) {
            if (rte_ring_enqueue(new_conn->rx_ring, mbuf) < 0) {
                if (g_dpdk_state->debug) {
                    printf("DPDK accept: failed to enqueue first packet to new connection\n");
                }
                rte_pktmbuf_free(mbuf);
            }
        } else {
            /* No payload, just free it */
            rte_pktmbuf_free(mbuf);
        }

        /* Transfer any remaining packets from listener that match the new connection */
        struct rte_mbuf *temp_bufs[DPDK_RX_RING_SIZE];
        unsigned int temp_count = 0;
        struct rte_mbuf *pkt;

        /* Drain the listener's rx_ring temporarily */
        while (rte_ring_dequeue(conn->rx_ring, (void **)&pkt) == 0) {
            struct rte_ether_hdr *pkt_eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
            struct rte_ipv4_hdr *pkt_ip = (struct rte_ipv4_hdr *)(pkt_eth + 1);
            struct rte_tcp_hdr *pkt_tcp = (struct rte_tcp_hdr *)(pkt_ip + 1);

            /* Check if this packet is for the new connection */
            if (pkt_ip->src_addr == remote_addr.sin_addr.s_addr &&
                pkt_tcp->src_port == remote_addr.sin_port) {
                /* This packet belongs to the new connection */
                if (rte_ring_enqueue(new_conn->rx_ring, pkt) < 0) {
                    if (g_dpdk_state->debug) {
                        printf("DPDK accept: failed to transfer packet to new connection\n");
                    }
                    rte_pktmbuf_free(pkt);
                }
            } else {
                /* Keep this packet for re-queuing to listener */
                if (temp_count < DPDK_RX_RING_SIZE) {
                    temp_bufs[temp_count++] = pkt;
                } else {
                    rte_pktmbuf_free(pkt);
                }
            }
        }

        /* Re-queue packets that don't match the new connection back to listener */
        for (unsigned int i = 0; i < temp_count; i++) {
            if (rte_ring_enqueue(conn->rx_ring, temp_bufs[i]) < 0) {
                rte_pktmbuf_free(temp_bufs[i]);
            }
        }

        if (addr && addrlen) {
            socklen_t copy_len = *addrlen < sizeof(remote_addr) ? *addrlen : sizeof(remote_addr);
            memcpy(addr, &remote_addr, copy_len);
            *addrlen = copy_len;
        }

        if (g_dpdk_state->debug) {
            char remote_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &remote_addr.sin_addr, remote_ip, INET_ADDRSTRLEN);
            printf("DPDK accept: new connection fd=%d from %s:%u\n",
                   new_fd, remote_ip, rte_be_to_cpu_16(remote_addr.sin_port));
        }

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
    struct sockaddr_in *local_sin;
    static uint16_t next_ephemeral_port = 32768;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        errno = EBADF;
        return -1;
    }

    memcpy(&conn->remote_addr, addr, addrlen);
    conn->remote_addr_len = addrlen;

    /* Auto-assign local address if not already bound */
    if (conn->local_addr_len == 0 && addr->sa_family == AF_INET) {
        local_sin = (struct sockaddr_in *)&conn->local_addr;
        local_sin->sin_family = AF_INET;
        local_sin->sin_addr.s_addr = g_dpdk_state->ipv4_addr;
        local_sin->sin_port = rte_cpu_to_be_16(next_ephemeral_port++);
        conn->local_addr_len = sizeof(struct sockaddr_in);

        if (g_dpdk_state->debug) {
            char local_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &local_sin->sin_addr, local_ip, INET_ADDRSTRLEN);
            printf("DPDK auto-assigned local address: %s:%u\n",
                   local_ip, rte_be_to_cpu_16(local_sin->sin_port));
        }

        /* Wrap around if we exceed the ephemeral port range */
        if (next_ephemeral_port > 60999) {
            next_ephemeral_port = 32768;
        }
    }

    if (conn->protocol == DPDK_PROTO_TCP) {
        /* Send SYN packet */
        conn->state = DPDK_CONN_STATE_SYN_SENT;
        dpdk_send_tcp_syn(conn);

        /* In a real implementation, wait for SYN-ACK */
        conn->state = DPDK_CONN_STATE_ESTABLISHED;
        conn->connected = 1;

        if (g_dpdk_state->debug) {
            char remote_ip[INET_ADDRSTRLEN];
            struct sockaddr_in *remote_sin = (struct sockaddr_in *)&conn->remote_addr;
            inet_ntop(AF_INET, &remote_sin->sin_addr, remote_ip, INET_ADDRSTRLEN);
            printf("DPDK connect established: sockfd=%d remote=%s:%u state=%d\n",
                   sockfd, remote_ip, rte_be_to_cpu_16(remote_sin->sin_port), conn->state);
        }
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
    size_t total_sent = 0;
    size_t chunk_size;
    const char *data_ptr = (const char *)buf;
    /* Maximum segment size - MTU (1500) - IP header (20) - TCP header (20) = 1460 */
    const size_t MAX_SEGMENT_SIZE = 1460;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        if (g_dpdk_state && g_dpdk_state->debug) {
            printf("DPDK send: Bad file descriptor %d (connection not found)\n", sockfd);
        }
        errno = EBADF;
        return -1;
    }

    if (!conn->connected) {
        if (g_dpdk_state && g_dpdk_state->debug) {
            printf("DPDK send: Socket %d not connected (state=%d)\n", sockfd, conn->state);
        }
        errno = ENOTCONN;
        return -1;
    }

    if (g_dpdk_state && g_dpdk_state->debug && len > MAX_SEGMENT_SIZE) {
        printf("DPDK send: sockfd=%d len=%zu (will segment into %zu packets)\n",
               sockfd, len, (len + MAX_SEGMENT_SIZE - 1) / MAX_SEGMENT_SIZE);
    }

    /* Segment large data into multiple packets */
    while (total_sent < len) {
        chunk_size = (len - total_sent > MAX_SEGMENT_SIZE) ? MAX_SEGMENT_SIZE : (len - total_sent);

        /* Allocate mbuf */
        mbuf = rte_pktmbuf_alloc(g_dpdk_state->mbuf_pool);
        if (!mbuf) {
            if (total_sent == 0) {
                errno = ENOMEM;
                return -1;
            }
            /* Partial send is OK */
            break;
        }

        /* Create packet with this chunk */
        if (conn->protocol == DPDK_PROTO_TCP) {
            ret = dpdk_create_tcp_packet(conn, mbuf, data_ptr + total_sent, chunk_size,
                                        DPDK_TCP_FLAG_PSH | DPDK_TCP_FLAG_ACK);
        } else {
            ret = dpdk_create_udp_packet(conn, mbuf, data_ptr + total_sent, chunk_size);
        }

        if (ret < 0) {
            rte_pktmbuf_free(mbuf);
            if (total_sent == 0) {
                errno = EINVAL;
                return -1;
            }
            /* Partial send is OK */
            break;
        }

        /* Queue packet for transmission */
        if (rte_ring_enqueue(conn->tx_ring, mbuf) < 0) {
            rte_pktmbuf_free(mbuf);
            /* TX ring full, trigger burst and break */
            dpdk_tx_burst(conn->port_id);
            if (total_sent == 0) {
                errno = EAGAIN;
                return -1;
            }
            /* Partial send is OK */
            break;
        }

        total_sent += chunk_size;

        /* Trigger TX burst more frequently for lower latency */
        if (rte_ring_count(conn->tx_ring) >= 32 || total_sent >= len) {
            dpdk_tx_burst(conn->port_id);
        }
    }

    if (g_dpdk_state && g_dpdk_state->debug && len > MAX_SEGMENT_SIZE) {
        printf("DPDK send: sockfd=%d sent %zu/%zu bytes in segments\n",
               sockfd, total_sent, len);
    }

    return total_sent;
}

/* Receive data */
ssize_t dpdk_recv(int sockfd, void *buf, size_t len, int flags)
{
    struct dpdk_connection *conn;
    struct rte_mbuf *mbuf;
    ssize_t copied = 0;

    conn = dpdk_get_connection(sockfd);
    if (!conn) {
        errno = EBADF;
        return -1;
    }

    /* Process incoming packets once */
    dpdk_process_packets();

    /* Try to get packets from rx_ring and extract payload */
    while (conn->rx_buffer_offset < DPDK_RX_BUFFER_SIZE &&
           rte_ring_dequeue(conn->rx_ring, (void **)&mbuf) == 0) {
        struct rte_ether_hdr *eth_hdr;
        struct rte_ipv4_hdr *ip_hdr;
        struct rte_tcp_hdr *tcp_hdr;
        struct rte_udp_hdr *udp_hdr;
        char *payload;
        size_t payload_len;
        size_t total_hdr_len;

        /* Parse headers */
        eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

        if (conn->protocol == DPDK_PROTO_TCP) {
            tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);
            uint8_t tcp_hdr_len = (tcp_hdr->data_off >> 4) * 4;
            payload = (char *)tcp_hdr + tcp_hdr_len;
            total_hdr_len = sizeof(*eth_hdr) + sizeof(*ip_hdr) + tcp_hdr_len;
        } else {
            udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
            payload = (char *)(udp_hdr + 1);
            total_hdr_len = sizeof(*eth_hdr) + sizeof(*ip_hdr) + sizeof(*udp_hdr);
        }

        /* Calculate payload length */
        payload_len = rte_pktmbuf_pkt_len(mbuf) - total_hdr_len;

        /* Copy payload to rx_buffer if there's space */
        if (payload_len > 0 && conn->rx_buffer_offset + payload_len <= DPDK_RX_BUFFER_SIZE) {
            rte_memcpy(conn->rx_buffer + conn->rx_buffer_offset, payload, payload_len);
            conn->rx_buffer_offset += payload_len;
        }

        rte_pktmbuf_free(mbuf);
    }

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

    /* Blocking mode: wait for data to arrive */
    /* Wait up to 10 seconds for data (10000 * 1ms = 10s) */
    int attempts = 0;
    int max_attempts = 200000;
    while (attempts < max_attempts && conn->rx_buffer_offset == 0) {
        /* Process packets once per iteration to avoid mbuf exhaustion */
        dpdk_process_packets();

        /* Try to dequeue and process packets */
        while (conn->rx_buffer_offset < DPDK_RX_BUFFER_SIZE &&
               rte_ring_dequeue(conn->rx_ring, (void **)&mbuf) == 0) {
            struct rte_ether_hdr *eth_hdr;
            struct rte_ipv4_hdr *ip_hdr;
            struct rte_tcp_hdr *tcp_hdr;
            struct rte_udp_hdr *udp_hdr;
            char *payload;
            size_t payload_len;
            size_t total_hdr_len;

            eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
            ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

            if (conn->protocol == DPDK_PROTO_TCP) {
                tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);
                uint8_t tcp_hdr_len = (tcp_hdr->data_off >> 4) * 4;
                payload = (char *)tcp_hdr + tcp_hdr_len;
                total_hdr_len = sizeof(*eth_hdr) + sizeof(*ip_hdr) + tcp_hdr_len;
            } else {
                udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
                payload = (char *)(udp_hdr + 1);
                total_hdr_len = sizeof(*eth_hdr) + sizeof(*ip_hdr) + sizeof(*udp_hdr);
            }

            payload_len = rte_pktmbuf_pkt_len(mbuf) - total_hdr_len;

            if (payload_len > 0 && conn->rx_buffer_offset + payload_len <= DPDK_RX_BUFFER_SIZE) {
                rte_memcpy(conn->rx_buffer + conn->rx_buffer_offset, payload, payload_len);
                conn->rx_buffer_offset += payload_len;
            }

            rte_pktmbuf_free(mbuf);
        }

        if (conn->rx_buffer_offset > 0) {
            break;
        }

        attempts++;
        /* Short busy-wait to improve throughput while still yielding */
        rte_delay_us_block(50);
    }

    /* Check if we got data after waiting */
    if (conn->rx_buffer_offset > 0) {
        copied = (conn->rx_buffer_offset < len) ? conn->rx_buffer_offset : len;

        memcpy(buf, conn->rx_buffer, copied);

        if (copied < conn->rx_buffer_offset) {
            memmove(conn->rx_buffer, conn->rx_buffer + copied,
                    conn->rx_buffer_offset - copied);
        }
        conn->rx_buffer_offset -= copied;

        return copied;
    }

    /* Still no data after waiting */
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
    int i, idx;

    nb_rx = rte_eth_rx_burst(port_id, 0, bufs, DPDK_MAX_RX_BURST);

    g_dpdk_state->rx_packets += nb_rx;

    for (i = 0; i < nb_rx; i++) {
        struct rte_ether_hdr *eth_hdr;
        struct rte_ipv4_hdr *ip_hdr;
        struct rte_tcp_hdr *tcp_hdr;
        struct rte_udp_hdr *udp_hdr;
        uint16_t src_port, dst_port;
        uint8_t protocol;
        int matched = 0;

        g_dpdk_state->rx_bytes += rte_pktmbuf_pkt_len(bufs[i]);

        /* Dump packet if debug enabled (only small control packets < 100 bytes) */
        if (g_dpdk_state->packet_dump && rte_pktmbuf_pkt_len(bufs[i]) < 100) {
            dpdk_dump_packet("RX", bufs[i]);
        }

        /* Parse packet headers */
        eth_hdr = rte_pktmbuf_mtod(bufs[i], struct rte_ether_hdr *);
        if (rte_be_to_cpu_16(eth_hdr->ether_type) != RTE_ETHER_TYPE_IPV4) {
            rte_pktmbuf_free(bufs[i]);
            continue;
        }

        ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
        protocol = ip_hdr->next_proto_id;

        if (protocol == DPDK_PROTO_TCP) {
            tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);
            src_port = rte_be_to_cpu_16(tcp_hdr->src_port);
            dst_port = rte_be_to_cpu_16(tcp_hdr->dst_port);
        } else if (protocol == DPDK_PROTO_UDP) {
            udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
            src_port = rte_be_to_cpu_16(udp_hdr->src_port);
            dst_port = rte_be_to_cpu_16(udp_hdr->dst_port);
        } else {
            rte_pktmbuf_free(bufs[i]);
            continue;
        }

        /* Route packet to appropriate connection */
        /* Debug: Show all connections when routing TCP packets */
        if (g_dpdk_state->debug && protocol == DPDK_PROTO_TCP) {
            struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip_hdr + 1);
            printf("DPDK RX: Routing seq=%u from %u.%u.%u.%u:%u to %u.%u.%u.%u:%u\n",
                   rte_be_to_cpu_32(tcp->sent_seq),
                   (ip_hdr->src_addr >> 0) & 0xFF, (ip_hdr->src_addr >> 8) & 0xFF,
                   (ip_hdr->src_addr >> 16) & 0xFF, (ip_hdr->src_addr >> 24) & 0xFF,
                   src_port,
                   (ip_hdr->dst_addr >> 0) & 0xFF, (ip_hdr->dst_addr >> 8) & 0xFF,
                   (ip_hdr->dst_addr >> 16) & 0xFF, (ip_hdr->dst_addr >> 24) & 0xFF,
                   dst_port);
            printf("DPDK RX: Connection table:\n");
            for (int i = 0; i < DPDK_MAX_CONNECTIONS; i++) {
                struct dpdk_connection *c = g_dpdk_state->connections[i];
                if (c) {
                    struct sockaddr_in *r = (struct sockaddr_in *)&c->remote_addr;
                    struct sockaddr_in *l = (struct sockaddr_in *)&c->local_addr;
                    printf("  [%d] fd=%d connected=%d listening=%d local=%u.%u.%u.%u:%u remote=%u.%u.%u.%u:%u\n",
                           i, c->fd, c->connected, c->listening,
                           (l->sin_addr.s_addr >> 0) & 0xFF, (l->sin_addr.s_addr >> 8) & 0xFF,
                           (l->sin_addr.s_addr >> 16) & 0xFF, (l->sin_addr.s_addr >> 24) & 0xFF,
                           ntohs(l->sin_port),
                           (r->sin_addr.s_addr >> 0) & 0xFF, (r->sin_addr.s_addr >> 8) & 0xFF,
                           (r->sin_addr.s_addr >> 16) & 0xFF, (r->sin_addr.s_addr >> 24) & 0xFF,
                           ntohs(r->sin_port));
                }
            }
        }

        /* Check established connections first (more specific match) */
        for (idx = 0; idx < DPDK_MAX_CONNECTIONS; idx++) {
            struct dpdk_connection *conn = g_dpdk_state->connections[idx];
            if (!conn || !conn->connected) {
                continue;
            }

            struct sockaddr_in *remote_sin = (struct sockaddr_in *)&conn->remote_addr;
            struct sockaddr_in *local_sin = (struct sockaddr_in *)&conn->local_addr;

            if (ip_hdr->src_addr == remote_sin->sin_addr.s_addr &&
                src_port == ntohs(remote_sin->sin_port) &&
                dst_port == ntohs(local_sin->sin_port)) {
                /* This packet is for this connection */
                if (g_dpdk_state->debug && protocol == DPDK_PROTO_TCP) {
                    struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip_hdr + 1);
                    printf("DPDK RX: Routed seq=%u to ESTABLISHED connection fd=%d\n",
                           rte_be_to_cpu_32(tcp->sent_seq), conn->fd);
                }
                /* Try to copy payload directly into the connection buffer to free mbufs quickly */
                size_t total_hdr_len = 0;
                size_t payload_len = 0;
                char *payload = NULL;

                if (protocol == DPDK_PROTO_TCP) {
                    uint8_t tcp_hdr_len = (tcp_hdr->data_off >> 4) * 4;
                    total_hdr_len = sizeof(*eth_hdr) + sizeof(*ip_hdr) + tcp_hdr_len;
                    payload = (char *)tcp_hdr + tcp_hdr_len;
                } else {
                    total_hdr_len = sizeof(*eth_hdr) + sizeof(*ip_hdr) + sizeof(*udp_hdr);
                    payload = (char *)(udp_hdr + 1);
                }

                if (rte_pktmbuf_pkt_len(bufs[i]) >= total_hdr_len) {
                    payload_len = rte_pktmbuf_pkt_len(bufs[i]) - total_hdr_len;
                }

                if (payload_len > 0 &&
                    conn->rx_buffer_offset + payload_len <= conn->rx_buffer_size) {
                    rte_memcpy(conn->rx_buffer + conn->rx_buffer_offset, payload, payload_len);
                    conn->rx_buffer_offset += payload_len;
                    rte_pktmbuf_free(bufs[i]);
                } else if (payload_len == 0) {
                    rte_pktmbuf_free(bufs[i]);
                } else {
                    if (rte_ring_enqueue(conn->rx_ring, bufs[i]) < 0) {
                        if (g_dpdk_state->debug) {
                            printf("DPDK RX: rx_ring full for connection %d\n", conn->fd);
                        }
                        rte_pktmbuf_free(bufs[i]);
                    }
                }
                matched = 1;
                break;
            }
        }

        /* If no established connection matched, check listening sockets */
        if (!matched) {
            for (idx = 0; idx < DPDK_MAX_CONNECTIONS; idx++) {
                struct dpdk_connection *conn = g_dpdk_state->connections[idx];
                if (!conn || !conn->listening || protocol != DPDK_PROTO_TCP) {
                    continue;
                }

                struct sockaddr_in *local_sin = (struct sockaddr_in *)&conn->local_addr;
                if (dst_port == ntohs(local_sin->sin_port)) {
                    /* This is for our listening socket */
                    if (g_dpdk_state->debug) {
                        struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip_hdr + 1);
                        printf("DPDK RX: Routed seq=%u to LISTENING socket fd=%d\n",
                               rte_be_to_cpu_32(tcp->sent_seq), conn->fd);
                    }
                    if (rte_ring_enqueue(conn->rx_ring, bufs[i]) < 0) {
                        if (g_dpdk_state->debug) {
                            printf("DPDK RX: rx_ring full for listening socket %d\n", conn->fd);
                        }
                        rte_pktmbuf_free(bufs[i]);
                    }
                    matched = 1;
                    break;
                }
            }
        }

        if (!matched) {
            /* No matching connection found */
            if (g_dpdk_state->debug) {
                char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &ip_hdr->src_addr, src_ip, INET_ADDRSTRLEN);
                inet_ntop(AF_INET, &ip_hdr->dst_addr, dst_ip, INET_ADDRSTRLEN);
                printf("DPDK RX: No matching connection for %s:%u -> %s:%u proto=%u\n",
                       src_ip, src_port, dst_ip, dst_port, protocol);
            }
            rte_pktmbuf_free(bufs[i]);
        }
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
        /* Dump packets if debug enabled (only control packets, not bulk data) */
        if (g_dpdk_state->packet_dump) {
            for (i = 0; i < nb_tx; i++) {
                /* Only dump small control packets (< 100 bytes) to avoid performance impact */
                if (rte_pktmbuf_pkt_len(bufs[i]) < 100) {
                    dpdk_dump_packet("TX", bufs[i]);
                }
            }
        }

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
    /* For now, use broadcast MAC - proper implementation would need ARP resolution */
    memset(&eth_hdr->dst_addr, 0xff, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    /* Extract remote IP and port from sockaddr */
    struct sockaddr_in *remote_sin = (struct sockaddr_in *)&conn->remote_addr;
    struct sockaddr_in *local_sin = (struct sockaddr_in *)&conn->local_addr;
    uint32_t dst_ip = remote_sin->sin_addr.s_addr;
    uint16_t src_port = local_sin->sin_port;
    uint16_t dst_port = remote_sin->sin_port;

    /* Fill IP header - simplified */
    ip_hdr->version_ihl = 0x45; /* IPv4, 20 byte header */
    ip_hdr->type_of_service = 0;
    ip_hdr->total_length = rte_cpu_to_be_16(sizeof(*ip_hdr) + sizeof(*tcp_hdr) + len);
    ip_hdr->packet_id = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = 64;
    ip_hdr->next_proto_id = DPDK_PROTO_TCP;
    ip_hdr->hdr_checksum = 0;
    ip_hdr->src_addr = g_dpdk_state->ipv4_addr;
    ip_hdr->dst_addr = dst_ip;
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

    /* Fill TCP header - simplified */
    tcp_hdr->src_port = src_port;
    tcp_hdr->dst_port = dst_port;
    tcp_hdr->sent_seq = rte_cpu_to_be_32(conn->seq_num);
    tcp_hdr->recv_ack = rte_cpu_to_be_32(conn->ack_num);
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

    /* Extract remote IP and port from sockaddr */
    struct sockaddr_in *remote_sin = (struct sockaddr_in *)&conn->remote_addr;
    struct sockaddr_in *local_sin = (struct sockaddr_in *)&conn->local_addr;
    uint32_t dst_ip = remote_sin->sin_addr.s_addr;
    uint16_t src_port = local_sin->sin_port;
    uint16_t dst_port = remote_sin->sin_port;

    /* Fill headers - simplified */
    memcpy(&eth_hdr->src_addr, &g_dpdk_state->mac_addr, RTE_ETHER_ADDR_LEN);
    /* For now, use broadcast MAC - proper implementation would need ARP resolution */
    memset(&eth_hdr->dst_addr, 0xff, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    ip_hdr->version_ihl = 0x45;
    ip_hdr->type_of_service = 0;
    ip_hdr->total_length = rte_cpu_to_be_16(sizeof(*ip_hdr) + sizeof(*udp_hdr) + len);
    ip_hdr->packet_id = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = 64;
    ip_hdr->next_proto_id = DPDK_PROTO_UDP;
    ip_hdr->hdr_checksum = 0;
    ip_hdr->src_addr = g_dpdk_state->ipv4_addr;
    ip_hdr->dst_addr = dst_ip;
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

    udp_hdr->src_port = src_port;
    udp_hdr->dst_port = dst_port;
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

/* Dump packet contents for debugging */
void dpdk_dump_packet(const char *prefix, struct rte_mbuf *mbuf)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_tcp_hdr *tcp_hdr;
    struct rte_udp_hdr *udp_hdr;
    uint8_t *data;
    uint16_t ether_type;
    uint8_t ip_proto;
    int i;

    if (!mbuf || !g_dpdk_state) {
        return;
    }

    /* Get Ethernet header */
    eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    ether_type = rte_be_to_cpu_16(eth_hdr->ether_type);

    printf("=== %s Packet (len=%u) ===\n", prefix, rte_pktmbuf_pkt_len(mbuf));
    printf("Ethernet: src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x type=0x%04x\n",
           eth_hdr->src_addr.addr_bytes[0], eth_hdr->src_addr.addr_bytes[1],
           eth_hdr->src_addr.addr_bytes[2], eth_hdr->src_addr.addr_bytes[3],
           eth_hdr->src_addr.addr_bytes[4], eth_hdr->src_addr.addr_bytes[5],
           eth_hdr->dst_addr.addr_bytes[0], eth_hdr->dst_addr.addr_bytes[1],
           eth_hdr->dst_addr.addr_bytes[2], eth_hdr->dst_addr.addr_bytes[3],
           eth_hdr->dst_addr.addr_bytes[4], eth_hdr->dst_addr.addr_bytes[5],
           ether_type);

    /* Check if IPv4 */
    if (ether_type == RTE_ETHER_TYPE_IPV4) {
        ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
        ip_proto = ip_hdr->next_proto_id;

        char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip_hdr->src_addr, src_ip, INET_ADDRSTRLEN);
        inet_ntop(AF_INET, &ip_hdr->dst_addr, dst_ip, INET_ADDRSTRLEN);

        printf("IPv4: src=%s dst=%s proto=%u ttl=%u len=%u\n",
               src_ip, dst_ip, ip_proto, ip_hdr->time_to_live,
               rte_be_to_cpu_16(ip_hdr->total_length));

        /* Check protocol */
        if (ip_proto == DPDK_PROTO_TCP) {
            tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);
            printf("TCP: sport=%u dport=%u seq=%u ack=%u flags=0x%02x win=%u\n",
                   rte_be_to_cpu_16(tcp_hdr->src_port),
                   rte_be_to_cpu_16(tcp_hdr->dst_port),
                   rte_be_to_cpu_32(tcp_hdr->sent_seq),
                   rte_be_to_cpu_32(tcp_hdr->recv_ack),
                   tcp_hdr->tcp_flags,
                   rte_be_to_cpu_16(tcp_hdr->rx_win));
        } else if (ip_proto == DPDK_PROTO_UDP) {
            udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
            printf("UDP: sport=%u dport=%u len=%u\n",
                   rte_be_to_cpu_16(udp_hdr->src_port),
                   rte_be_to_cpu_16(udp_hdr->dst_port),
                   rte_be_to_cpu_16(udp_hdr->dgram_len));
        }
    }

    /* Dump first 64 bytes of packet data in hex */
    data = rte_pktmbuf_mtod(mbuf, uint8_t *);
    printf("Data (first 64 bytes): ");
    for (i = 0; i < 64 && i < (int)rte_pktmbuf_pkt_len(mbuf); i++) {
        if (i > 0 && i % 16 == 0) {
            printf("\n                       ");
        }
        printf("%02x ", data[i]);
    }
    printf("\n");
}

/* Enable or disable packet dumping */
void dpdk_enable_packet_dump(int enable)
{
    if (g_dpdk_state) {
        g_dpdk_state->packet_dump = enable;
        if (enable) {
            printf("DPDK packet dumping enabled\n");
        } else {
            printf("DPDK packet dumping disabled\n");
        }
    }
}

/* Wrapper for read() that detects DPDK sockets */
ssize_t dpdk_wrapped_read(int fd, void *buf, size_t count)
{
    /* DPDK sockets use fd >= 100 */
    if (fd >= 100 && g_dpdk_state) {
        struct dpdk_connection *conn = dpdk_get_connection(fd);
        if (conn) {
            return dpdk_recv(fd, buf, count, 0);
        }
    }

    /* Not a DPDK socket, use real system call */
    return read(fd, buf, count);
}

/* Wrapper for write() that detects DPDK sockets */
ssize_t dpdk_wrapped_write(int fd, const void *buf, size_t count)
{
    /* DPDK sockets use fd >= 100 */
    if (fd >= 100 && g_dpdk_state) {
        struct dpdk_connection *conn = dpdk_get_connection(fd);
        if (conn) {
            return dpdk_send(fd, buf, count, 0);
        }
    }

    /* Not a DPDK socket, use real system call */
    return write(fd, buf, count);
}

/* Wrapper for select() that handles DPDK sockets */
int dpdk_wrapped_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
    fd_set dpdk_readfds, dpdk_writefds, regular_readfds, regular_writefds;
    int fd, regular_nfds = 0;
    int dpdk_ready = 0, regular_ready = 0;
    int has_dpdk_sockets = 0, has_regular_sockets = 0;
    struct timeval short_timeout = {0, 1000}; /* 1ms for DPDK polling */
    struct timeval start_time, current_time;
    uint64_t timeout_us = 0;
    uint64_t elapsed_us = 0;
    int poll_iterations = 0;
    int spin_iterations = 0;
    const int spin_limit = 64;

    if (!g_dpdk_state) {
        /* No DPDK initialized, use regular select */
        return select(nfds, readfds, writefds, exceptfds, timeout);
    }

    /* Calculate timeout in microseconds */
    if (timeout) {
        timeout_us = (uint64_t)timeout->tv_sec * 1000000 + timeout->tv_usec;
        gettimeofday(&start_time, NULL);
    }

    /* Initialize fd_sets */
    FD_ZERO(&dpdk_readfds);
    FD_ZERO(&dpdk_writefds);
    FD_ZERO(&regular_readfds);
    FD_ZERO(&regular_writefds);

    /* Separate DPDK and regular sockets */
    for (fd = 0; fd < nfds; fd++) {
        struct dpdk_connection *conn = NULL;
        int is_dpdk = 0;

        if (fd >= 100) {
            conn = dpdk_get_connection(fd);
            is_dpdk = (conn != NULL);
        }

        if (is_dpdk) {
            has_dpdk_sockets = 1;
            if (readfds && FD_ISSET(fd, readfds)) {
                FD_SET(fd, &dpdk_readfds);
            }
            if (writefds && FD_ISSET(fd, writefds)) {
                FD_SET(fd, &dpdk_writefds);
            }
        } else {
            if (readfds && FD_ISSET(fd, readfds)) {
                FD_SET(fd, &regular_readfds);
                has_regular_sockets = 1;
                if (fd >= regular_nfds) regular_nfds = fd + 1;
            }
            if (writefds && FD_ISSET(fd, writefds)) {
                FD_SET(fd, &regular_writefds);
                has_regular_sockets = 1;
                if (fd >= regular_nfds) regular_nfds = fd + 1;
            }
        }
    }

    /* Poll for DPDK sockets until timeout or data arrives */
    while (1) {
        dpdk_ready = 0;

        /* Process DPDK packets if we have DPDK sockets */
        if (has_dpdk_sockets) {
            /* Single process call to avoid mbuf exhaustion */
            dpdk_process_packets();

            /* Check DPDK sockets for readiness */
            for (fd = 100; fd < nfds; fd++) {
                struct dpdk_connection *conn = dpdk_get_connection(fd);
                if (!conn) continue;

                /* Check if socket is readable (has data in rx_ring or rx_buffer) */
                if (FD_ISSET(fd, &dpdk_readfds)) {
                    unsigned int count = 0;
                    if (conn->rx_ring) {
                        count = rte_ring_count(conn->rx_ring);
                    }
                    if (count > 0 || conn->rx_buffer_offset > 0) {
                        dpdk_ready++;
                        if (readfds) FD_SET(fd, readfds);
                    } else {
                        if (readfds) FD_CLR(fd, readfds);
                    }
                }

                /* Check if socket is writable (connected and tx_ring not full) */
                if (FD_ISSET(fd, &dpdk_writefds)) {
                    if (conn->connected) {
                        dpdk_ready++;
                        if (writefds) FD_SET(fd, writefds);
                    } else {
                        if (writefds) FD_CLR(fd, writefds);
                    }
                }
            }

            /* If DPDK sockets are ready, we can return immediately */
            if (dpdk_ready > 0) {
                break;
            }
        }

        /* Check timeout if specified */
        if (timeout) {
            gettimeofday(&current_time, NULL);
            elapsed_us = (current_time.tv_sec - start_time.tv_sec) * 1000000 +
                        (current_time.tv_usec - start_time.tv_usec);

            if (elapsed_us >= timeout_us) {
                /* Timeout expired */
                break;
            }
        }

        poll_iterations++;

        /* Busy-poll briefly to reduce latency, then yield if still idle */
        if (spin_iterations < spin_limit) {
            rte_delay_us_block(1);
            spin_iterations++;
        } else {
            usleep(1000);
            spin_iterations = 0;
        }
    }

    /* Handle regular sockets with select() if any */
    if (has_regular_sockets) {
        /* Use a short timeout if we have DPDK sockets to check */
        struct timeval *select_timeout = has_dpdk_sockets ? &short_timeout : timeout;
        regular_ready = select(regular_nfds,
                              has_regular_sockets ? &regular_readfds : NULL,
                              has_regular_sockets ? &regular_writefds : NULL,
                              exceptfds,
                              select_timeout);

        if (regular_ready < 0) {
            return regular_ready; /* Error from select */
        }

        /* Merge regular socket results back */
        if (regular_ready > 0) {
            for (fd = 0; fd < regular_nfds; fd++) {
                if (readfds && FD_ISSET(fd, &regular_readfds)) {
                    FD_SET(fd, readfds);
                }
                if (writefds && FD_ISSET(fd, &regular_writefds)) {
                    FD_SET(fd, writefds);
                }
            }
        }
    }

    return dpdk_ready + regular_ready;
}
