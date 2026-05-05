/* SPDX-License-Identifier: MIT | (c) 2026 [cof] */

/*
 * SOCK - A simple socket layer API
 * --------------------------------
 * See sock.h for API description.
 *
 * API sections
 * ------------
 * Init       : Init sock state
 * Connection : Create or close client|server socket connections
 * State      : Update socket mode or fd state
 * FD I/O     : Read|Write memory buffers to|from file descriptor
 * buffer I/O : send|recv sock buffers to|from file descriptor
 * line   I/O : Read and write lines
 * Status     : Socket status and info
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "util.h"
#include "log.h"
#include "rwbuf.h"
#include "dns_resolv.h"
#include "sock.h"

// resolv mode,hostname,port to array of dns_sockaddr
static int sock_resolv(uint32_t mode,
    const char *hostname, const char *port,
    int max_addr, struct dns_sockaddr addrs[max_addr])
{
    uint32_t flags = 0;

    if (mode & SOCK_PASSIVE) flags |= DNS_PASSIVE;
    if (mode & SOCK_NUMPORT) flags |= DNS_NUMPORT;

    // addr
    if (mode & SOCK_ANY) {
        flags |= DNS_IPV6 | DNS_V4MAPPED | DNS_ALL;
    }
    else if (mode & SOCK_IPV4) {
        flags |= DNS_IPV4;
    }
    else if (mode & SOCK_IPV6) {
        flags |= DNS_IPV6;
    }

    if (mode & SOCK_TCP) flags |= DNS_TCP;
    if (mode & SOCK_UDP) flags |= DNS_UDP;

    return dns_resolv(flags, hostname, port, max_addr, addrs);
}

// load sock_addr from sa
static int sock_addr_load(struct sock_addr *addr, struct sockaddr *sa, socklen_t sa_len)
{
    memset(addr, 0, sizeof(*addr));

    switch(sa->sa_family) {
    case AF_INET:
        struct sockaddr_in *sin =  (void *) sa;
        if (sa_len != sizeof(*sin)) break;
        addr->type   = SOCK_IPV4;
        addr->port   = sin->sin_port;
        addr->u32[0] = sin->sin_addr.s_addr;
        return SOCK_IPV4;
    case AF_INET6:
        struct sockaddr_in6 *sin6  = (void *) sa;
        if (sa_len != sizeof(*sin6)) break;
        addr->type = SOCK_IPV6;
        addr->port = sin6->sin6_port;
        memcpy(&addr->v6, &sin6->sin6_addr, 16);
        return SOCK_IPV6;
    }

    // not supported
    return 0;
}

// create sockect fd + connect to addr
static int connect_addr(struct simple_sock *sock, struct dns_sockaddr *addr)
{
    int rc = sock_addr_load(&sock->addr, &addr->sa, addr->len);
    if (rc == 0) return log_error_rf("Unsupported addr %d", addr->len);

    int domain = addr->sa.sa_family;
    int type = addr->sock_type;
    sock->fd = socket(domain, type, 0);

    log_debug("a=%s t=%s fd=%d", dns_sockaddr_tostr(addr), dns_socktype_tostr(addr), sock->fd);
    if (sock->fd == -1) return log_errno_rf("socket(%d,%d) failed", domain, type);

    // need-connect
    if (!(sock->mode & (SOCK_TCP | SOCK_UDPCON))) return 0;

    // connect to addr
    rc = connect(sock->fd, &addr->sa, addr->len);
    if (rc == -1) return sock_close(sock, rc);

    if (sock->mode & SOCK_NONBLK) {
        rc = sock_set_nonblk(sock);
        if (rc) return sock_close(sock, rc);
    }

    // connected
    return 0;
}

// create socket-fd + listen on resolved address
static int listen_addr(struct simple_sock *sock, struct dns_sockaddr *addr)
{
    int rc = sock_addr_load(&sock->addr, &addr->sa, addr->len);
    if (rc == 0) return log_error_rf("Unsupported addr %d", addr->len);

    // create socket
    int domain = addr->sa.sa_family;
    int type = addr->sock_type;
    if (sock->mode & SOCK_NONBLK) type |= SOCK_NONBLOCK;
    sock->fd = socket(domain, type, 0);

    log_debug("addr=%s type=%s fd=%d",
        dns_sockaddr_tostr(addr), dns_socktype_tostr(addr), sock->fd);
    if (sock->fd == -1) return log_errno_rf("socket(%d,%d) failed", domain, type);

    // dual-stack
    if (sock->mode & SOCK_ANY) {
        int opt = 0;
        rc = setsockopt(sock->fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
        if (rc == -1) log_errno("disable IPV6_ONLY");
    }

    // resuse-addr
    if (sock->mode & SOCK_REUSE) {
        int opt = 1;
        rc = setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (rc == -1) log_errno("enable SO_REUSEADDR");
    }

    // server
    if (sock->mode & SOCK_PASSIVE) {
        // bind to address
        if (bind(sock->fd, &addr->sa, addr->len) == -1) {
            rc = log_errno_rf("bind fd=%d addr=%s failed", sock->fd, dns_sockaddr_tostr(addr));
            return sock_close(sock, rc);
        }
        // listen for incoming connectons
        if (listen(sock->fd, SOMAXCONN) == -1) {
            rc = log_errno_rf("listen fd=%d failed", sock->fd);
            return sock_close(sock, rc);
        }
    }

    // all done
    return 0;
}

// init state for new or accepted file descriptor
int sock_init(struct simple_sock *sock,
    int fd, struct sock_addr *addr,
    size_t max_line, size_t buf_size,
    size_t min_size, size_t max_size)
{
    sock->fd = fd;
    if (addr) sock->addr = *addr;
    sock->max_line = max_line;
    sock->min_size = min_size;

    int rc;
    if ((rc = rwbuf_init(&sock->recv_buf, buf_size, max_size))) return rc;
    if ((rc = rwbuf_init(&sock->send_buf, buf_size, max_size))) return rc;

    return 0;
}

// close sock free memory
void sock_deinit(struct simple_sock *sock, int rc)
{
    sock_close(sock, rc);
    rwbuf_deinit(&sock->send_buf);
    rwbuf_deinit(&sock->recv_buf);
}

// create a client - e.g sock_connect(sock, SOCK_TCP, "example.com", 80)
int sock_client(struct simple_sock *sock, uint32_t mode,
    const char *hostname, const char *port)
{
    // resolv hostname+port
    struct dns_sockaddr addrs[DNS_MAXADDR];
    int num_addr = sock_resolv(mode, hostname, port, ARRAY(addrs));
    if (num_addr < 0) return num_addr;
    if (num_addr == 0) return log_error_rf("resolv(%s,%s) not-found", hostname, port);

    // find working addr
    sock->mode = mode;
    sock->fd = -1;
    for (int i = 0; i < num_addr; i++) {
        if (connect_addr(sock, &addrs[i]) == 0) break;
    }
    if (sock->fd == -1) return log_errno_rf("connect(%s,%s) failed", hostname, port);

    // all done
    return 0;
}

// create a server - e.g sock_server(sock, SOCK_TCP, "", 80);
int sock_server(struct simple_sock *sock, uint32_t mode,
    const char *hostname, const char *port)
{
    // resolv hostname+port
    struct dns_sockaddr addrs[DNS_MAXADDR];
    int num_addr = sock_resolv(mode, hostname, port, ARRAY(addrs));
    if (num_addr <= 0) return num_addr;
    if (num_addr == 0) return log_error_rf("resolv(%s,%s) not-found", hostname, port);

    sock->mode = mode;
    sock->fd = -1;

    return listen_addr(sock, &addrs[0]);
}

int sock_accept(struct simple_sock *sock, struct sock_addr *addr)
{
    struct sockaddr_storage store;
    socklen_t store_len = sizeof(store);

    // accept new connection
    int flags = sock->mode & SOCK_NONBLK ? SOCK_NONBLOCK : 0;
    int fd = accept4(sock->fd, (struct sockaddr *) &store, &store_len, flags);
    if (fd == -1) {
        /// EAGAIN|EWOULDBLOCK - means no more pending accepts ..
        return -1;
    }

    // store addr,fd
    int rc = sock_addr_load(addr, (struct sockaddr *) &store, store_len);
    if (rc == 0) {
        close(fd);
        return log_error_rf("Unsupported addr %d", store_len);
    }

    // turn off nagle
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    return fd;
}

// shutdown writes on socket
int sock_sendfin(struct simple_sock *sock)
{
    if (sock->sys_err) return SOCK_ERROR;
    if (sock->fin_sent) return 0;

    int rc = shutdown(sock->fd, SHUT_WR);
    if (rc != 0 && errno != ENOTSOCK) {
        sock->sys_err = 1;
        return log_errno_rf("shutdown write fd=%d failed", sock->fd);
    }

    sock->fin_sent = 1;
    if (sock->send_fin) {
        // send done
        sock->send_fin = 0;
    }

    return 0;
}

// close the socket fd
int sock_close(struct simple_sock *sock, int rc)
{
    if (sock->fd != -1) {
        int _errno = errno;
        int ec = close(sock->fd);
        if (ec && rc == 0) {
            log_error("close fd=%d failed", sock->fd);
            rc = ec;
            _errno = errno;
        }
        sock->fd = -1;
        errno = _errno;
    }

    return rc;
}

/*
 * Change fd state
 * -----------------------
 * sock_set_mode   - change socket mode flags
 * sock_set_nonblk - set socket non blocking
 * sock_set_sndto  - set socket send timeout in ms
 * sock_set_rcvto  - set socket recv timeout in ms:
 */
