/*
 * server   : TCP server than supports telnet api to acesss a key/value store
 * Usage    : ./server --help
 * Example  : ./server --hostnanme 127.0.0.1 --port 5379
 *
 * Overview
 * -------
 * Implements a TCP server that supports CLI cmds to update a key value store.
 *
 * Supported commands:
 *
 *  SET key value - store a key value
 *  GET key       - retrive a key value
 *  DEL key       - delete key/value from store
 *  QUIT          - close connection
 *
 * Notes:
 * - code is single threaded 
 * - code is using dual-stack sockets
 * - code is using epoll for socket fd activity
 * - default listen is [::]:6379
 * - commands are case insensitve
 * - see config.h for current defaults
 */
#include <netdb.h> 
#include <sys/epoll.h>
#include <errno.h>

#include "config.h"
#include "util.h"
#include "list.h"
#include "log.h"
#include "dns_resolv.h"
#include "sock.h"
#include "db.h"

struct simple_client {
    struct simple_sock sock;
    struct list_elem node;
    struct simple_server *parent;
    unsigned int log_line : 1; // log req|rsp lines
    unsigned int snd_prompt : 1; // send prompt after rsp
};

struct simple_server {
    struct simple_sock sock;
    struct simple_sig sig;
    struct list_elem clients;
    pid_t pid;
    // user config
    char *hostname;
    char *port;
    char *database;
    //  state
    int epoll_fd; // epoll_create1
    unsigned int log_line : 1; // log req|rsp lines
};

static int poll_ctrl(struct simple_server *server, struct simple_sock *sock, uint32_t events);
static void client_destroy(struct simple_client *client, int can_log);
static void client_close(struct simple_client *client, int force);

static int send_prompt(struct simple_client *client) 
{
    return sock_write_mem(&client->sock, STR_LIT("> "));
}

static int send_line(struct simple_client *client, struct str_slice line)
{
    return sock_write_line(&client->sock, line);
}

static int send_rsp(struct simple_client *client, struct str_slice rsp)
{
    if (client->log_line) {
        log_info("LOG", "send-rsp: %.*s", SLICE(rsp));
    }

    int rc = send_line(client, rsp);
    if (!rc && client->snd_prompt) {
        rc = send_prompt(client);
    }

    return rc;
}

// handler - SET key value
static int cmd_set(struct simple_client *client, struct str_slice args)
{
    struct str_slice val = slice_copy(args);
    struct str_slice key = slice_splitch(&val, ' ');  
    slice_trim(&key);
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

// handler - GET key
static int cmd_get(struct simple_client *client, struct str_slice key)
{
    struct str_slice res = db_get(key);

    if (!res.ptr) {
        res = slice_make(STR_LIT("FAIL"));
    }

    return send_rsp(client, res);
}

// handler - DEL key
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

// handler - QUIT
static int cmd_quit(struct simple_client *client, struct str_slice args)
{
    (void) args;
    struct str_slice res = slice_make(STR_LIT("OK"));

    client->snd_prompt = 0;
    int rc = send_rsp(client, res);
    client_close(client, rc == 0);

    return rc;
}

// take a wild guess
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
} cli_cmds[] = {
    { 0, 0,  cmd_unsupp },
    { STR_LIT("SET"),  cmd_set },
    { STR_LIT("GET"),  cmd_get },
    { STR_LIT("DEL"),  cmd_del },
    { STR_LIT("QUIT"), cmd_quit },
};

static int find_cli_cmd(struct str_slice cmd)
{
    for (size_t i = 1; i < ARR_LEN(cli_cmds); i++) {
        if (!slice_cmpmem(cmd, cli_cmds[i].name, cli_cmds[i].len)) {
            return i;
        }
    }

    // unsupp
    return 0;
}

static int process_line(struct simple_client *client, struct str_slice cmd)
{
    struct str_slice name = slice_splitch(&cmd, ' ');
    name = slice_toupper(name);
    slice_trim(&cmd);

    int cmd_idx = find_cli_cmd(name);
    int rc = cli_cmds[cmd_idx].func(client, cmd);

    return rc;
}

static void client_close(struct simple_client *client, int force)
{
    // close writes
    sock_write_close(&client->sock, force);
}

static void client_destroy(struct simple_client *client, int rc)
{
    if (rc == 0) {
        log_info("+", "server-close %s", sock_tostr(&client->sock));
    }

    sock_deinit(&client->sock, rc);
    if (list_inuse(&client->node)) {
        list_remove(&client->node);
    }
    free(client);
}

// create client for fd + addr
struct simple_client *client_create(int fd, struct sock_addr *addr)
{
    struct simple_client *client;

    client = malloc(sizeof(*client));
    if (!client) return log_errno_rn("Create client failed");
    memset(client, 0, sizeof(*client));

    // use config.h settings
    sock_init(&client->sock, fd, addr, 
        MAX_LINE, SOCK_INIT_BUFSIZE, SOCK_MIN_BUFSIZE, SOCK_MAX_BUFSIZE
    );

    // self link
    list_init(&client->node);

    return client;
}

#define RDWR_EVENTS (EPOLLOUT | EPOLLIN | EPOLLRDHUP)
#define RD_EVENTS (EPOLLIN | EPOLLRDHUP)

// flush send-buffer to socket fd
static void do_client_send(struct simple_client *client)
{
    int rc = sock_send(&client->sock);
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
        rc = poll_ctrl(client->parent, &client->sock, events);
        if (rc) return;
        client->sock.wait_write = events & EPOLLOUT ? 1 : 0;
    }
}

