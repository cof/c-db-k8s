#ifndef __SOCK_H__
#define __SOCK_H__

// socket errors
#define ERR_READSOCK -1
#define ERR_WRITESOCK -2
#define ERR_CLOSESOCK -3
#define ERR_READLINE -4
#define ERR_BUFSIZE -5
#define ERR_SOCKNAME -6
#define ERR_QUIT -7
#define ERR_POLL -8

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
    unsigned int send_close : 1; // we call close
    unsigned int recv_close : 1; // got read 0
    unsigned int wait_write : 1; // waitiinf for writeable event
    unsigned int sys_err    : 1; // read/write error
};

int create_listener(const char *host, const char *port, char *addr_str, int addr_str_len);
int sock_accept(struct simple_sock *sock, char *addr_str, int addr_str_len);
int sock_read(struct simple_sock *sock, struct rwbuf *buf);
int sock_write(struct simple_sock *sock, struct rwbuf *buf);



#endif