int sock_set_mode(struct simple_sock *sock, uint32_t mode)
{
    if (mode & SOCK_NONBLK && (sock->mode & SOCK_NONBLK) == 0) {
        int rc = sock_set_nonblk(sock);
        if (rc) return rc;
    }
    sock->mode = mode;
    return 0;
}

// set non-blocking
int sock_set_nonblk(struct simple_sock *sock)
{
    int flags = fcntl(sock->fd, F_GETFL, 0);
    if (flags == -1) return log_errno_rf("fcntl %d GETFL failed", sock->fd);

    flags |= O_NONBLOCK;

    int rc = fcntl(sock->fd, F_SETFL, flags);
    if (rc == -1) return log_errno_rf("fcntl %d SETFL 0x%x failed", sock->fd, flags);

    return 0;
}

// set fd send timeout
int sock_set_sndto(struct simple_sock *sock, uint32_t ms)
{
    struct timeval tv = {
        .tv_sec = ms / 1000,
        .tv_usec = (ms % 1000) * 1000
    };

    int rc = setsockopt(sock->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (rc) return log_errno_rf("set SO_SNDTIMEO on %d failed", sock->fd);

    sock->send_timeout = 1;

    return 0;
}

// set fd recv timeout
int sock_set_rcvto(struct simple_sock *sock, uint32_t ms)
{
    struct timeval tv = {
        .tv_sec = ms / 1000,
        .tv_usec = (ms % 1000) * 1000
    };

    int rc = setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (rc) return log_errno_rf("set SO_RCVTIMEO, on %d failed", sock->fd);

    sock->recv_timeout = 1;

    return 0;
}

// read into data-buffer from fd
ssize_t sock_read_data(struct simple_sock *sock, void *data, size_t len)
{
    // joker checks
    if (sock->sys_err) return SOCK_ERROR;
    if (sock->recv_fin) return SOCK_CLOSED;

    int rc = SOCK_OK;
    int ec = SOCK_OK;

    ssize_t tread = 0;
    uint8_t *wptr = data;

    while (len) {

        // read as much as we can
        ssize_t nread = read(sock->fd, wptr, len);
        if (nread == -1)  {
            // read failed
            ec = SOCK_ERROR;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // timeout or recv buffer empty
                ec = sock->recv_timeout ? SOCK_TIMEOUT : SOCK_AGAIN;
            }
            else if (errno != EINTR) {
               log_errno("sock_read fd=%d", sock->fd);
               sock->sys_err = 1;
            }
            // stop read
            break;
        }

        if (nread == 0) {
            // peer closed
            sock->recv_fin = 1;
            ec = SOCK_CLOSED;
            // stop read
            break;
        }

        // read data
        rc = SOCK_DATA;
        wptr  += nread;
        len -= nread;
        tread += nread;

        // UDP is one shot
        if (sock->mode & SOCK_UDP) break;
    }

    // data or error
    return rc == SOCK_DATA ? tread : ec;
}

