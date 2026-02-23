
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h> 
#include <stdarg.h>
#include <stddef.h>
#include <string.h> 
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h> 
#include <unistd.h>
#include <sys/epoll.h>

#include <errno.h>

#include "util.h"
#include "db.h"


#define MAX_LINE 256
#define TCP_PORT 6379
#define MAX_EVENTS 10

#define ERR_READSOCK -1
#define ERR_WRITESOCK -2
#define ERR_CLOSESOCK -3
#define ERR_READLINE -4
#define ERR_BUFSIZE -5
#define ERR_SOCKNAME -6
#define ERR_QUIT -7
#define ERR_POLL -8

#define MAX_HOSTPORT (3 + NI_MAXHOST + NI_MAXSERV)

int get_addr(int sockfd, char *dst, int dst_len)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    int rc;

    rc = getsockname(sockfd, (struct sockaddr *)&addr, &len);
    if (rc != 0) return ERR_SOCKNAME;

    char host[NI_MAXHOST];
    char port[NI_MAXSERV];

    rc = getnameinfo((struct sockaddr *) &addr, len, 
        host, sizeof(host),
        port, sizeof(port),
        NI_NUMERICHOST | NI_NUMERICSERV
    );

    if (rc != 0) return ERR_SOCKNAME;

    if (addr.ss_family == AF_INET6) {
        rc = snprintf(dst, dst_len, "[%s]:%s", host, port);
    }
    else {
        rc = snprintf(dst, dst_len, "%s:%s", host, port);
    }

    return rc > 0 ? 0 : ERR_SOCKNAME;
}


void log_info(const char *fmt, ...)
{
    va_list args;  

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fprintf(stdout, "\n");
}

void log_err(const char *fmt, ...)
{
    va_list args;  

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

int log_err_ret(int ec, const char *fmt, ...)
{
    va_list args;  

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");

    return ec;
}

int log_errno(int ec, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, ": %s\n", strerror(ec));
    va_end(args);

    // TODO just call abort ?

    return 0;
}


// create inet(4|6) tcp listener socket
int open_listener(void)
{
    int fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0); 
    if (fd == -1) {
        perror("create listen socket");
        return -1;
    }

    int opt = 0;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)) == -1) {
        perror("setsocktopt IPPROTO_IPV6");
        return -1;
    }

    opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsocktopt SO_REUSEADDR");
        return -1;
    }

    struct sockaddr_in6 addr = { 0 };
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(TCP_PORT);

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        perror("bind addr");
        return -1;
    }

    if (listen(fd, SOMAXCONN) == -1) {
        perror("listn addr");
        return -1;
    }

    char tmp[MAX_HOSTPORT];
    if (get_addr(fd, tmp, sizeof(tmp)) != 0) {
        perror("get_addr failed");
        return -1;
    }

    log_info("Database listening on %s", tmp);

    return fd;
}

struct rwbuf {
    size_t cap; // fixed size 
    size_t len; // bytes avail to read
    char *rptr;  // start of bytes to read
    char *data;
};


#define ALIGN_UP(n, a) (((n) + (a) - 1) & ~((a) - 1))

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

int read_sock(int fd, struct rwbuf *buf)
{
    int tr = 0;
    char *rptr = buf->rptr + buf->len;
    size_t rem = buf->data + buf->cap - rptr;
    
    while (rem) {
        
        // read as much as we can
        ssize_t nr = read(fd, rptr, rem);

        if (nr == -1)  {
            // read failed
           if (errno == EINTR) continue; // interrupt ?
           if (errno != EAGAIN) {
               log_errno(errno, "read socket");
               tr = ERR_READSOCK;
           }
           break;
        }
        if (nr == 0) {
            // closed ?
            if (tr == 0) tr = ERR_CLOSESOCK;
            break;
        }

        tr += nr;

        rptr += nr;
        rem -= nr;
        buf->len += nr;
    }

    // total read or err
    return tr;
}

