/*
 * iperf, Copyright (c) 2014-2025, The Regents of the University of
 * California, through Lawrence Berkeley National Laboratory (subject
 * to receipt of any required approvals from the U.S. Dept. of
 * Energy).  All rights reserved.
 *
 * Socket wrapper for DPDK compatibility
 */
#ifndef __SOCKET_WRAPPER_H
#define __SOCKET_WRAPPER_H

#ifdef HAVE_DPDK

#include "dpdk_net.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

/* Forward declarations for wrapper functions */
ssize_t dpdk_wrapped_read(int fd, void *buf, size_t count);
ssize_t dpdk_wrapped_write(int fd, const void *buf, size_t count);
int dpdk_wrapped_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);

/* Replace standard socket functions with DPDK equivalents */
#define socket(domain, type, protocol) dpdk_socket(domain, type, protocol)
#define bind(sockfd, addr, addrlen) dpdk_bind(sockfd, addr, addrlen)
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

/*
 * For listen, accept, connect: use macros like other socket functions
 * Function-like macros only expand when followed by '(', so they won't
 * interfere with protocol struct members like 'protocol->listen'
 */
#define listen(sockfd, backlog) dpdk_listen(sockfd, backlog)
#define accept(sockfd, addr, addrlen) dpdk_accept(sockfd, addr, addrlen)
#define connect(sockfd, addr, addrlen) dpdk_connect(sockfd, addr, addrlen)

/* Wrap read/write to detect DPDK sockets */
#define read(fd, buf, count) dpdk_wrapped_read(fd, buf, count)
#define write(fd, buf, count) dpdk_wrapped_write(fd, buf, count)

/* Wrap select to handle DPDK sockets */
#define select(nfds, readfds, writefds, exceptfds, timeout) \
    dpdk_wrapped_select(nfds, readfds, writefds, exceptfds, timeout)

#endif /* HAVE_DPDK */

#endif /* __SOCKET_WRAPPER_H */