// write data to socket fd
ssize_t sock_write_data(struct simple_sock *sock, void *data, size_t len)
{
    // joker checks
    if (sock->sys_err) return SOCK_ERROR;
    if (sock->fin_sent) return SOCK_CLOSED;

    int rc = SOCK_OK;
    int ec = SOCK_OK;

    ssize_t twrite = 0;
    uint8_t *wptr = data;

    while (len) {

        ssize_t nwrite = write(sock->fd, wptr, len);

        if (nwrite == -1) {
            // write failed
            ec = SOCK_ERROR;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // timeout or write buffer full
                ec = sock->send_timeout ? SOCK_TIMEOUT : SOCK_AGAIN;
            }
            else if (errno != EINTR) {
               log_errno("sock_write fd=%d len=%zu", sock->fd,  len);
               sock->sys_err = 1;
            }
            // stop write
            break;
        }

        if (nwrite == 0)  {
            // should not happen
            break;
        }

        // wrote data
        rc = SOCK_DATA;
        wptr += nwrite;
        len -= nwrite;
        twrite += nwrite;
    }

    // data or error
    return rc == SOCK_DATA ? twrite : ec;
}

// write data-buffers to socket fd
ssize_t sock_write_iovs(struct simple_sock *sock, int niov, struct iovec iovs[static niov])
{
    // joker checks
    if (sock->sys_err) return SOCK_ERROR;
    if (sock->fin_sent) return SOCK_CLOSED;

    int rc = SOCK_OK;
    int ec = SOCK_OK;

    // calc total-length
    size_t write_len = iovs_len(niov, iovs);
    struct iovec *iov = iovs;
    ssize_t twrite = 0;

    while (write_len) {

        ssize_t nw = writev(sock->fd, iov, niov);

        if (nw == -1) {
            // write failed
            ec = SOCK_ERROR;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // timeout or write buffer full
                ec = sock->send_timeout ? SOCK_TIMEOUT : SOCK_AGAIN;
            }
            else if (errno != EINTR) {
               log_errno("sock_write fd=%d len=%zu", sock->fd,  iov->iov_len);
               sock->sys_err = 1;
            }
            // stop write
            break;
        }

        if (nw == 0)  {
            // should not happen
            break;
        }

        // wrote data
        rc = SOCK_DATA;
        write_len -= nw;
        twrite += nw;

        // update vectors for next write
        while (niov > 0 && (size_t) nw >= iov->iov_len) {
            nw -= iov->iov_len;
            iov->iov_len = 0;
            iov++;
            niov--;
        }

        // check for partial write
        if (niov > 0 && nw > 0) {
            iov->iov_base = (char *) iov->iov_base + nw;
            iov->iov_len -= nw;
        }
    }

    // data written or error
    return rc == SOCK_DATA ? twrite : ec;
}

