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

#include "config.h"
#include "util.h"
#include "log.h"
#include "sock.h"

#define MAX_EVENTS 10

// big enough for "[" host "]" :" port + null
#define MAX_HOSTPORT (4 + NI_MAXHOST + NI_MAXSERV)

// buffer code
int resize_cap(struct rwbuf *buf, size_t cap)
{
    cap = ALIGN_UP(cap, 128);
    void *data = malloc(cap);
    if (!data) return 0;

    if (buf->data) {
        memcpy(data, buf->rptr, buf->len);
        free(buf->data);
    }

    buf->data = data;
    buf->rptr = data;
    buf->cap = cap;

    return 1;
}

void *make_space(struct rwbuf *buf, size_t len)
{
    char *wptr = buf->rptr + buf->len;
    size_t rem = buf->data + buf->cap - wptr;

    if (rem < len) {
        // not enough space
        if (buf->no_resize) return NULL;
        if (!resize_cap(buf, len + buf->len)) {
            return NULL;
        }
        wptr = buf->rptr + buf->len;
    }

    buf->len += len;

    return wptr;
}

int rwbuf_write(struct rwbuf *buf, void *data, size_t len)
{
    void *space = make_space(buf, len);
    if (!space) return -1;

    memcpy(space, data, len); 

    return 0;
}

void init_rwbuf(struct rwbuf *buf, size_t cap)
{
    memset(buf, 0, sizeof(*buf));

    if (cap) 
        resize_cap(buf, cap);
}

void deinit_rwbuf(struct rwbuf *buf)
{
    buf->len = 0;
    buf->cap = 0;
    buf->rptr = NULL;

    if (buf->data) {
        free(buf->data);
        buf->data = NULL;
    }
}

int read_line(struct rwbuf *buf, struct str_slice *line, int eof)
{
    char *eol = memchr(buf->rptr, '\n', buf->len);

    if (eol) {
        // found terminator
        int rlen = eol - buf->rptr + 1;
        char *str = buf->rptr;
        // remove from buffer
        int len = rlen;
        buf->len -= len;
        buf->rptr += len;

        // chop cr/lf - TODO remove this ?
        if (len && str[len - 1] == '\n') len--;
        if (len && str[len - 1] == '\r') len--;
        str[len] = '\0';

        // store line
        line->ptr = str;
        line->len = len;

        if (len > MAX_LINE) {
            return log_error_rf("line too big - len %d > max %d", len, MAX_LINE);
        }

        // line length + CRLF
        return rlen;
    }

    // incomplete line
    if (buf->len > MAX_LINE) {
        return log_error_rf("line too big - len %zu > max %d", buf->len, MAX_LINE);
    }

    if (eof) {
        // store line
        line->ptr = buf->rptr;
        line->len =  buf->len;
        // clear buffer
        buf->len = 0;
        buf->rptr = buf->data;
        return line->len;
    }

    if (buf->rptr > buf->data) {
        // ensure partial line at buffer start
        memmove(buf->data, buf->rptr, buf->len);
        buf->rptr = buf->data;
    }

    // wait for eol
    return 0;
}

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

static struct addrinfo *resolve_addr(const char *host, const char *port)
{
    struct addrinfo hints, *res;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6; // want dual stack
    hints.ai_socktype = SOCK_STREAM; 
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV | AI_V4MAPPED | AI_ALL;
    res = NULL;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        return log_error_rn(gai_strerror(rc), "resolve address(host=%s port=%s)", host, port);
    }

    return res;
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

    sock->fd = socket(res->ai_family, res->ai_socktype | SOCK_NONBLOCK, 0);
    if (sock->fd == -1) {
        log_errno("create socket(%d, %d) for addr %s failed",
            res->ai_family, res->ai_socktype, addr_str);
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


int sock_listen_hostport(struct simple_sock *sock, const char *host, const char *port)
{
    // resolve addr/port string to IP address + port
    struct addrinfo *res = resolve_addr(host, port);
    if (!res) return -1;

    int rc = sock_listen_addr(sock, res);
    freeaddrinfo(res);
    
    return rc;
}

int sock_accept(struct simple_sock *sock, struct sockaddr_in6 *addr)
{
    socklen_t addr_len = sizeof(*addr);

    int fd = accept4(sock->fd, addr, &addr_len, SOCK_NONBLOCK);
    if (fd == -1) {
        /// EAGAIN|EWOULDBLOCK - means no more pending accepts ..
        return -1;
    }

    // turn off nagle
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    return fd;
}

int sock_set_noblock(struct simple_sock *sock)
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

int sock_connect_hostport(struct simple_sock *sock, const char *host, const char *port)
{
    struct addrinfo *addr_list = resolve_addr(host, port);
    if (!addr_list) return -1;

    // loop over addr list for working connection
    int sock_fd =-1;
    for (struct addrinfo *ai = addr_list; ai; ai = ai->ai_next) {
        sock_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock_fd == -1) continue;
        int rc = connect(sock_fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != -1) {
            // connected
            *sock  = (struct simple_sock) { 0 };
            sock->fd = sock_fd;
            memcpy(&sock->addr, ai->ai_addr, sizeof(sock->addr));
            break;
        }
        close(sock_fd);
        sock_fd = -1;
    }
    freeaddrinfo(addr_list);
    if (sock_fd == -1) return -1;

    int rc = sock_set_noblock(sock);
    if (rc) {
        close(sock_fd);
        sock->fd = -1;
        return rc;
    }

    return 0;
}

