/*
 * server   : TCP server than supports telnet api to acesss a key/value store
 * Usage    : ./server --help
 * Example  : ./server --hostnanme 127.0.0.1 --port 5379
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
#include "list.h"
#include "log.h"
#include "sock.h"
#include "db.h"

struct simple_client {
    struct simple_sock sock;
    struct list_elem node;
    struct simple_server *parent;
    unsigned int log_req : 1; // log request
    unsigned int log_rsp : 1; // log response
};

struct simple_server {
    struct simple_sock sock;
    struct simple_sig sig;
    struct list_elem clients;
    const char *prog_name;
    pid_t pid;
    // user config
    char *hostname;
    char *port;
    char *database;
    //  state
    int epoll_fd; // epoll_create1
    unsigned int log_line : 1; // log request, response
};

static int poll_ctrl(struct simple_server *server, struct simple_sock *sock, uint32_t events);
static void client_destroy(struct simple_client *client, int can_log);
static void client_close(struct simple_client *client, int force);

static int send_line(struct simple_client *client, struct str_slice line)
{
    int rc = sock_write_line(&client->sock, line);
    if (rc) return rc;

    return 0;
}

static int send_rsp(struct simple_client *client, struct str_slice rsp)
{
    if (client->log_rsp) {
        log_info("LOG", "send-rsp: %.*s", SLICE(rsp));
    }

    return send_line(client, rsp);
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
    (void) args;
    struct str_slice res = slice_make(STR_LIT("OK"));

    send_rsp(client, res);

    client_close(client, 0);

    return 0;
}

static int cmd_unsupp(struct simple_client *client, struct str_slice args)
{
    (void) args;
    struct str_slice res = slice_make(STR_LIT("UNSUPP"));

    return send_rsp(client, res);
}

static struct {
    const char *name;
    size_t len;
    int (*func)(struct simple_client *client, struct str_slice args);
} cmds[] = {
    { 0, 0,  cmd_unsupp },
    { STR_LIT("SET"),  cmd_set },
    { STR_LIT("GET"),  cmd_get },
    { STR_LIT("DEL"),  cmd_del },
    { STR_LIT("QUIT"), cmd_quit },
};

static int find_cmd(struct str_slice cmd)
{
    for (size_t i = 1; i < ARR_LEN(cmds); i++) {
        if (slice_cmp_cstr(cmd, cmds[i].name, cmds[i].len)) {
            return i;
        }
    }

    // unsupp
    return 0;
}

int process_cmd(struct simple_client *client, struct str_slice cmd)
{
    struct str_slice name = slice_copy(cmd);
    struct str_slice args = slice_split(&name, ' ');

    name = slice_toupper(name);
    slice_trim(&args);

    int cmd_idx = find_cmd(name);
    int rc = cmds[cmd_idx].func(client, args);

    return rc;
}

static void client_close(struct simple_client *client, int force)
{
    // close writes
    sock_wrclose(&client->sock, force);
}

static void client_destroy(struct simple_client *client, int can_log)
{
    if (can_log) {
        log_info("+", "server-close %s", sock_tostr(&client->sock));
    }

    sock_deinit(&client->sock, can_log);

    if (list_inuse(&client->node)) {
        list_remove(&client->node);
    }

    free(client);
}

struct simple_client *client_create(int fd, struct sockaddr_in6 *addr)
{
    struct simple_client *client;

    client = malloc(sizeof(*client));
    if (!client) {
        return log_errno_rn("Create client failed");
    }
    memset(client, 0,  sizeof(*client));

    sock_init(&client->sock, fd, addr, SOCK_INIT_BUFSIZE, SOCK_MIN_BUFSIZE, SOCK_MAX_BUFSIZE);

    list_init(&client->node);

    return client;
}

#define RDWR_EVENTS (EPOLLOUT | EPOLLIN | EPOLLRDHUP)
#define RD_EVENTS (EPOLLIN | EPOLLRDHUP)

static void do_client_write(struct simple_client *client)
{
    int rc = sock_write(&client->sock);
    if (rc < 0) return;

    uint32_t events;
    if (sock_sendbuf_used(&client->sock)) {
        // partial write 
        events = client->sock.wait_write ? RDWR_EVENTS : 0;
    }
    else {
        // write complete
        events = client->sock.wait_write ? RD_EVENTS : 0;
    }

    if (events) {
        // need an epoll update
        int rc = poll_ctrl(client->parent, &client->sock, events);
        if (rc) return;
        client->sock.wait_write = events & EPOLLOUT ? 1 : 0;
    }
}

void do_client_read(struct simple_client *client)
{
    int eof = 0;
    int rc = sock_read(&client->sock);
    if (rc < 0) {
        // read error (SOCK_ERR|SOCK_CLOSED|SOCK_AGAIN)
        if (rc != SOCK_CLOSED) return;
         // client closed its end
         log_info("+", "client-disconnect %s", sock_tostr(&client->sock));
         client_close(client, 0);
         eof = 1;
    }

    // loop until no more lines or error
    struct str_slice line;
    while ((rc = sock_readline(&client->sock, &line, eof)) > 0) {
        if (client->log_req) log_info("LOG", "recv-req: %.*s", SLICE(line));
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
    return sock_canclose(&client->sock);
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
        sock->sys_err = 1;
        return log_errno_rf("epoll_ctl failed (fd=%d, op=%d,events=%u", sock->fd, op, events);
    }

    // registered
    sock->is_epoll = 1;

    // all done
    return 0;
}

struct simple_client *server_accept(struct simple_server *server)
{
    struct sockaddr_in6 addr;

    int fd = sock_accept(&server->sock, &addr);
    if (fd == -1) {
        // no connection available ?
        return NULL;
    }

    struct simple_client *client = client_create(fd, &addr);
    if (!client) {
        // out of memory ?
        close(fd);
        return log_error_rn("client_create failed!");
    }

    client->parent = server;
    client->log_req = server->log_line;
    client->log_rsp = server->log_line;

    // register with epoll - XXX readable events only
    if (poll_ctrl(server, &client->sock, RD_EVENTS) != 0) { 
        // register failed ?
        client_destroy(client, 1);
        return NULL;
    }

    // add to servers client list 
    list_append(&server->clients, &client->node);

    log_info("+", "Client connected from %s", sock_tostr(&client->sock));

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

    log_info("+", "Database stopped listening on %s", sock_tostr(&server->sock));
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

static int server_poll(struct simple_server *server)
{
    struct epoll_event events[MAX_EVENTS];

    int nfd = epoll_wait(server->epoll_fd, events, MAX_EVENTS, -1);

    if (nfd < 0) {
        if (errno == EINTR) return 0;
        return log_errno_rf("server PID:%d epoll_wait failed", server->pid);
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

static int server_run(struct simple_server *serv)
{
    while (serv->sig.run) {
        if (server_poll(serv) != 0) return -1;
    }

    if (serv->sig.signo) {
        log_info("+","server PID:%d shutting down: got signal %d (%s) from UID:%d PID:%d ", 
            serv->pid, 
            serv->sig.signo, strsignal(serv->sig.signo), 
            serv->sig.uid,
            serv->sig.pid);
    }

    return 0;
}

static int setup_listener(struct simple_server *server)
{
    int rc = sock_listen_hostport(&server->sock, server->hostname, server->port);
    if (rc) return rc;

    server->epoll_fd = epoll_create1(0);
    if (server->epoll_fd == -1) {
        return log_errno_rf("epoll_create1 failed");
    }

    // register for incoming connections
    if (poll_ctrl(server, &server->sock, EPOLLIN) != 0) {
        return -1;
    }

    log_info("+", "Database listening on %s", sock_tostr(&server->sock));

    // all done
    return 0;
}

static int setup_database(struct simple_server *serv)
{
    return db_init(serv->database);
}

/* cmd-line */