/* buffer I/O - send and recv buffers */

// append mem-block to send-buffer
int sock_write_mem(struct simple_sock *sock, void *mem, size_t len)
{
    int rc = rwbuf_write(&sock->send_buf, mem, len);
    if (rc) sock->sys_err = 1;
    return rc;
}

// append str-slice to send-buffer
int sock_write_str(struct simple_sock *sock, struct slice str)
{
    return sock_write_mem(sock, str.ptr, str.len);
}

// append str-slice + CRLF to send-buffer
int sock_write_line(struct simple_sock *sock, struct slice line)
{
    struct iovec iovs[2];

    // load line + CRLF
    iov_load(iovs + 0, line.ptr, line.len);
    iov_load(iovs + 1, STR_LIT("\r\n"));

    int rc = rwbuf_writev(&sock->send_buf, 2, iovs);
    if (rc) {
        sock->sys_err = 1;
        return rc;
    }

    return 0;
}

// write send-buffer to fd
int sock_send(struct simple_sock *sock)
{
    size_t len = rwbuf_used(&sock->send_buf);
    void *buf = rwbuf_rptr(&sock->send_buf);

    // send now
    ssize_t nwrite = sock_write_data(sock, buf, len);
    if (nwrite <= 0) return nwrite;

    // update our send buffer
    sock->send_buf.ridx += nwrite;
    if (sock->send_buf.ridx == sock->send_buf.widx) {
        // all sent - empty buffer
        sock->send_buf.ridx = 0;
        sock->send_buf.widx = 0;
    }

    // data sent
    return SOCK_DATA;
}

// write send-buffer + mem to fd, buffer remaining
int sock_send_mem(struct simple_sock *sock, void *mem, size_t len)
{
    struct iovec iovs[2];

    // load backlog + data
    iov_load(iovs + 0, rwbuf_rptr(&sock->send_buf), rwbuf_used(&sock->send_buf));
    iov_load(iovs + 1, mem, len);

    // write backlog + data
    ssize_t rc = sock_write_iovs(sock, 2, iovs);
    if (rc < 0) return rc;
    if (rc == 0) return 0;

    // update backlog
    rc = rwbuf_rdinc(&sock->send_buf, iovs[0].iov_len);
    if (rc) return rc;

    // add partial data
    if (iovs[1].iov_len) {
        rc = rwbuf_write(&sock->send_buf, iovs[1].iov_base, iovs[1].iov_len);
        if (rc) return rc;
    }

    return 0;
}

// write send-buffer + str-slice to fd, buffer remaining
int sock_send_str(struct simple_sock *sock, struct slice str)
{
    return sock_send_mem(sock, str.ptr, str.len);
}

