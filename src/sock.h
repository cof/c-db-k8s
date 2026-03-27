/*
 * A simple socket layer API
 * ========================
 *
 * Create client|server socket
 * -------------------------
 * sock_connect(sock,mode,host,port) : connect to host,port 
 * sock_listen(sock,mode,host,port)  : listen on host,port
 * sock_accept(sock, addr) : accept client sock fd
 *
 * mode is bit-wise or of the following flags:
 *
 * Resolver
 * --------
 * SOCK_ANY -  Use IPv4, IPv6 or V4 mapped
 * SOCK_IPV4 - IPv4 only
 * SOCK_IPV6 - IPv6 only
 * SOCK_PASSIVE - use bindable address (server/listener)
 * SOCK_NUMSERV - disable name resolution on port str
 *
 * type
 * ----
 * SOCK_FILE - socket is a file (e,g stdin,stdout)
 * SOCK_TCP  - TCP socket
 * SOCK_UDP  - UDP socket
 *
 * state
 * ------
 * SOCK_UDPCON - connect UDP socket to address
 * SOCK_NONBLK - make socket non-blocking
 *
 * Close socket fd
 * ------------
 * sock_close   - close socket fd
 * sock_sendfin - shutdown writes send TCP fin
 * 
 * Change fd state
 * -----------------------
 * sock_set_mode   - change socket mode flags
 * sock_set_nonblk - set socket non blocking
 * sock_set_sndto  - set socket send timeout in ms
 * sock_set_rcvto  - set socket recv timeout in ms:
 *
 * send|recv memory buffers to|from sock fd 
 * ------------------
 * sock_send_data - send memory buffer to fd
 * sock_recv_data - recv into memory buffer from fd
 * sock_send_iovs - send iov buffer list to fd
 *
 * send|recv sock buffer data to|from sock fd
 * ------------------------------------------
 * sock_recv - read from fd into sock recv buffer
 * sock_send - write to fd sock send buffer
 *
 * readline - sock buffer
 * ---------------------
 * sock_readline - read line from sock recv buffer into string slice
 *
 * buffer now - send later
 * ------------------------
 * sock_write_data - write data to sock send buffer
 * sock_write_line - write line to sock send buffer
 *
 * send now - buffer unsent
 * -------------------------
 * sock_send_mem - write send buffer + mem to socket fd, buffer unsent
 * sock_send_str - write send buffer + str to socket fd, buffer unsent
 * sock_send_line - write send buffer + line to socket fd, buffer unsent
 * 
 * 
 * Helper
 * -------
 * sockaddr_tostr - convert socket addr to string
 * sock_tostr - convert socket to str
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

// Create client|server socket
int sock_connect(struct simple_sock *sock, uint32_t mode, const char *host, const char *port);
int sock_listen(struct simple_sock *sock, uint32_t mode, const char *host, const char *port);
int sock_accept(struct simple_sock *sock, struct sockaddr_in6 *addr);

// change socket fd state
int sock_set_mode(struct simple_sock *sock, uint32_t mode);
int sock_set_nonblk(struct simple_sock *sock);
int sock_set_sndto(struct simple_sock *sock, uint32_t ms);
int sock_set_rcvto(struct simple_sock *sock, uint32_t ms);

// send|recv memory buffers to|from sock fd
ssize_t sock_send_data(struct simple_sock *sock, void *data, size_t len);
ssize_t sock_recv_data(struct simple_sock *sock, void *data, size_t len);
ssize_t sock_send_iovs(struct simple_sock *sock, int nbuf, struct iovec iovs[nbuf]);

// send|recv memory buffers to|from sock fd
int sock_recv(struct simple_sock *sock);
int sock_send(struct simple_sock *sock);

// read line from sock recv buffer into string slice 
int sock_readline(struct simple_sock *sock, struct str_slice *line, int eof);

// buffer now - send later
int sock_write_data(struct simple_sock *sock, struct str_slice data);
int sock_write_line(struct simple_sock *sock, struct str_slice line);

// send now - buffer unsent
int sock_send_mem(struct simple_sock *sock, void *mem, size_t len);
int sock_send_str(struct simple_sock *sock, struct str_slice str);
int sock_send_line(struct simple_sock *sock, struct str_slice line);

// close socket
int sock_close(struct simple_sock *sock, int can_log);
int sock_sendfin(struct simple_sock *sock);

// helpers
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