enum { opt_help, opt_host, opt_port, opt_dbase, opt_log, opt_argv };

struct cmd_opt opts[] = {
    // name, desc, def, has_arg
    { "--help",    "This help",              0,    0  },
    { "--hostname", "hostname to listen on", 0,    1  },
    { "--port",     "port to listen on",     SERV_PORT_STR, 1  },
    { "--database", "Path to database file", 0,        1  },
    { "--log",      "log request/response",  0,        0  },
    { "--argv",     "Dump argv to stdout",   0,        0  },
    { NULL }
};

static const char *examples[] = {
    "--hostname 127.0.0.1 --port 6379 --database mydb.bin",
    NULL
};

// process cmd-line options
static int server_parse_argv(struct simple_server *serv, int argc, char *argv[])
{
    struct cmd_argv parser = { argc, argv, opts };
    int rc;

    while ( (rc = cmd_argv_next(&parser)) >= 0) {
        switch(rc) {
        case opt_help:  print_usage(argv[0], opts, examples); exit(0);
        case opt_host:  rc = opt_setstr(&serv->hostname, &parser); break;
        case opt_port:  rc = opt_setstr(&serv->port, &parser); break;
        case opt_dbase: rc = opt_setstr(&serv->database, &parser); break;
        case opt_log:   serv->log_line = 1; break;
        case opt_argv:  log_argv("LOG", argc, argv); break;
        }
        if (rc < 0) break;
    }

    return rc == OPT_EOF ? 0 : -1;
}

static void server_free(struct simple_server *server)
{
    struct simple_client *client, *next;

    list_fornext_entry_safe(client, next, &server->clients, node) {
        list_remove(&client->node);
        client_destroy(client, 0);
    }

    if (server->sock.fd  != -1) close(server->sock.fd);
    if (server->epoll_fd != -1) close(server->epoll_fd);

    if (server->hostname) free(server->hostname);
    if (server->port)     free(server->port);
    if (server->database) free(server->database);

    free(server);
}

static void server_destroy(struct simple_server *server)
{
    db_deinit();

    server_free(server);
}

static int server_init(struct simple_server *server)
{
    memset(server, 0, sizeof(*server));
    server->sock.fd = -1;
    server->epoll_fd = -1;

    server->sock.is_server = 1;
    list_init(&server->clients);

    server->pid = getpid();

    // set defaults
    server->port = strdup(SERV_PORT_STR);
    if (!server->port) {
        return log_errno_rf("strdup %s", SERV_PORT_STR);
    }

    return 0;
}

static struct simple_server *server_create(void)
{
    struct simple_server *server;

    server = malloc(sizeof(*server));
    if (!server) {
        return log_errno_rn("Malloc failed for server state");
    }

    return server;
}

int main(int argc, char *argv[])
{
    struct simple_server *serv = NULL;
    int ec = EXIT_FAILURE;

    if (!(serv = server_create())) { ec = 1; goto done; }
    if (server_init(serv))    { ec = 2; goto done; }
    if (setup_signals(&serv->sig))  { ec = 3 ;goto done; }
    if (server_parse_argv(serv, argc, argv)) { ec = 4;  goto done; }
    if (setup_database(serv)) { ec = 5; goto done; }
    if (setup_listener(serv)) { ec = 6; goto done; }

    if (server_run(serv) != 0) { ec = 7; goto done; }

    // all done
    ec = 0;

done:
    if (serv) {
        server_destroy(serv);
    }

    return ec;
}
