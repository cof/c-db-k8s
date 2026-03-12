/*
 S* server - a TCP server than supports telnet api to acesss a key/value store
 *
 * Usage: server [hosthame[:port]]
 *
 *   hostname - an ip address to listen on (default wildcard)
 *   port     - port number to listen on (default=6379)
 *
 * Supported commands:
 *
 *  SET key value - store a key value
 *  GET key       - retrive a key value
 *  DEL key       - delete key/value from store
 *  QUIT          - close connection
 *
 * Notes:
 * - code is using dual-stack sockets
 * - default listen is [::]:6379
 * - commands are case insensitve
 * - see config.h for current defaults
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
#include "sock.h"
#include "db.h"

struct simple_client {
    struct simple_sock sock;
    struct list_elem node;
    struct simple_server *parent;
    struct rwbuf read_buf;
    struct rwbuf write_buf;
    char name[MAX_HOSTPORT];
};

struct simple_server {
    pid_t pid;
    struct simple_sock sock;
    struct list_elem clients;
    // config
    char *host;
    char *port;
    //  state
    int epoll_fd; // epoll_create1
    char name[MAX_HOSTPORT];
};


static int poll_ctrl(struct simple_server *server, struct simple_sock *sock, uint32_t events);
static void client_destroy(struct simple_client *client, int can_log);
static void client_close(struct simple_client *client, int force);

static int send_str(struct simple_client *client, struct str_slice str)
{
    char *dst = make_space(&client->write_buf, str.len + 2);
    if (!dst) {
        return log_error("make space for %zu bytes failed", str.len + 2);
    }

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
    struct str_slice key = slice_copy(args);
    struct str_slice val = slice_split(&key, ' ');  

    slice_trim(&val);

    struct str_slice res;

    if (!key.len || ! val.len)
        res = slice_make(STR_LIT("FAIL"));
    else if (db_set(key, val)) 
        res = slice_make(STR_LIT("FAIL"));
    else
        res = slice_make(STR_LIT("OK"));

    return send_rsp(client, res);
}

static int cmd_get(struct simple_client *client, struct str_slice key)
{
    struct str_slice res = db_get(key);

    if (!res.ptr) {
        res = slice_make(STR_LIT("FAIL"));
    }

    return send_rsp(client, res);
}

static int cmd_del(struct simple_client *client, struct str_slice key)
{
    struct str_slice res;

    if (!key.len)
        res = slice_make(STR_LIT("FAIL"));
    else if (db_del(key))
        res = slice_make(STR_LIT("FAIL"));
    else 
        res = slice_make(STR_LIT("OK"));

    return send_rsp(client, res);
}

static int cmd_quit(struct simple_client *client, struct str_slice args)
{
    struct str_slice res = slice_make(STR_LIT("OK"));

    send_rsp(client, res);

    client_close(client, 0);

    return 0;
}

static int cmd_unsupp(struct simple_client *client, struct str_slice args)
{
    struct str_slice res = slice_make(STR_LIT("UNSUPP"));

    return send_rsp(client, res);
}

static struct {
    const char *name;
    size_t len;
    int (*func)(struct simple_client *client, struct str_slice args);
} cmds[] = {
    { STR_LIT("SET"), cmd_set },
    { STR_LIT("GET"), cmd_get },
    { STR_LIT("DEL"), cmd_del },
    { STR_LIT("QUIT"), cmd_quit },
};

static int find_cmd(struct str_slice cmd)
{
    for (int i = 0; i < ARR_LEN(cmds); i++) {
        if (slice_cmp_cstr(cmd, cmds[i].name, cmds[i].len)) {
            return i;
        }
    }

    return -1;
}

int process_cmd(struct simple_client *client, struct str_slice cmd)
{
    struct str_slice name = slice_copy(cmd);
    struct str_slice args = slice_split(&name, ' ');

    slice_toupper(&name);
    slice_trim(&args);

    int cmd_idx = find_cmd(name);
    int rc;

    if (cmd_idx != -1) {
        rc = cmds[cmd_idx].func(client, args);
    }
    else {
        rc = cmd_unsupp(client, args); 
    }

    return rc;
}

static void client_close(struct simple_client *client, int force)
{
    client->sock.send_close = 1;

    if (force) {
        client->sock.force_close = 1;
    }
}

static void client_destroy(struct simple_client *client, int can_log)
{
    if (can_log) {
        log_info("Client local-close %s", client->name);
    }

    sock_close(&client->sock, can_log);

    deinit_rwbuf(&client->read_buf);
    deinit_rwbuf(&client->write_buf);

    if (list_inuse(&client->node)) {
        list_remove(&client->node);
    }

    free(client);
}

struct simple_client *client_create(int fd)
{
    struct simple_client *client;

    client = malloc(sizeof(*client));
    if (!client) return NULL;

    memset(client, 0,  sizeof(*client));
    client->sock.fd = fd;
    init_rwbuf(&client->read_buf, MAX_LINE * 4);
    list_init(&client->node);

    return client;
}



#define RDWR_EVENTS (EPOLLOUT | EPOLLIN | EPOLLRDHUP)
#define RD_EVENTS (EPOLLIN | EPOLLRDHUP)

static void do_client_write(struct simple_client *client)
{
    int rc = sock_write(&client->sock, &client->write_buf);

    if (rc < 0) {
        // write failed -> bail
        return;
    }

    if (client->write_buf.len) {
        // write pending
        if (!client->sock.wait_write && poll_ctrl(client->parent, &client->sock, RDWR_EVENTS) == 0) {
            client->sock.wait_write = 1;
        }
    }
    else {
        // write complete
        client->write_buf.rptr = client->write_buf.data;
        if (client->sock.wait_write && poll_ctrl(client->parent, &client->sock, RD_EVENTS) == 0) {
            client->sock.wait_write = 0;
        }
    }
}

void do_client_read(struct simple_client *client)
{
    int rc = sock_read(&client->sock, &client->read_buf);

    if (rc < 0) {
        // read failed
        if (rc == SOCK_CLOSED) {
            // client closed its end
            log_info("Client remote-closed %s", client->name);
            client_close(client, 0);
        }
        return;
    }

    // loop until no more lines or error
    struct str_slice line;
    while ((rc = read_line(&client->read_buf, &line)) > 0) {
        rc = process_cmd(client, line);
        if (rc != 0) break;
    }

    if (rc < 0) {
        // error - mark conn for close
       client_close(client, 1);
    }
}

static int client_must_close(struct simple_client *client)
{
    if (client->sock.sys_err) return 1;

    if (client->sock.send_close) {
        if (client->sock.force_close) return 1;
        if (client->write_buf.len == 0) return 1;
    }

    return 0;
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

    if (client_must_close(client)) {
        client_destroy(client, 1);
    }
}

static int poll_ctrl(struct simple_server *server, struct simple_sock *sock, uint32_t events) 
{
    struct epoll_event ev = { 0 };

    ev.events = events;
    ev.data.ptr = sock;
    int op = !sock->is_epoll ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    // do epoll - loop if interuppted
    int rc;
    do {
        rc = epoll_ctl(server->epoll_fd, op, sock->fd, &ev);
    } while (rc == -1 && errno == EINTR);

    if (rc == -1) {
        sock->sys_err = -1;
        return log_errno("epoll_ctl failed (fd=%d, op=%d,events=%u", sock->fd, op, events);
    }

    // registered
    sock->is_epoll = 1;

    // all done
    return 0;
}

struct simple_client *server_accept(struct simple_server *server)
{
    char name[MAX_HOSTPORT];

    int fd = sock_accept(&server->sock, name, sizeof(name));
    if (fd == -1) {
        // no connection available ?
        return NULL;
    }

    struct simple_client *client = client_create(fd);
    if (!client) {
        // out of memory ?
        log_error("client_create failed!");
        close(fd);
        return NULL;
    }

    // set peer name
    memcpy(client->name, name, strlen(name));

    // register with epoll - XXX readable events only
    client->parent = server;
    if (poll_ctrl(server, &client->sock, RD_EVENTS) != 0) { 
        // register failed ?
        client_destroy(client, 1);
        return NULL;
    }

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
        log_errno("get socket error for listener %d", server->sock.fd);
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

// signal handling
volatile sig_atomic_t keep_running = 1;
volatile sig_atomic_t caught_signo = 0; 
volatile sig_atomic_t sender_pid = 0; 
volatile sig_atomic_t sender_uid = 0; 

void handle_signal(int signo, siginfo_t *info, void *ucontext)
{
    caught_signo = signo;

    sender_pid = 0;
    sender_uid = 0;

    if (info->si_code <= 0) {
        sender_pid = info->si_pid;
        sender_uid = info->si_uid;
    }

    keep_running = 0;
}

int setup_signals(struct simple_server *server)
{
    struct sigaction sa = { 0 };

    sa.sa_sigaction = handle_signal;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        return log_errno("setup sigint");
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        return log_errno("setup sigterm");
    }

    // XXX prevent write(fd) trigger a signal
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        return log_errno("setup SIGPIPE");
    }

    return 0;
}

int server_poll(struct simple_server *server)
{
    struct epoll_event events[MAX_EVENTS];

    int nfd = epoll_wait(server->epoll_fd, events, MAX_EVENTS, -1);

    if (nfd < 0) {
        if (errno == EINTR) return 0;
        return log_errno("server PID:%d epoll_wait failed", server->pid);
    }

    for (int i = 0; i < nfd; i++) {
        struct simple_sock *sock = events[i].data.ptr;
        if (sock->is_server) {
            handle_server(server, events[i].events);
        }
        else {
            handle_client((struct simple_client *) sock, events[i].events);
        }
    }

    // all done
    return 0;
}

int server_run(struct simple_server *server)
{
    while (keep_running) {
        if (server_poll(server) != 0) return -1;
    }

    return 0;
}

int setup_listener(struct simple_server *server)
{
    server->sock.fd = create_listener(
        server->host, server->port, 
        server->name, sizeof(server->name)
    );
    if (server->sock.fd == -1) {
        return -1;
    }

    server->epoll_fd = epoll_create1(0);
    if (server->epoll_fd == -1) {
        return log_errno("epoll_create1 failed");
    }

    // register for incoming connections
    if (poll_ctrl(server, &server->sock, EPOLLIN) != 0) {
        return -1;
    }

    log_info("Database listening on %s", server->name);

    // all done
    return 0;
}

int setup_database(struct simple_server *state)
{
    return db_init();
}

int server_parse_argv(struct simple_server *server, int argc, char *argv[])
{
    // listenr address:port 
    if (argc > 1 && argv[1]) {
        // parse
        struct str_slice host = slice_make(argv[1], strlen(argv[1]));
        struct str_slice port = slice_rsplit(&host, ':');
        if (host.len && host.ptr[0] == '[') {
            host.ptr++; host.len--;
            if (host.ptr[host.len] == ']') host.len--;
        }
        // store
        if (host.len && (server->host = strndup(host.ptr, host.len)) == NULL) {
            return log_errno("strdup-hostname");
        }
        if (port.len && (server->port = strndup(port.ptr, port.len)) == NULL) { 
            return log_errno("strdup-portno");
        }
    }

    return 0;
}

void server_destroy(struct simple_server *server)
{
    struct simple_client *client, *next;

    list_fornext_entry_safe(client, next, &server->clients, node) {
        list_remove(&client->node);
        client_destroy(client, 0);
    }

    if (server->sock.fd != -1) {
        close(server->sock.fd);
        server->sock.fd = -1;
    }

    if (server->epoll_fd != -1) {
        close(server->epoll_fd);
        server->epoll_fd = -1;
    }

    free(server);
}

static int server_init(struct simple_server *state)
{
    memset(state, 0, sizeof(*state));
    state->sock.fd = -1;
    state->epoll_fd = -1;

    state->sock.is_server = 1;
    list_init(&state->clients);

    state->pid = getpid();

    state->port = strdup(TCP_PORT_STR);
    if (!state->port) {
        return log_errno("strdup %s", TCP_PORT_STR);
    }

    return 0;
}

struct simple_server *server_create(void)
{
    struct simple_server *server;

    server = malloc(sizeof(*server));
    if (!server) {
        return log_errnon("Malloc failed for server state");
    }

    return server;
}

int main(int argc, char *argv[])
{
    struct simple_server *server = NULL;
    int ec = EXIT_FAILURE;

    if (!(server = server_create())) { ec = 1; goto done; }
    if (server_init(server) != 0)    { ec = 3; goto done; }
    if (setup_signals(server) != 0)  { ec = 2 ;goto done; }
    if (server_parse_argv(server, argc, argv) != 0) { ec = 4;  goto done; }
    if (setup_database(server) != 0) { ec = 5; goto done; }
    if (setup_listener(server) != 0) { ec = 6; goto done; }

    if (server_run(server) != 0) { ec = 7; goto done; }

    if (caught_signo) {
        log_info("Server PID:%d shutting down: got signal %d (%s) from UID:%d PID:%d ", 
            server->pid, 
            caught_signo, strsignal(caught_signo), 
            sender_uid,
            sender_pid);
    }

    // all done
    ec = 0;

done:
    if (server) {
        server_destroy(server);
    }

    return ec;
}