// recv-data from sock
void do_client_recv(struct simple_client *client)
{
    int rc;

    while ((rc = sock_recv(&client->sock)) == SOCK_DATA) {
        struct str_slice line;
        int is_eof = sock_iseof(&client->sock);
        // recv cmd-line
        while ((rc = sock_recv_line(&client->sock, &line, is_eof)) > 0) {
            if (client->log_line) log_info("LOG", "recv-req: %.*s", SLICE(line));
            rc = process_line(client, line);
            if (rc != 0) break;
        }
        if (rc < 0) break;
    }

    if (rc < 0) {
        // error
        if (rc == SOCK_AGAIN) return;
        if (rc == SOCK_CLOSED) {
            // client closed its end
            log_info("+", "client-disconnect %s", sock_tostr(&client->sock));
        }
        // close now
        client_close(client, rc != SOCK_CLOSED);
    }
}

static int client_must_close(struct simple_client *client)
{
    return sock_canclose(&client->sock);
}

static void client_recv_event(struct simple_client *client, uint32_t events)
{
    if (events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
        do_client_send(client);
    }

    if (events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
        do_client_recv(client);
        do_client_send(client);
    }

    if (client_must_close(client)) {
        client_destroy(client, 1);
    }
}

// add or remove socket fd from epoll ctrl
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

// accept incoming client 
struct simple_client *server_accept(struct simple_server *server)
{
    struct sock_addr addr;

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
    client->log_line = server->log_line;

    // register with epoll - readable events only
    if (poll_ctrl(server, &client->sock, RD_EVENTS) != 0) { 
        // register failed ?
        client_destroy(client, 1);
        return NULL;
    }

    // add to servers client list 
    list_append(&server->clients, &client->node);

    // send welcome banner
    client->snd_prompt = 1;
    send_prompt(client);
    do_client_send(client);

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

    // set error state
    server->sock.sys_err = 1;
}

// check if we stop server
static void do_server_check(struct simple_server *server)
{
    if (!server->sock.sys_err) return;

    // stop server
    sock_close(&server->sock, -1);
    server->sig.run = 0;

    log_info("+", "Database stopped listening on %s", sock_tostr(&server->sock));
}

static void server_recv_event(struct simple_server *server, uint32_t events)
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

// wait for I/O event
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
            server_recv_event(server, events[i].events);
        }
        else {
            client_recv_event((struct simple_client *) sock, events[i].events);
        }
    }

    // all done
    return 0;
}

// main loop for server events
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

// add a socket listener
static int setup_listener(struct simple_server *server)
{
    uint32_t mode = SOCK_LISTEN | SOCK_NONBLK | SOCK_TCP;
    int rc = sock_server(&server->sock, mode, server->hostname, server->port);
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

enum { opt_help, opt_host, opt_port, opt_dbase, opt_logline, opt_loglevel, opt_argv };

struct cmd_opt opts[] = {
    // name, desc, def, has_arg
    { "--help",      "This help",              0,    0  },
    { "--hostname",  "hostname to listen on", 0,    1  },
    { "--port",      "port to listen on",     SERV_PORT_STR, 1  },
    { "--database",  "Path to database file", 0, 1  },
    { "--log-line",  "log request and response lines",  0, 0  },
    { "--log-level", "logging level ",        STR(APP_LOGLEVEL), 1  },
    { "--argv",      "Dump argv to stdout",   0, 0  },
    { NULL }
};

static const char *examples[] = {
    "--hostname 127.0.0.1 --port 6379 --database mydb.bin",
    NULL
};

// process cmd-line options
static int server_argv(struct simple_server *serv, int argc, char *argv[])
{
    struct cmd_argv parser = { argc, argv, opts };
    int rc;

    while ( (rc = cmd_argv_next(&parser)) >= 0) {
        switch(rc) {
        case opt_help:  prog_usage(argv[0], opts, examples); exit(0);
        case opt_host:  rc = opt_setstr(&serv->hostname, &parser); break;
        case opt_port:  rc = opt_setstr(&serv->port, &parser); break;
        case opt_dbase: rc = opt_setstr(&serv->database, &parser); break;
        case opt_logline:  serv->log_line = 1; break;
        case opt_loglevel: rc = opt_setint(&log_level, &parser); break;
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

static struct simple_server *server_create(void)
{
    struct simple_server *server;

    server = malloc(sizeof(*server));
    if (!server) return log_errno_rn("Malloc failed for server state");

    memset(server, 0, sizeof(*server));
    server->sock.fd = -1;
    server->epoll_fd = -1;
    server->sock.is_server = 1;
    list_init(&server->clients);
    server->pid = getpid();

    // set defaults
    server->port = strdup(SERV_PORT_STR);
    if (!server->port) return log_errno_rn("strdup %s", SERV_PORT_STR);

    return server;
}

int main(int argc, char *argv[])
{
    struct simple_server *serv = NULL;
    int ec = EXIT_FAILURE;

    log_init(NULL, LOG_INFO);

    if (!(serv = server_create())) { ec = 1;  goto done; }
    if (server_argv(serv, argc, argv)) { ec = 2; goto done; }
    if (setup_signals(&serv->sig)) { ec = 3 ; goto done; }
    if (dns_init(0, 0, &serv->sig))  { ec = 4; goto done; }
    if (setup_database(serv))  { ec = 5; goto done; }
    if (setup_listener(serv))  { ec = 6; goto done; }
    if (server_run(serv) != 0) { ec = 7; goto done; }

    // all done
    ec = 0;

done:
    if (serv) server_destroy(serv);

    return ec;
}
