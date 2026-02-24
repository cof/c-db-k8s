/*
 *
 */
#define _GNU_SOURCE 1 // for accept4
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

#include "util.h"
#include "db.h"


#define MAX_LINE 256
#define TCP_PORT 6379
#define MAX_EVENTS 10

// big enough for "[" host "]" :" port + null
#define MAX_HOSTPORT (4 + NI_MAXHOST + NI_MAXSERV)


#define ERR_READSOCK -1
#define ERR_WRITESOCK -2
#define ERR_CLOSESOCK -3
#define ERR_READLINE -4
#define ERR_BUFSIZE -5
#define ERR_SOCKNAME -6
#define ERR_QUIT -7
#define ERR_POLL -8


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

void log_estr(const char *file, int line, const char *estr, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "[%s:%d] ", file, line);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, ": %s\n", estr);
    va_end(args);
}


#define LOG_ESTR(estr, ...)  log_estr(__FILE__, __LINE__, estr, __VA_ARGS__);
#define LOG_ERRNO(...)  log_estr(__FILE__, __LINE__, strerror(errno), __VA_ARGS__);

int addr_tostr(struct sockaddr *addr, socklen_t addr_len, char *buf, int len)
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
        LOG_ESTR(gai_strerror(rc), "get name+port string");
        return -1;
    }

    // finaly load addr:port string
    char *dst = buf;
    if (addr->sa_family == AF_INET6) *dst++ = '[';
    dst = mempcpy(dst, host, strlen(host));
    if (addr->sa_family == AF_INET6) *dst++ = ']';
    *dst++ = ':';
    dst = mempcpy(dst, port, strlen(port));
    *dst = '\0';

    return dst - buf;
}

int get_addr(int sock_fd, char *buf, int len)
{
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    int rc;

    rc = getsockname(sock_fd, (struct sockaddr *)&addr, &addr_len);
    if (rc == -1) {
        LOG_ERRNO("get ip address");
        return -1;
    }

    return addr_tostr((struct sockaddr *) &addr, addr_len, buf, len);
}

static struct addrinfo *resolve_addr(const char *host, const char *port)
{
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET6; // want dual stack
	hints.ai_socktype = SOCK_STREAM; 
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV | AI_V4MAPPED | AI_ALL;
    if (!port) port = "6379";

	int rc = getaddrinfo(host, port, &hints, &res);
	if (rc != 0) {
        LOG_ESTR(gai_strerror(rc), "resolve address(host=%s port=%s)", host, port);
        return NULL;
	}

    return res;
}

// create inet(4|6) tcp listener socket
int open_listener(struct addrinfo *res, const char *addr_str)
{
    int fd = socket(res->ai_family, res->ai_socktype | SOCK_NONBLOCK, 0);
    if (fd == -1) {
        LOG_ERRNO("create listener socket");
        goto err;
    }

    // turn off IPV6_ONLY - request dual stack
    int opt = 0;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)) == -1)  {
        LOG_ERRNO("disable IPV6_ONLY");
        goto err;
    }

    // turn on REUSE_ADDR
    opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        LOG_ERRNO("enable reuse_addr");
        goto err;
    }

    // bind to address
    if (bind(fd, res->ai_addr, res->ai_addrlen) == -1) {
        LOG_ERRNO("bind to %s failed", addr_str);
        goto err;
    }

    // finally tell os to start listening
    if (listen(fd, SOMAXCONN) == -1) {
        LOG_ERRNO("listen for connections");
        goto err;
    }

    // all done
    return fd;

err:
    if (fd != -1) close(fd);
    return -1;
}

