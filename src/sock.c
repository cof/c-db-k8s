/*
 * socket wrapper code
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
#include "sock.h"

static int sockaddr_tobuf(struct sockaddr *addr, socklen_t addr_len, char *buf, size_t buf_len)
{
    // convert address/port to string
    char host_buf[NI_MAXHOST];
    char port_buf[NI_MAXSERV];

    int rc = getnameinfo(addr, addr_len, 
        host_buf, sizeof(host_buf),
        port_buf, sizeof(port_buf),
        NI_NUMERICHOST | NI_NUMERICSERV
    );

    if (rc != 0) {
        log_error("get name+port string - %s", gai_strerror(rc));
        return -1;
    }

    if (buf_len == 0) return 0;

    // copy host
    char *hostname = host_buf;
    int add_bracket = addr->sa_family == AF_INET6;
    struct str_slice prefix = slice_make_cstr("::ffff:");
    if (!strncmp(hostname, prefix.ptr, prefix.len)) {
        // remove IPv4-mapped IPv6 prefix
        hostname += prefix.len;
        add_bracket = 0;
    }
    size_t wlen = 0;
    size_t len = strlen(hostname);
    size_t need_len = len;
    if (add_bracket) need_len += 2;
    if (wlen + need_len > buf_len) {
        return log_error_rf("No space for hostname");
    }
    if (add_bracket) buf[wlen++] = '[';
    memcpy(buf + wlen, hostname, len);
    wlen += len;
    if (add_bracket) buf[wlen++] = ']';

    // copy port:
    char *port = port_buf;
    len = strlen(port);
    need_len = len + 2;
    if (wlen + need_len > buf_len) {
        return log_error_rf("No space for port");
    }
    buf[wlen++] = ':';
    memcpy(buf + wlen, port, len);
    wlen += len;

    buf[wlen] = '\0';

    return wlen;
}

int sockfd_get_addr(int sockfd, char *buf, int len)
{
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    int rc;

    rc = getsockname(sockfd, (struct sockaddr *)&addr, &addr_len);
    if (rc == -1) {
        log_errno("get ip address");
        return -1;
    }

    return sockaddr_tobuf((struct sockaddr *) &addr, addr_len, buf, len);
}

/*
 * resolv_addr : resovle addr/port string to list of IP address + port
 *
 * Refs:
 * -----
 * AI_PASSIVE     - find address suitable for bind (e.g. server listener)
 * AI_NUMERICSERV - disable name resolution for service port
 * AI_V4MAPPED    - support DUAL stack if possible
 * AI_ALL         - return all matching IPv6 and IPv4 address
 */
static struct addrinfo *resolv_addr(uint32_t mode, const char *host, const char *port)
{
    struct addrinfo hints = { 0 };
    struct addrinfo *res = NULL;

    if (mode & SOCK_PASSIVE) hints.ai_flags |= AI_PASSIVE;
    if (mode & SOCK_NUMSERV) hints.ai_flags |= AI_NUMERICSERV;

    if (mode & SOCK_ANY) {
        hints.ai_flags |= AI_V4MAPPED | AI_ALL;
        hints.ai_family = AF_INET6;
    }
    else if (mode & SOCK_IPV4) {
        hints.ai_family = AF_INET;
    }
    else if (mode & SOCK_IPV4) {
        hints.ai_family = AF_INET6;
    }
    else {
        hints.ai_family = AF_UNSPEC;
    }

    if (mode & SOCK_TCP) hints.ai_socktype = SOCK_STREAM;
    if (mode & SOCK_UDP) hints.ai_socktype = SOCK_DGRAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        return log_error_rn(gai_strerror(rc), "resolve-addr (host=%s port=%s) failed", host, port);
    }

    return res;
}

int sock_init(struct simple_sock *sock,
    int fd, struct sockaddr_in6 *addr,
    size_t max_line, size_t buf_size,
    size_t min_size, size_t max_size)
{
    sock->fd = fd;
    if (addr) memcpy(&sock->addr, addr, sizeof(*addr));
    sock->max_line = max_line;
    sock->min_size = min_size;

    int rc;
    if ((rc = rwbuf_init(&sock->recv_buf, buf_size, max_size))) return rc;
    if ((rc = rwbuf_init(&sock->send_buf, buf_size, max_size))) return rc;

    return 0;
}

// close sock free memory
void sock_deinit(struct simple_sock *sock, int can_log)
{
    sock_close(sock, can_log);

    rwbuf_deinit(&sock->send_buf);
    rwbuf_deinit(&sock->recv_buf);
}