static int write_sock(int fd, struct rwbuf *buf)
{
    int tw = 0;

    while (buf->len) {

        size_t nw = write(fd, buf->rptr, buf->len);
        if (nw == -1) {
            // write failed
            if (errno == EINTR) continue;
            if (errno != EAGAIN) {
                log_errno(errno, "write socket");
                tw = ERR_WRITESOCK;
            }
            break;
        }

        tw += nw;
        buf->rptr += nw;
        buf->len -= nw;
    }

    return tw;
}

int read_line(struct rwbuf *buf, struct str_slice *line)
{
    char *eol = memchr(buf->rptr, '\n', buf->len);

    if (eol) {
        // found line
        size_t len = eol - buf->rptr + 1;
        char *str = buf->rptr;
        // remove from buffer
        buf->len -= len ;
        buf->rptr += len;
        // chop cr/lf - TODO remove this ?
        if (len && str[len - 1] == '\n') len--;
        if (len && str[len - 1] == '\r') len--;
        str[len] = '\0';
        // store line
        line->ptr = str;
        line->len = len;
        // have line or error
        return len > MAX_LINE ? ERR_READLINE : (int) len;
    }

    // incomplete line
    if (buf->len >= MAX_LINE) return ERR_READLINE;

    if (buf->rptr > buf->data) {
        // ensure partial line at buffer start
        memmove(buf->data, buf->rptr, buf->len);
        buf->rptr = buf->data;
    }

    // wait for eol
    return 0;
}

// simple wrapper around fd
struct simple_sock {
    int fd; // socket_fd
    // bit fields
    unsigned int is_server; // 1 = simple_server, 0= simple_client
    unsigned int is_epoll; // 1 = registered
    unsigned int is_closed; // 
    unsigned int wait_write;
};

struct simple_buf {
    struct list_elem node;
    struct str_slice buf;
};

struct simple_client {
    struct simple_sock sock;
    struct list_elem node;
    struct simple_server *parent;
    struct rwbuf read_buf;
    struct rwbuf write_buf;
};

struct simple_server {
    struct simple_sock sock;
    struct list_elem clients;
    int epoll_fd; // epoll_create1
};


static int poll_ctrl(struct simple_server *state, struct simple_sock *sock, uint32_t events);
static void destroy_client(struct simple_client *client);


static int do_client_write(struct simple_client *client)
{
    int nw = write_sock(client->sock.fd, &client->write_buf);
    if (nw < 0) return nw;

    if (client->write_buf.len) {
        // write pending - wait for EPOLLOUT event
        if (!poll_ctrl(client->parent, &client->sock, EPOLLOUT | EPOLLIN | EPOLLRDHUP)) {
            return log_err_ret(ERR_POLL, "client enable poll write");
        }
        client->sock.wait_write = 1;
        return 0;
    }

    if (client->sock.wait_write) {
        if (!poll_ctrl(client->parent, &client->sock, EPOLLIN | EPOLLRDHUP)) {
            return log_err_ret(ERR_POLL, "client disable poll write");
        }
        client->sock.wait_write = 0;
    }

    // write buffer empty
    client->write_buf.rptr = client->write_buf.data;
    if (client->sock.is_closed) {
        // safe to close
        destroy_client(client);
    }

    return 0;
}

static int send_str(struct simple_client *client, struct str_slice str)
{
    char *dst = make_space(&client->write_buf, str.len + 2);
    if (!dst) return ERR_BUFSIZE;

    memcpy(dst, str.ptr, str.len);
    dst += str.len;
    *dst++ = '\r';
    *dst++ = '\n';

    return do_client_write(client);
}

static int send_rsp(struct simple_client *client, struct str_slice rsp)
{
    return send_str(client, rsp);
}