int create_listener(const char *host, const char *port, char *name, int name_len)
{
    // resolve addr/port string to IP address + port
    struct addrinfo *res = resolve_addr(host, port);
    if (!res) return -1;

    // convert the bindable address to string - (logging)
    if (addr_tostr(res->ai_addr, res->ai_addrlen, name, name_len) == -1) {
        freeaddrinfo(res);
        return -1;
    }

    int fd = open_listener(res, name);
    freeaddrinfo(res);

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

// wrapper around fd
struct simple_sock {
    int fd; // socket_fd
    // bit fields
    unsigned int is_server; // 1 = simple_server, 0= simple_client
    unsigned int is_epoll; // 1 = registered
    unsigned int send_close; // we call close
    unsigned int recv_close; // got read 0
    unsigned int wait_write;
    unsigned int sys_err;
};

int read_sock(struct simple_sock *sock, struct rwbuf *buf)
{
    int tr = 0;
    char *rptr = buf->rptr + buf->len;
    size_t rem = buf->data + buf->cap - rptr;

    if (sock->sys_err) return ERR_READSOCK;
    
    while (rem) {
        
        // read as much as we can
        ssize_t nr = read(sock->fd, rptr, rem);

        if (nr == -1)  {
            // read failed
           if (errno == EINTR) continue; // interrupt ?
           if (errno != EAGAIN) {
               LOG_ERRNO("read simple_socket");
               tr = ERR_READSOCK;
               sock->sys_err = 1;
           }
           break;
        }
        if (nr == 0) {
            // closed
            sock->recv_close = 1;
            if (tr == 0) tr = ERR_CLOSESOCK;
            break;
        }

        tr += nr;

        rptr += nr;
        rem -= nr;
        buf->len += nr;
    }

    // nread | error
    return tr;
}

static int write_sock(struct simple_sock *sock, struct rwbuf *buf)
{
    int tw = 0;

    if (sock->sys_err) return ERR_WRITESOCK;

    while (buf->len) {

        size_t nw = write(sock->fd, buf->rptr, buf->len);
        if (nw == -1) {
            // write failed
            if (errno == EINTR) continue;
            if (errno != EAGAIN) {
                LOG_ERRNO("write simple_socket");
                tw = ERR_WRITESOCK;
                sock->sys_err = 1;
            }
            break;
        }

        // record what was written
        tw += nw;
        buf->rptr += nw;
        buf->len -= nw;
        break;
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
        if (len > MAX_LINE) {
            LOG_ESTR("line too big", "len %d > max %d", len, MAX_LINE);
            len = ERR_READLINE;
        }
        return len;
    }

    // incomplete line
    if (buf->len > MAX_LINE) {
        LOG_ESTR("line too big", "len %d > max %d", buf->len, MAX_LINE);
        return ERR_READLINE;
    }

    if (buf->rptr > buf->data) {
        // ensure partial line at buffer start
        memmove(buf->data, buf->rptr, buf->len);
        buf->rptr = buf->data;
    }

    // wait for eol
    return 0;
}


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
    char name[MAX_HOSTPORT];
};

struct simple_server {
    struct simple_sock sock;
    struct list_elem clients;
    // config
    char *host;
	char *port;
	//  state
    int epoll_fd; // epoll_create1
    char name[MAX_HOSTPORT];
};


static int poll_ctrl(struct simple_server *state, struct simple_sock *sock, uint32_t events);
static void client_destroy(struct simple_client *client);


static int send_str(struct simple_client *client, struct str_slice str)
{
    char *dst = make_space(&client->write_buf, str.len + 2);
    if (!dst) return ERR_BUFSIZE;

    memcpy(dst, str.ptr, str.len);
    dst += str.len;
    *dst++ = '\r';
    *dst++ = '\n';

    // XXX do_client_write needs to be called ...

    return 0;
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

struct simple_client *client_create(int fd, struct simple_server *server)
{
    struct simple_client *client;

    client = malloc(sizeof(*client));
    if (!client) return NULL;
    memset(client, 0,  sizeof(*client));
    client->sock.fd = fd;
    client->parent = server;

    init_rwbuf(&client->read_buf, 128);
    list_init(&client->node);

    return client;
}

static void client_destroy(struct simple_client *client)
{
    if (client->sock.fd != -1) {
        close(client->sock.fd);
    }

    deinit_rwbuf(&client->read_buf);
    deinit_rwbuf(&client->write_buf);

    list_remove(&client->node);

    free(client);
}

static void client_close(struct simple_client *client, int force)
{
    client->sock.send_close = 1;

    if (force || client->write_buf.len) {
        // discard the write buffer
        client->write_buf.len = 0;
    }
}

#define RDWR_EVENTS (EPOLLOUT | EPOLLIN | EPOLLRDHUP)
#define RD_EVENTS (EPOLLIN | EPOLLRDHUP)

static void do_client_write(struct simple_client *client)
{
    int nw = write_sock(&client->sock, &client->write_buf);
    if (nw < 0) {
        // write failed -> bail
        return;
    }

    if (client->write_buf.len) {
        // write pending
        if (!client->sock.wait_write && poll_ctrl(client->parent, &client->sock, RDWR_EVENTS)) {
            client->sock.wait_write = 1;
        }
    }
    else {
        // write complete
        client->write_buf.rptr = client->write_buf.data;
        if (client->sock.wait_write && poll_ctrl(client->parent, &client->sock, RD_EVENTS)) {
            client->sock.wait_write = 0;
        }
    }
}

void do_client_read(struct simple_client *client)
{
    struct str_slice line;
    int rc;

    rc = read_sock(&client->sock, &client->read_buf);
    if (rc < 0) {
        // read failed -> bail
        return;
    }

    // loop until no more lines or error
    while ((rc = read_line(&client->read_buf, &line)) > 0) {
        rc = process_cmd(client, line);
        if (rc != 0) break;
    }

    if (rc < 0) {
        // error ? mark conn for close
       client_close(client, rc != ERR_QUIT);
    }
}

static void do_client_check(struct simple_client *client)
{
    if (client->sock.sys_err || (client->sock.send_close && !client->write_buf.len)) {
        // safe to close
        client_destroy(client);
    }
}

static void handle_client(struct simple_client *client, uint32_t events)
{
    if (events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
        // send buffer is writable or error
        do_client_write(client);
    }

    if (events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
        // read buffer has data/fin or error
        do_client_read(client);
        // start sending any reponse
        do_client_write(client);
    }

    // handle close or error
    do_client_check(client);
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
        LOG_ERRNO("epoll_ctl failed (fd=%d, op=%d,events=%u", sock->fd, op, events);
        sock->sys_err = -1;
        return 0;
    }

    // registered
    sock->is_epoll = 1;

    return 1;
}