int sock_read(struct simple_sock *sock, struct rwbuf *buf)
{
    if (sock->sys_err) return SOCK_ERROR;
    if (sock->recv_fin) return SOCK_CLOSED;
    
    int rc = SOCK_OK;
    int ec = SOCK_OK;
    char *rptr = buf->rptr + buf->len;
    size_t rem = buf->data + buf->cap - rptr;

    while (rem) {
        
        // read as much as we can
        ssize_t nr = read(sock->fd, rptr, rem);

        if (nr == -1)  {
            // read failed
            ec = SOCK_ERROR;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // buffer empty
                ec = SOCK_AGAIN;
            }
            else if (errno != EINTR) {
               log_errno("sock_read fd=%d", sock->fd);
               sock->sys_err = 1;
            }
            // stop read
            break;
        }

        if (nr == 0) {
            // peer closed
            sock->recv_fin = 1;
            ec = SOCK_CLOSED;
            // stop read
            break;
        }

        // read data
        rc = SOCK_DATA;
        rptr += nr;
        rem -= nr;
        buf->len += nr;
    }

    // data or error
    return rc == SOCK_DATA ? rc : ec;
}

int sock_write(struct simple_sock *sock, struct rwbuf *buf)
{
    if (sock->sys_err) {
        return SOCK_ERROR;
    }

    int rc = SOCK_OK;
    int ec = SOCK_OK;

    while (buf->len) {

        ssize_t nw = write(sock->fd, buf->rptr, buf->len);

        if (nw == -1) {
            // write failed
            ec = SOCK_ERROR;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // buffer full
                ec = SOCK_AGAIN;
            }
            else if (errno != EINTR) {
               log_errno("sock_write fd=%d len=%zu", sock->fd,  buf->len);
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
        buf->rptr += nw;
        buf->len -= nw;
    }

    // data or error
    return rc == SOCK_DATA ? rc : ec;
}

// super-duper buffering
static inline void load_iov(struct iovec *iov, void *data, size_t len)
{
    iov->iov_base = data;
    iov->iov_len = len;
}

static int sock_writev(struct simple_sock *sock, int nbuf,  struct iovec iovs[nbuf])
{
    if (sock->sys_err) {
        return SOCK_ERROR;
    }

    int rc = SOCK_OK;
    int ec = SOCK_OK;

    // calc total-length
    size_t write_len = 0;
    for (int i = 0; i < nbuf; i++)  {
        write_len += iovs[i].iov_len;
    }
    struct iovec *iov = iovs;

    while (write_len) {

        ssize_t nw = writev(sock->fd, iov, nbuf);

        if (nw == -1) {
            // write failed
            ec = SOCK_ERROR;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // buffer full
                ec = SOCK_AGAIN;
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

    // data written  or error
    return rc == SOCK_DATA ? rc : ec;
}

int sock_write_data(struct simple_sock *sock, struct rwbuf *backlog, struct str_slice data)
{
    struct iovec iovs[2];

    // load backlog + data 
    load_iov(iovs + 0, backlog->rptr, backlog->len);
    load_iov(iovs + 1, data.ptr, data.len);

    // write backlog + data
    int rc = sock_writev(sock, 2, iovs);
    if (rc < 0) return rc;

    // update backlog
    rc = rwbuf_update(backlog, backlog->len - iovs[0].iov_len);
    if (rc) return rc;

    // add partial data
    if (iovs[1].iov_len) {
        rc = rwbuf_write(backlog, iovs[1].iov_base, iovs[1].iov_len);
        if (rc) return rc;
    }

    return 0;
}

int sock_write_line(struct simple_sock *sock, struct rwbuf *backlog, struct str_slice line)
{
    struct iovec iovs[3];

    // load backlog + data + CRLF
    load_iov(iovs + 0, backlog->rptr, backlog->len);
    load_iov(iovs + 1, line.ptr, line.len);
    load_iov(iovs + 2, STR_LIT("\r\n"));

    // write it
    int rc = sock_writev(sock, 3, iovs);
    if (rc < 0) return rc;

    // update backlog
    rc = rwbuf_update(backlog, backlog->len - iovs[0].iov_len);
    if (rc) return rc;

    // add partial data
    if (iovs[1].iov_len) {
        rc = rwbuf_write(backlog, iovs[1].iov_base, iovs[1].iov_len);
        if (rc) return rc;
    }

    // add partial CFLF
    if (iovs[2].iov_len) {
        rc = rwbuf_write(backlog, iovs[2].iov_base, iovs[2].iov_len);
        if (rc) return rc;
    }

    return 0;
}

// shutdown writes on socket
int sock_sendfin(struct simple_sock *sock) 
{
    if (sock->fin_sent) return 0;

    int rc = shutdown(sock->fd, SHUT_WR);
    if (rc != 0 && errno != ENOTSOCK) {
        sock->sys_err = 1;
        return log_errno_rf("shutdown write fd=%d failed", sock->fd);
    }

    sock->fin_sent = 1;

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
    return sockaddr_tostr((struct sockaddr *) &sock->addr, sizeof(sock->addr));
}
