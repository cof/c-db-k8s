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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h> 
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>

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

char *make_space(struct rwbuf *buf, size_t len)
{
    char *wptr = buf->rptr + buf->len;
    size_t rem = buf->data + buf->cap - wptr;

    if (rem < len) {
        // not enough space
        if (!resize_cap(buf, len + buf->len)) {
            return NULL;
        }
        wptr = buf->rptr + buf->len;
    }

    buf->len += len;

    return wptr;
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

int read_line(struct rwbuf *buf, struct str_slice *line)
{
    char *eol = memchr(buf->rptr, '\n', buf->len);

    if (eol) {
        // found terminator
        int rlen = eol - buf->rptr + 1;
        char *str = buf->rptr;
        // remove from buffer
        int len = rlen;
        buf->len -= len ;
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
    char host[NI_MAXHOST];
    char port[NI_MAXSERV];

    int rc = getnameinfo(addr, addr_len, 
        host, sizeof(host),
        port, sizeof(port),
        NI_NUMERICHOST | NI_NUMERICSERV
    );

    if (rc != 0) {
        log_error("get name+port string - %s", gai_strerror(rc));
        return -1;
    }

    if (buf_len == 0) return 0;

    // copy host
    size_t wlen = 0;
    size_t len = strlen(host);
    size_t need_len = len;
    if (addr->sa_family == AF_INET6) need_len += 2;
    if (wlen + need_len > buf_len) {
        return log_error_rf("No space for hostname");
    }
    if (addr->sa_family == AF_INET6) buf[wlen++] = '[';
    memcpy(buf + wlen, host, len);
    wlen += len;
    if (addr->sa_family == AF_INET6) buf[wlen++] = ']';

    // copy port:
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
    struct addrinfo hints, *res = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6; // want dual stack
    hints.ai_socktype = SOCK_STREAM; 
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV | AI_V4MAPPED | AI_ALL;

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

int sock_read(struct simple_sock *sock, struct rwbuf *buf)
{
    if (sock->sys_err) {
        return SOCK_ERROR;
    }
    
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
               log_errno("sock_read fd=%d len=%zu", sock->fd, rem);
               sock->sys_err = 1;
            }
            // stop read
            break;
        }

        if (nr == 0) {
            // peer closed
            sock->recv_close = 1;
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

char *sock_tostr(struct simple_sock *sock)
{
    static char bufs[16][40];
    static int idx;

    char *buf = bufs[idx];
    size_t len = sizeof(bufs[0]);
    idx = (idx + 1) & 15;

    socklen_t addr_len = sizeof(struct sockaddr_in6);
    int rc = sockaddr_tobuf((struct sockaddr *) &sock->addr, addr_len, buf, len);
    if (rc == -1) {
        // need a value
        return "<null>";
    }

    return buf;
}