struct simple_client *server_accept(struct simple_server *server)
{
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);

    int fd = accept4(server->sock.fd, (struct sockaddr *) &addr, &addr_len, SOCK_NONBLOCK);
    if (fd == -1) {
        /// EAGAIN|EWOULDBLOCK - means no more pending accepts ..
        return NULL;
    }

    struct simple_client *client = client_create(fd, server);
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
    if (!poll_ctrl(server, &client->sock, RD_EVENTS)) { 
        // register failed ?
        client_destroy(client);
        return NULL;
    }

    addr_tostr((struct sockaddr *)&addr, addr_len, client->name, sizeof(client->name));

    // add to servers client list 
    list_append(&server->clients, &client->node);

    log_info("Client connected from %s", client->name);

    return client;
}

static void do_server_accept(struct simple_server *server) 
{
    // incoming client connections
    int max_accept = 5;

    do {
        struct simple_client *client = server_accept(server);
        if (!client) {
            break;
        }
    } while(--max_accept);
}

static void do_server_err(struct simple_server *server) 
{
    int error = 0;
    socklen_t errlen = sizeof(error);

    if (!getsockopt(server->sock.fd, SOL_SOCKET, SO_ERROR, &error, &errlen)) {
        LOG_ERRNO("get socket error for listener %d", server->sock.fd);
    }

    server->sock.sys_err = 1;
}

static void do_server_check(struct simple_server *server)
{
    if (!server->sock.sys_err) return;
    if (server->sock.fd == -1) return;

    close(server->sock.fd);
    server->sock.fd = -1;

    log_info("Database stopped listening on %s", server->name);
}

static void handle_server(struct simple_server *server, uint32_t events)
{
    if (events & (EPOLLERR | EPOLLHUP)) {
        // interface gone down ?
        do_server_err(server);
    }

    if (events & EPOLLIN) {
        // incoming connection
        do_server_accept(server);
    }

    do_server_check(server);
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
            handle_client((struct simple_client *) sock, events[i].events);
        }
    }

    return 0;
}



int setup_listener(struct simple_server *server)
{
    server->sock.fd = create_listener(
        server->host, server->port, 
        server->name, sizeof(server->name)
    );
    if (server->sock.fd == -1) return 0;

    server->epoll_fd = epoll_create1(0);
    if (server->epoll_fd == -1) {
        LOG_ERRNO("epoll_create1 failed");
        return 0;
    }

    // register for incoming connections
    if (!poll_ctrl(server, &server->sock, EPOLLIN)) {
        return 0;
    }

    log_info("Database listening on %s", server->name);

    // all done
    return 1;


}

int setup_database(struct simple_server *state)
{
    return db_init() == 0;
}

int parse_cmdline(struct simple_server *server, int argc, char *argv[])
{
    // listenr address:port 
    if (argc > 1 && argv[1]) {
		struct str_slice host = make_slice(argv[1], strlen(argv[1]));
		char *port_str = memrchr(host.ptr, ':', host.len);
		int host_len = port_str ? port_str - host.ptr : host.len;
		int port_len = host.len - host_len;
 		struct str_slice port = make_slice(port_str, port_len);
		host.len = host_len;
		// store
		server->host = host.ptr ? strndup(host.ptr, host.len) : NULL;
		server->port = port.ptr ? strndup(port.ptr, port.len) : NULL;
	}

    return 1;
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

    // XXX prevent write(fd) trigger a signal
    signal(SIGPIPE, SIG_IGN);

    if (!parse_cmdline(&server, argc, argv)) return 1;
    if (!setup_database(&server)) return 2;
    if (!setup_listener(&server)) return 3;

    while (1) {
        do_poll(&server);
    }    

    return 0;
}