// create inet(4|6) tcp listener socket
static int sock_listen_addr(struct simple_sock *sock, struct addrinfo *res)
{
    char tmp[100];
    char *addr_str = "?";

    int rc = sockaddr_tobuf(res->ai_addr, res->ai_addrlen, tmp, sizeof(tmp));
    if (rc != -1) {
        // got an address
        addr_str = tmp;
    }

    int sock_type = res->ai_socktype; 
    if (sock->mode & SOCK_NONBLK) sock_type |= SOCK_NONBLOCK;

    sock->fd = socket(res->ai_family, sock_type, 0);
    if (sock->fd == -1) {
        log_errno("create socket(%d, %d) for addr %s failed",
            res->ai_family, sock_type, addr_str);
        goto err;
    }

    // copy addr
    memcpy(&sock->addr, res->ai_addr, sizeof(sock->addr));

    // turn off IPV6_ONLY - request dual stack
    int opt = 0;
    if (setsockopt(sock->fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)) == -1)  {
        log_errno("disable IPV6_ONLY");
        goto err;
    }

    // turn on REUSE_ADDR
    opt = 1;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        log_errno("enable reuse_addr");
        goto err;
    }

    // bind to address
    if (bind(sock->fd, res->ai_addr, res->ai_addrlen) == -1) {
        log_errno("bind to (%s) failed", addr_str);
        goto err;
    }

    // finally tell os to start listening
    if (listen(sock->fd, SOMAXCONN) == -1) {
        log_errno("listen on %d,%s failed", sock->fd, addr_str);
        goto err;
    }

    // all done
    return 0;

err:
    if (sock->fd != -1) {
        close(sock->fd);
        sock->fd = -1;
        //sock->sys_err = 1;
    }

    // failed
    return SOCK_ERROR;
}

int sock_listen(struct simple_sock *sock, uint32_t mode, const char *host, const char *port)
{
    struct addrinfo *res = resolv_addr(mode, host, port);
    if (!res) return -1;

    sock->mode = mode;

    int rc = sock_listen_addr(sock, res);
    freeaddrinfo(res);
    
    return rc;
}

int sock_accept(struct simple_sock *sock, struct sockaddr_in6 *addr)
{
    socklen_t addr_len = sizeof(*addr);

    int flags = sock->mode & SOCK_NONBLK ? SOCK_NONBLOCK : 0;
    int fd = accept4(sock->fd, addr, &addr_len, flags);
    if (fd == -1) {
        /// EAGAIN|EWOULDBLOCK - means no more pending accepts ..
        return -1;
    }

    // turn off nagle
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    return fd;
}

int sock_connect(struct simple_sock *sock, uint32_t mode, const char *host, const char *port)
{
    struct addrinfo *addr_list = resolv_addr(mode, host, port);
    if (!addr_list) return -1;

    // loop over addr list for working connection
    int sock_fd = -1;
    for (struct addrinfo *ai = addr_list; ai; ai = ai->ai_next) {
        sock_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock_fd == -1) continue;
        if (mode & SOCK_UDP && (mode & SOCK_UDPCON) == 0) break;
        int rc = connect(sock_fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != -1) {
            // connected
            sock->fd = sock_fd;
            sock->mode = mode;
            memcpy(&sock->addr, ai->ai_addr, sizeof(sock->addr));
            break;
        }
        close(sock_fd);
        sock_fd = -1;
    }
    freeaddrinfo(addr_list);

    if (sock_fd == -1) {
        return log_error_rf("Connect to %s:%s failed", host, port);
    }

    if (mode & SOCK_NONBLK) {
        int rc = sock_set_nonblk(sock);
        if (rc) {
            close(sock_fd);
            sock->fd = -1;
            return rc;
        }
    }

    // all done
    return 0;
}

int sock_set_mode(struct simple_sock *sock, uint32_t mode)
{
    if (mode & SOCK_NONBLK && (sock->mode & SOCK_NONBLK) == 0) {
        int rc = sock_set_nonblk(sock);
        if (rc) return rc;
    }
    sock->mode = mode;
    return 0;
}

int sock_set_nonblk(struct simple_sock *sock)
{
    int flags = fcntl(sock->fd, F_GETFL, 0);
    if (flags == -1) {
        return log_errno_rf("fcntl %d getfl failed", sock->fd);
    }

    flags |= O_NONBLOCK;

    int rc = fcntl(sock->fd, F_SETFL, flags);
    if (rc == -1) {
        return log_errno_rf("fcntl %d setfl %d failed", sock->fd, flags);
    }

    return 0;
}

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

// receive data from socket fd
ssize_t sock_recv_data(struct simple_sock *sock, void *data, size_t space)
{
    // joker checks
    if (sock->sys_err) return SOCK_ERROR;
    if (sock->recv_fin) return SOCK_CLOSED;

    int rc = SOCK_OK;
    int ec = SOCK_OK;

    ssize_t tread = 0;
    uint8_t *wptr = data;

    while (space) {
        
        // read as much as we can
        ssize_t nread = read(sock->fd, wptr, space);

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
        space -= nread;
        tread += nread;

        // UDP is one shot
        if (sock->mode & SOCK_UDP) break;
    }

    // data or error
    return rc == SOCK_DATA ? tread : ec;
}

// send data to socket fd
ssize_t sock_send_data(struct simple_sock *sock, void *data, size_t len)
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

