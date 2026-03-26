/*
 * A simple socket layer
 */
#ifndef _SOCK_H_
#define _SOCK_H_

#include "rwbuf.h"

// socket errors
#define SOCK_OK       0 // read return 0 - nothing to do
#define SOCK_DATA     1 // read or write worked
#define SOCK_ERROR   -1 // open|read|write|close error
#define SOCK_AGAIN   -2 // read would block (EAGAIN|EWOULDBLOCK)
#define SOCK_CLOSED  -3 // read returned 0
#define SOCK_TIMEOUT -4 // read|write timeout

#define MAX_EVENTS    10
#define BUF_MINSIZE 4096

// big enough for "[" host "]" :" port + null
#define MAX_HOSTPORT (4 + NI_MAXHOST + NI_MAXSERV)

// mode flags
#define SOCK_ANY      0x0001
#define SOCK_IPV4     0x0002
#define SOCK_IPV6     0x0004
#define SOCK_FILE     0x0008
#define SOCK_TCP      0x0010
#define SOCK_UDP      0x0020
#define SOCK_UDPCON   0x0040
#define SOCK_NONBLK   0x0080
#define SOCK_PASSIVE  0x0100
#define SOCK_NUMSERV  0x0200

#define SOCK_LISTEN (SOCK_ANY | SOCK_NUMSERV | SOCK_PASSIVE | SOCK_NONBLK)

// wrapper around fd
struct simple_sock {
    int fd; // socket fd
    struct sockaddr_in6 addr;
    struct rwbuf recv_buf;
    struct rwbuf send_buf;
    size_t max_line;
    size_t min_size;
    uint32_t mode;  // connect|listen flags
    // flags  - using bit fields
    unsigned int is_server    : 1; // 1 = simple_server, 0= simple_client
    unsigned int is_epoll     : 1; // 1 = registered
    unsigned int send_timeout : 1; // SO_SNDTIMEO set
    unsigned int recv_timeout : 1; // SO_RCVTIMEO set
    unsigned int recv_fin     : 1; // got read 0
    unsigned int send_fin     : 1; // shutdown writes when write buffer empty
    unsigned int fin_sent     : 1; // shutdown write sent
    unsigned int close_now    : 1; // ignore write buffer - close now
    unsigned int wait_write   : 1; // waiting for writeable event
    unsigned int sys_err      : 1; // read/write error
};

#define SOCK_INIT(_fd, _max_line,  _min_size, _buf1, _len1, _buf2, _len2) { \
    .fd = _fd, \
    .max_line = _max_line, \
    .min_size = _min_size, \
    .recv_buf = RWBUF_LOAD(_buf1, _len1), \
    .send_buf = RWBUF_LOAD(_buf2, _len2) \
}

// main api
int sock_init(struct simple_sock *sock,
    int fd, struct sockaddr_in6 *addr,
    size_t max_line, size_t buf_size,
    size_t min_size, size_t max_size);
void sock_deinit(struct simple_sock *sock, int can_log);

// create socket fd
int sock_connect(struct simple_sock *sock, uint32_t mode, const char *host, const char *port);
int sock_listen(struct simple_sock *sock, uint32_t mode, const char *host, const char *port);
int sock_accept(struct simple_sock *sock, struct sockaddr_in6 *addr);

// change socket fd state
int sock_set_mode(struct simple_sock *sock, uint32_t mode);
int sock_set_nonblk(struct simple_sock *sock);
int sock_set_sndto(struct simple_sock *sock, uint32_t ms);
int sock_set_rcvto(struct simple_sock *sock, uint32_t ms);

// send|recv data to|from socket fd
ssize_t sock_send_data(struct simple_sock *sock, void *data, size_t len);
ssize_t sock_recv_data(struct simple_sock *sock, void *data, size_t len);
ssize_t sock_send_iovs(struct simple_sock *sock, int nbuf, struct iovec iovs[nbuf]);

// send|recv buffers to|from socket fd
int sock_recv(struct simple_sock *sock);
int sock_send(struct simple_sock *sock);

// read line from our recv buffer
int sock_readline(struct simple_sock *sock, struct str_slice *line, int eof);

// buffer now - send later
int sock_write_data(struct simple_sock *sock, struct str_slice data);
int sock_write_line(struct simple_sock *sock, struct str_slice line);

// send now - buffer unsent
int sock_send_mem(struct simple_sock *sock, void *mem, size_t len);
int sock_send_str(struct simple_sock *sock, struct str_slice str);
int sock_send_line(struct simple_sock *sock, struct str_slice line);

int sock_close(struct simple_sock *sock, int can_log);
int sock_sendfin(struct simple_sock *sock);

char *sockaddr_tostr(struct sockaddr *addr, socklen_t addr_len);
char *sock_tostr(struct simple_sock *sock);

// inline helpers
static inline size_t sock_sendbuf_used(struct simple_sock *sock)
{
    return rwbuf_used(&sock->send_buf);
}

static inline size_t sock_recvbuf_used(struct simple_sock *sock)
{
    return rwbuf_used(&sock->recv_buf);
}

static inline void sock_wrclose(struct simple_sock *sock, int force)
{
    // socket is closed for writes
    sock->send_fin = 1; 

    if (force) {
        sock->close_now = 1;
    }
}

static inline void sock_seterr(struct simple_sock *sock)
{
    sock->sys_err = 1;
}

static inline int sock_recveof(struct simple_sock *sock)
{
    return sock->recv_fin;
}

// read side done
static inline int sock_rd_done(struct simple_sock *sock)
{
    return sock->recv_fin && sock_recvbuf_used(sock) == 0;
}

// write side done
static inline int sock_wr_done(struct simple_sock *sock)
{
    return sock->send_fin && sock_sendbuf_used(sock) == 0;
}

static inline int sock_isclosing(struct simple_sock *sock)
{
    return sock->send_fin;
}

static inline int sock_isbusy(struct simple_sock *sock)
{
    return rwbuf_used(&sock->send_buf) > 0 || (sock->send_fin && !sock->fin_sent);
}

static inline int sock_isclosed(struct simple_sock *sock)
{
    return sock->recv_fin && sock->fin_sent;
}

static inline int sock_mustclose(struct simple_sock *sock)
{
    return sock->sys_err || sock->close_now || sock_isclosed(sock);
}

static inline int sock_canclose(struct simple_sock *sock)
{
    return sock_mustclose(sock) || (sock->send_fin && rwbuf_used(&sock->send_buf) == 0);
}

static inline int sock_isactive(struct simple_sock *sock)
{
    return sock->fd >= 0 && !sock_mustclose(sock);
}

#endif