static int cmd_set(struct simple_client *client, struct str_slice args)
{
    char *pos = memchr(args.ptr, ' ', args.len);
    int key_len = pos ? pos - args.ptr : args.len;

    struct str_slice key = make_slice(args.ptr, key_len);
    struct str_slice val = ltrim(make_slice(pos, args.len - key_len));
    struct str_slice res;

    if (!key.len || ! val.len)
        res = make_slice(STR_LIT("FAIL"));
    else if (!db_set(key, val)) 
        res = make_slice(STR_LIT("FAIL"));
    else
        res = make_slice(STR_LIT("OK"));

    return send_rsp(client, res);
}

static int cmd_get(struct simple_client *client, struct str_slice key)
{
    struct str_slice res = db_get(key);

    if (!res.ptr)
        res = make_slice(STR_LIT("FAIL"));

    return send_rsp(client, res);
}

static int cmd_del(struct simple_client *client, struct str_slice key)
{
    struct str_slice res;

    if (!key.len)
        res = make_slice(STR_LIT("FAIL"));
    else if (!db_del(key))
        res = make_slice(STR_LIT("FAIL"));
    else 
        res = make_slice(STR_LIT("OK"));

    return send_rsp(client, res);
}

static int cmd_quit(struct simple_client *client, struct str_slice args)
{
    struct str_slice res = make_slice(STR_LIT("OK"));

    send_rsp(client, res);

    return ERR_QUIT;
}

static int cmd_unsupp(struct simple_client *client, struct str_slice args)
{
    struct str_slice res = make_slice(STR_LIT("UNSUPP"));

    return send_rsp(client, res);
}

static struct {
    const char *cmd;
    size_t len;
    int (*func)(struct simple_client *client, struct str_slice args);
} cmds[] = {
    { STR_LIT("SET"), cmd_set },
    { STR_LIT("GET"), cmd_get },
    { STR_LIT("DEL"), cmd_del },
    { STR_LIT("QUIT"), cmd_quit },
};

static int find_cmd(const char *cmd, int len)
{
    for (int i = 0; i < ARR_LEN(cmds); i++) {
        if (cmds[i].len == len && !memcmp(cmd, cmds[i].cmd, len)) {
            return i;
        }
    }

    return -1;
}

int process_cmd(struct simple_client *client, struct str_slice cmd)
{
    char *args = memchr(cmd.ptr, ' ', cmd.len);
    int cmd_len = args ? args - cmd.ptr : cmd.len;
    struct str_slice cmd_args = ltrim(make_slice(args, cmd.len - cmd_len));

    str2upper(cmd.ptr, cmd_len);
    int cmd_idx = find_cmd(cmd.ptr, cmd_len);
    int rc;

    if (cmd_idx != -1) {
        rc = cmds[cmd_idx].func(client, cmd_args);
    }
    else {
        rc = cmd_unsupp(client, cmd_args); 
    }

    return rc;
}



struct simple_client *create_client(int fd, struct simple_server *server)
{
    struct simple_client *client;

    client = malloc(sizeof(*client));
    if (!client) return NULL;

    memset(client, 0,  sizeof(*client));
    client->sock.fd = fd;
    client->parent = server;

    init_rwbuf(&client->read_buf, 128);

    return client;
}

static void destroy_client(struct simple_client *client)
{
    if (client->sock.fd != -1) {
        close(client->sock.fd);
    }

    deinit_rwbuf(&client->read_buf);
    deinit_rwbuf(&client->write_buf);
    
    free(client);
}

static void close_client(struct simple_client *client, int force)
{
    client->sock.is_closed = 1;

    if (force || !client->write_buf.len) {
        destroy_client(client);
    }
}

void do_client_read(struct simple_client *client)
{
    struct str_slice line;
    int rc;

    rc = read_sock(client->sock.fd, &client->read_buf);
    if (rc < 0) {
        // closed|err - XXX we don't support half close
        close_client(client, 1);
        return;
    }

    while ((rc = read_line(&client->read_buf, &line)) > 0) {
        rc = process_cmd(client, line);
        if (rc != 0) break;
    }

    if (rc < 0) {
        // read|procees error
        close_client(client, rc != ERR_QUIT);
    }
}