// write send-buffer + str-slice + CRLF to fd, buffer remaining
int sock_send_line(struct simple_sock *sock, struct slice line)
{
    struct iovec iovs[3];

    // load backlog + data + CRLF
    iov_load(iovs + 0, rwbuf_rptr(&sock->send_buf), rwbuf_used(&sock->send_buf));
    iov_load(iovs + 1, line.ptr, line.len);
    iov_load(iovs + 2, STR_LIT("\r\n"));

    // write it
    ssize_t rc = sock_write_iovs(sock, 3, iovs);
    if (rc < 0) return rc;
    if (rc == 0) return 0;

    // update backlog
    rc = rwbuf_rdinc(&sock->send_buf, iovs[0].iov_len);
    if (rc) return rc;

    // add partial data
    if (iovs[1].iov_len) {
        rc = rwbuf_write(&sock->send_buf, iovs[1].iov_base, iovs[1].iov_len);
        if (rc) return rc;
    }

    // add partial CFLF
    if (iovs[2].iov_len) {
        rc = rwbuf_write(&sock->send_buf, iovs[2].iov_base, iovs[2].iov_len);
        if (rc) return rc;
    }

    return 0;
}

// read into recv-buffer from fd
int sock_recv(struct simple_sock *sock)
{
    void *buf = rwbuf_wptr(&sock->recv_buf);
    size_t space = rwbuf_space(&sock->recv_buf);

    // ensure space to read
    if (sock->min_size && space < sock->min_size) {
        buf = rwbuf_mkspace(&sock->recv_buf, space - sock->min_size);
        if (!buf) {
            // no space
            sock->sys_err = 1;
            return SOCK_ERROR;
        }
        space = rwbuf_space(&sock->recv_buf);
    }

    // read now from fd
    ssize_t nread = sock_read_data(sock, buf, space);
    if (nread <= 0) return nread;

    // update our read buffer
    sock->recv_buf.widx += nread;

    // data recv
    return SOCK_DATA;
}

// extract a line from recv-buffer - return fragment if eof
int sock_recv_line(struct simple_sock *sock, struct slice *line, int eof)
{
    int flags = RWBUF_NOLOG;
    if (eof) flags |= RWBUF_EOF;

    int rc = rwbuf_readline(&sock->recv_buf, line, sock->max_line, flags);
    if (rc < 0) {
        // line too big
        sock->sys_err = 1;
        return log_error_rf("peer %s exceed max line length %zu",
            sock_tostr(sock), sock->max_line);
    }
    return rc;
}

// convert addr to text form - e.g "a.b.c.d:80"
static char *sock_addr_tostr(struct sock_addr *addr)
{
    static char bufs[16][IP_ADDRPORT_STRLEN];
    static int idx;

    char *buf = bufs[idx];
    size_t len = sizeof(bufs[0]);
    idx = (idx + 1) & 15;

    int addr_type = addr->type & (SOCK_IPV4 | SOCK_IPV6);
    switch (addr_type) {
    case SOCK_IPV4: { // a.b.c.d:port
        char *str = buf;
        str += ip4_str_encode(addr->v4, str, len);
        *str++ = ':';
        str = uint16_toa(str, __builtin_bswap16(addr->port));
        str = '\0';
        break;
    }
    case SOCK_IPV6: { // [::]:port
        char *str = buf;
        str += ip6_str_encode(addr->v6, IP6_STR_ADDBRACK | IP6_STR_STRIPV4, str, len);
        *str++ = ':';
        str = uint16_toa(str, __builtin_bswap16(addr->port));
        str = '\0';
        break;
    }
    default:
       buf = "<null>";
    }

    return buf;
}

// convert fd to str
static char *sock_fd_tostr(int sock_fd)
{
    static char bufs[16][40];
    static int idx;

    char *buf = bufs[idx];
    //size_t len = sizeof(bufs[0]);
    idx = (idx + 1) & 15;

    char tmp[20];
    char *fd_str = itoa(sock_fd, tmp, sizeof(tmp));

    char *str = buf;
    str = str_memcpy(str, STR_LIT("fd = "));
    str = str_cat(str, fd_str);

    return buf;
}

// format sock-addr to string
char *sock_tostr(struct simple_sock *sock)
{
    if (sock->mode & SOCK_FILE) {
        return sock_fd_tostr(sock->fd);
    }
    return sock_addr_tostr(&sock->addr);
}

int sock_ipstr_decode(struct slice str, struct sock_addr *addr)
{
    if (slice_ip4_decode(str, addr->v4)) return addr->type = SOCK_IPV4;
    if (slice_ip6_decode(str, addr->v6)) return addr->type = SOCK_IPV6;
    return 0;
}
