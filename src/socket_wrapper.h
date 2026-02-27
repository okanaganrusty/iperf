/*
 * iperf, Copyright (c) 2014-2025, The Regents of the University of
 * California, through Lawrence Berkeley National Laboratory (subject
 * to receipt of any required approvals from the U.S. Dept. of
 * Energy).  All rights reserved.
 *
 * Socket wrapper macros for DPDK compatibility
 */
#ifndef __SOCKET_WRAPPER_H
#define __SOCKET_WRAPPER_H

#ifdef HAVE_DPDK

#include "dpdk_net.h"

/* Replace standard socket functions with DPDK equivalents */
#define socket(domain, type, protocol) dpdk_socket(domain, type, protocol)
#define bind(sockfd, addr, addrlen) dpdk_bind(sockfd, addr, addrlen)
#define listen(sockfd, backlog) dpdk_listen(sockfd, backlog)
#define accept(sockfd, addr, addrlen) dpdk_accept(sockfd, addr, addrlen)
#define connect(sockfd, addr, addrlen) dpdk_connect(sockfd, addr, addrlen)
#define send(sockfd, buf, len, flags) dpdk_send(sockfd, buf, len, flags)
#define recv(sockfd, buf, len, flags) dpdk_recv(sockfd, buf, len, flags)
#define sendto(sockfd, buf, len, flags, dest_addr, addrlen) dpdk_sendto(sockfd, buf, len, flags, dest_addr, addrlen)
#define recvfrom(sockfd, buf, len, flags, src_addr, addrlen) dpdk_recvfrom(sockfd, buf, len, flags, src_addr, addrlen)
#define close(sockfd) dpdk_close(sockfd)
#define setsockopt(sockfd, level, optname, optval, optlen) dpdk_setsockopt(sockfd, level, optname, optval, optlen)
#define getsockopt(sockfd, level, optname, optval, optlen) dpdk_getsockopt(sockfd, level, optname, optval, optlen)
#define getsockname(sockfd, addr, addrlen) dpdk_getsockname(sockfd, addr, addrlen)
#define getpeername(sockfd, addr, addrlen) dpdk_getpeername(sockfd, addr, addrlen)
#define fcntl(sockfd, cmd, ...) dpdk_fcntl(sockfd, cmd, ##__VA_ARGS__)

/* Special handling for read/write as they're also used for files */
#define socket_read(sockfd, buf, count) dpdk_recv(sockfd, buf, count, 0)
#define socket_write(sockfd, buf, count) dpdk_send(sockfd, buf, count, 0)

#endif /* HAVE_DPDK */

#endif /* __SOCKET_WRAPPER_H */