static void handle_client(struct simple_client *client, uint32_t events)
{
    if (events & EPOLLOUT) {
        // handle writable event
        do_client_write(client);
    }

    if (events & EPOLLIN) {
        // handle read event
        do_client_read(client);
    }

    // TODO EPOLLHUP | EPOLLERR
}

static int poll_ctrl(struct simple_server *state, struct simple_sock *sock, uint32_t events) 
{
    struct epoll_event ev = { 0 };

    ev.events = events;
    ev.data.ptr = sock;
    int op = !sock->is_epoll ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    // do epoll - loop if interuppted
    int rc;
    do {
        rc = epoll_ctl(state->epoll_fd, op, sock->fd, &ev);
    } while (rc == -1 && errno == EINTR);

    if (rc == -1) {
        return 0;
    }

    // registered
    sock->is_epoll = 1;

    return 1;
}

struct simple_client *accept_client(struct simple_server *server)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);

    int fd = accept4(server->sock.fd, (struct sockaddr *) &addr, &len, SOCK_NONBLOCK);
    if (fd == -1) {
        /// EAGAIN|EWOULDBLOCK - means no more pending accepts ..
        return NULL;
    }

    struct simple_client *client = create_client(fd, server);
    if (!client) {
        // out of memory ?
        log_err("client_create failed!");
        close(fd);
        return NULL;
    }

    // turn off nagle
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // XXX - only set EPOLLOUT when a write(fd) reports EAGAIN/EWOULDBLOCK
    if (!poll_ctrl(server, &client->sock, EPOLLIN | EPOLLRDHUP)) { 
        // register failed ?
        log_err("epoll_ctl failed");
        destroy_client(client);
        return NULL;
    }

    // add to servers client list 
    //list_append(&server->clients, &client->node);

    return client;
}

static void handle_server(struct simple_server *state, uint32_t events)
{
    if (events & EPOLLIN) {
        // incomming accept
        int n = 5;
        do {
            struct simple_client *client = accept_client(state);
            if (!client) {
                // no nore clients or oom ?
                break;
            }
        } while(--n);
    }

    // TODO handle  EPOLLHUP | EPOLLERR
}


int do_poll(struct simple_server *server)
{
    struct epoll_event events[MAX_EVENTS];

    int nfd = epoll_wait(server->epoll_fd, events, MAX_EVENTS, -1);

    for (int i = 0; i < nfd; i++) {
        struct simple_sock *sock = events[i].data.ptr;
        if (sock->is_server) {
            handle_server(server, events[i].events);
        }
        else {
            struct simple_client *client = containerof(sock, struct simple_client, sock);
            handle_client(client, events[i].events);
        }
    }

    return 0;
}

int setup_listener(struct simple_server *state)
{
    state->sock.fd = open_listener();
    if (!state->sock.fd) return 0;

    state->epoll_fd = epoll_create1(0);
    if (!state->epoll_fd) {
        return log_err_ret(errno, "epoll_create1");
    }

    if (!poll_ctrl(state, &state->sock, EPOLLIN)) {
        return log_err_ret(errno, "epoll_ctl - add listener");
    }

    // XXX setup epoll AFTER socket/bind/listen
    return 1;
}

int setup_database(struct simple_server *state)
{
    return db_init() == 0;
}

static void init_state(struct simple_server *state)
{
    memset(state, 0, sizeof(*state));
    state->sock.is_server = 1;
    list_init(&state->clients);
}


int main(int argc, char *argv[])
{
    struct simple_server server;

    init_state(&server);
    // TODO parse command line args
    if (!setup_database(&server)) return log_err_ret(-1, "setup_db failed");
    if (!setup_listener(&server)) return log_err_ret(-1, "setup_listener failed");

    while (1) {
        do_poll(&server);
    }    

    return 0;
}

