#ifndef __SOCK_H__
#define __SOCK_H__

// socket errors
#define SOCK_OK      0 // read return 0 - nothing to do
#define SOCK_DATA    1 // read or write worked
#define SOCK_ERROR  -1 // open|read|write|close error
#define SOCK_AGAIN  -2 // read would block (EAGAIN|EWOULDBLOCK)
#define SOCK_CLOSED -3 // read returned 0

#define MAX_EVENTS 10

// big enough for "[" host "]" :" port + null
#define MAX_HOSTPORT (4 + NI_MAXHOST + NI_MAXSERV)

// buffer code
struct rwbuf {
    size_t cap; // fixed size 
    size_t len; // bytes avail to read
    char *rptr;  // start of bytes to read
    char *data;
};
void init_rwbuf(struct rwbuf *buf, size_t cap);
void deinit_rwbuf(struct rwbuf *buf);
int resize_cap(struct rwbuf *buf, size_t cap);
char *make_space(struct rwbuf *buf, size_t len);
int read_line(struct rwbuf *buf, struct str_slice *line);

// wrapper around fd
struct simple_sock {
    int fd; // socket fd
    // flags  - using bit fields
    unsigned int is_server  : 1; // 1 = simple_server, 0= simple_client
    unsigned int is_epoll   : 1; // 1 = registered
    unsigned int force_close : 1; // ignore write buffer
    unsigned int send_close : 1; // we call close
    unsigned int recv_close : 1; // got read 0
    unsigned int wait_write : 1; // waitiinf for writeable event
    unsigned int sys_err    : 1; // read/write error
    struct sockaddr_in6 addr;
};

int sock_listen_hostport(struct simple_sock *sock, const char *host, const char *port);
int sock_accept(struct simple_sock *sock, struct sockaddr_in6 *addr);
int sock_read(struct simple_sock *sock, struct rwbuf *buf);
int sock_write(struct simple_sock *sock, struct rwbuf *buf);
int sock_close(struct simple_sock *sock, int can_log);
char *sock_tostr(struct simple_sock *sock);

#endif
