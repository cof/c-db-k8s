/*
 * A simple socket layer
 */
#ifndef _SOCK_H_
#define _SOCK_H_

#include "rwbuf.h"

// socket errors
#define SOCK_OK      0 // read return 0 - nothing to do
#define SOCK_DATA    1 // read or write worked
#define SOCK_ERROR  -1 // open|read|write|close error
#define SOCK_AGAIN  -2 // read would block (EAGAIN|EWOULDBLOCK)
#define SOCK_CLOSED -3 // read returned 0

#define MAX_EVENTS    10
#define BUF_MINSIZE 4096

// big enough for "[" host "]" :" port + null
#define MAX_HOSTPORT (4 + NI_MAXHOST + NI_MAXSERV)

// wrapper around fd
struct simple_sock {
    int fd; // socket fd
    struct sockaddr_in6 addr;
    struct rwbuf recv_buf;
    struct rwbuf send_buf;
    size_t min_size;
    // flags  - using bit fields
    unsigned int is_server   : 1; // 1 = simple_server, 0= simple_client
    unsigned int is_epoll    : 1; // 1 = registered
    unsigned int recv_fin    : 1; // got read 0
    unsigned int send_fin    : 1; // shutdown writes when write buffer empty
    unsigned int fin_sent    : 1; // shutdown write sent
    unsigned int close_now   : 1; // ignore write buffer - close now
    unsigned int wait_write  : 1; // waiting for writeable event
    unsigned int sys_err     : 1; // read/write error
};

#define SOCK_INIT(_fd, _min_size, _buf1, _len1, _buf2, _len2) { \
    .fd = _fd, \
    .min_size = _min_size, \
    .recv_buf = RWBUF_LOAD(_buf1, _len1), \
    .send_buf = RWBUF_LOAD(_buf2, _len2) \
}

// main api
int sock_init(struct simple_sock *sock,
    int fd, struct sockaddr_in6 *addr,
    size_t buf_size, size_t min_size, size_t max_size);
void sock_deinit(struct simple_sock *sock, int can_log);

int sock_listen_hostport(struct simple_sock *sock, const char *host, const char *port);
int sock_accept(struct simple_sock *sock, struct sockaddr_in6 *addr);
int sock_connect_hostport(struct simple_sock *sock, const char *host, const char *port);
int sock_set_nonblock(struct simple_sock *sock);

int sock_read(struct simple_sock *sock);
int sock_readline(struct simple_sock *sock, struct str_slice *line, int eof);

int sock_write(struct simple_sock *sock);

// buffer now - send later via sock_write
int sock_write_data(struct simple_sock *sock, struct str_slice data);
int sock_write_line(struct simple_sock *sock, struct str_slice line);

// send now - buffer unsent - send via sock_write
int sock_send_data(struct simple_sock *sock, struct str_slice data);
int sock_sendline(struct simple_sock *sock, struct str_slice line);

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