// send iov data to socket fd
ssize_t sock_send_iovs(struct simple_sock *sock, int nbuf, struct iovec iovs[nbuf])
{
    // joker checks
    if (sock->sys_err) return SOCK_ERROR;
    if (sock->fin_sent) return SOCK_CLOSED;

    int rc = SOCK_OK;
    int ec = SOCK_OK;

    // calc total-length
    size_t write_len = iovs_len(nbuf, iovs);
    struct iovec *iov = iovs;
    ssize_t twrite = 0;

    while (write_len) {

        ssize_t nw = writev(sock->fd, iov, nbuf);

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
        while (nbuf > 0 && (size_t) nw >= iov->iov_len) {
            nw -= iov->iov_len;
            iov->iov_len = 0;
            iov++;
            nbuf--;
        }

        // check for partial write
        if (nbuf > 0 && nw > 0) {
            iov->iov_base = (char *) iov->iov_base + nw;
            iov->iov_len -= nw;
        }
    }

    // data written or error
    return rc == SOCK_DATA ? twrite : ec;
}

// read from socket fd into our recv buffer
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

    // recv now
    ssize_t nread = sock_recv_data(sock, buf, space);
    if (nread <= 0) return nread;

    // update our read buffer
    sock->recv_buf.widx += nread;

    // data recv
    return SOCK_DATA;
}

// write to socket fd our send buffer
int sock_send(struct simple_sock *sock)
{
    size_t len = rwbuf_used(&sock->send_buf);
    void *buf = rwbuf_rptr(&sock->send_buf);

    // send now
    ssize_t nwrite = sock_send_data(sock, buf, len);
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

// read a line from our recv buffer
int sock_readline(struct simple_sock *sock, struct str_slice *line, int eof)
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

// buffer now - send later
int sock_write_data(struct simple_sock *sock, struct str_slice data)
{
    int rc = rwbuf_write(&sock->send_buf, data.ptr, data.len);
    if (rc) {
        sock->sys_err = 1;
        return rc;
    }

    return 0;
}

// buffer line - send later
int sock_write_line(struct simple_sock *sock, struct str_slice line)
{
    struct iovec iovs[2];

    // load backlog + data + CRLF
    iov_load(iovs + 0, line.ptr, line.len);
    iov_load(iovs + 1, STR_LIT("\r\n"));

    int rc = rwbuf_writev(&sock->send_buf, 2, iovs);
    if (rc) {
        sock->sys_err = 1;
        return rc;
    }

    return 0;
}

// send now - buffer unsent
int sock_send_str(struct simple_sock *sock, struct str_slice str)
{
    struct iovec iovs[2];

    // load backlog + data 
    iov_load(iovs + 0, rwbuf_rptr(&sock->send_buf), rwbuf_used(&sock->send_buf));
    iov_load(iovs + 1, str.ptr, str.len);

    // write backlog + data
    ssize_t rc = sock_send_iovs(sock, 2, iovs);
    if (rc < 0) return rc;
    if (rc == 0) return 0;

    // update backlog
    rc = rwbuf_ridx_adj(&sock->send_buf, iovs[0].iov_len);
    if (rc) return rc;

    // add partial data
    if (iovs[1].iov_len) {
        rc = rwbuf_write(&sock->send_buf, iovs[1].iov_base, iovs[1].iov_len);
        if (rc) return rc;
    }

    return 0;
}

// send now - buffer unsent
int sock_send_mem(struct simple_sock *sock, void *mem, size_t len)
{
    struct str_slice str = slice_make(mem, len);
    return sock_send_str(sock, str);
}

// send now - buffer unsent
int sock_send_line(struct simple_sock *sock, struct str_slice line)
{
    struct iovec iovs[3];

    // load backlog + data + CRLF
    iov_load(iovs + 0, rwbuf_rptr(&sock->send_buf), rwbuf_used(&sock->send_buf));
    iov_load(iovs + 1, line.ptr, line.len);
    iov_load(iovs + 2, STR_LIT("\r\n"));

    // write it
    ssize_t rc = sock_send_iovs(sock, 3, iovs);
    if (rc < 0) return rc;
    if (rc == 0) return 0;

    // update backlog
    rc = rwbuf_ridx_adj(&sock->send_buf, iovs[0].iov_len);
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
int sock_close(struct simple_sock *sock, int can_log)
{
    if (sock->fd != -1) {
        int ec = close(sock->fd);
        if (ec && can_log) {
            log_error("close fd=%d failed", sock->fd);
        }
        sock->fd = -1;
        if (ec) return -1;
    }

    return 0;
}

char *sockaddr_tostr(struct sockaddr *addr, socklen_t addr_len)
{
    static char bufs[16][40];
    static int idx;

    char *buf = bufs[idx];
    size_t len = sizeof(bufs[0]);
    idx = (idx + 1) & 15;

    int rc = sockaddr_tobuf(addr, addr_len, buf, len);
    if (rc == -1) return "<null>";

    return buf;
}

char *sock_tostr(struct simple_sock *sock)
{
    if (sock->mode & SOCK_FILE) {
        // not a socket
        static char bufs[16][10];
        static int idx;
        char *buf = bufs[idx];
        size_t len = sizeof(bufs[0]);
        idx = (idx + 1) & 15;
        buf[0] = '\0';
        snprintf(buf, len, "fd %d", sock->fd);
        return buf;
    }

    return sockaddr_tostr((struct sockaddr *) &sock->addr, sizeof(sock->addr));
}
