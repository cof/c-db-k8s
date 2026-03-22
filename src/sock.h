#ifndef __SOCK_H__
#define __SOCK_H__

// socket errors
#define SOCK_OK      0 // read return 0 - nothing to do
#define SOCK_DATA    1 // read or write worked
#define SOCK_ERROR  -1 // open|read|write|close error
#define SOCK_AGAIN  -2 // read would block (EAGAIN|EWOULDBLOCK)
#define SOCK_CLOSED -3 // read returned 0

#define MAX_EVENTS 10
#define MAX_IOV    10

// big enough for "[" host "]" :" port + null
#define MAX_HOSTPORT (4 + NI_MAXHOST + NI_MAXSERV)

// buffer code
struct rwbuf {
    size_t cap; // fixed size 
    size_t len; // bytes avail to read
    char *rptr;  // start of bytes to read
    char *data;
    unsigned int no_resize : 1;
};
void init_rwbuf(struct rwbuf *buf, size_t cap);
void deinit_rwbuf(struct rwbuf *buf);
int resize_cap(struct rwbuf *buf, size_t cap);
void *make_space(struct rwbuf *buf, size_t len);
int read_line(struct rwbuf *buf, struct str_slice *line, int eof);
int rwbuf_write(struct rwbuf *buf, void *data, size_t len);

static inline int rwbuf_update(struct rwbuf *buf, size_t len)
{
    buf->len -= len;
    buf->rptr += len;

    if (!buf->len) {
        buf->rptr = buf->data;
    }
    
    return 0;
}

static inline void rwbuf_load(struct rwbuf *buf, void *data, size_t len)
{
    buf->rptr = buf->data = data;
    buf->cap = buf->len = len;
}
#define RWBUF_LOAD(_buf, _len) \
    { .data = _buf, .rptr = _buf, .cap = _len, .len = 0, .no_resize = 1 }

// wrapper around fd
struct simple_sock {
    int fd; // socket fd
    // flags  - using bit fields
    unsigned int is_server   : 1; // 1 = simple_server, 0= simple_client
    unsigned int is_epoll    : 1; // 1 = registered
    unsigned int recv_fin    : 1; // got read 0
    unsigned int send_fin    : 1; // shutdown writes when write buffer empty
    unsigned int fin_sent    : 1; // shutdown write sent
    unsigned int close_now   : 1; // ignore write buffer - close now
    unsigned int is_readonly : 1; // cannot write to socket
    unsigned int wait_write  : 1; // waiting for writeable event
    unsigned int sys_err     : 1; // read/write error
    struct sockaddr_in6 addr;
};

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

static inline int sock_is_readonly(struct simple_sock *sock)
{
    return sock->is_readonly;
}

static inline int sock_isclosing(struct simple_sock *sock)
{
    return sock->send_fin;
}

static inline int sock_isbusy(struct simple_sock *sock)
{
    // return (sock->wbuf_len > 0) || (sock->send_fin && !sock->fin_sent);
    return sock->send_fin && !sock->fin_sent;
}

static inline int sock_isclosed(struct simple_sock *sock)
{
    return sock->recv_fin && sock->fin_sent;
}

static inline int sock_mustclose(struct simple_sock *sock)
{
    return sock->close_now || sock->sys_err || sock_isclosed(sock);
}

static inline int sock_isactive(struct simple_sock *sock)
{
    return sock->fd != -1 || sock_mustclose(sock);
}

int sock_set_noblock(struct simple_sock *sock);
int sock_connect_hostport(struct simple_sock *sock, const char *host, const char *port);
int sock_listen_hostport(struct simple_sock *sock, const char *host, const char *port);
int sock_accept(struct simple_sock *sock, struct sockaddr_in6 *addr);
int sock_read(struct simple_sock *sock, struct rwbuf *buf);
int sock_write(struct simple_sock *sock, struct rwbuf *buf);
// super-duper buffering
int sock_write_data(struct simple_sock *sock, struct rwbuf *backlog, struct str_slice data);
int sock_write_line(struct simple_sock *sock, struct rwbuf *backlog, struct str_slice line);
int sock_close(struct simple_sock *sock, int can_log);
int sock_sendfin(struct simple_sock *sock);
char *sockaddr_tostr(struct sockaddr *addr, socklen_t addr_len);
char *sock_tostr(struct simple_sock *sock);

#endif
