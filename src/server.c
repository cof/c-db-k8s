/*
 * server - a TCP server than support simple commands to acesss a key/value store
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

    struct str_slice key = slice_make(args.ptr, key_len);
    struct str_slice val = ltrim(slice_make(pos, args.len - key_len));
    struct str_slice res;

    if (!key.len || ! val.len)
        res = slice_make(STR_LIT("FAIL"));
    else if (!db_set(key, val)) 
        res = slice_make(STR_LIT("FAIL"));
    else
        res = slice_make(STR_LIT("OK"));

    return send_rsp(client, res);
}

static int cmd_get(struct simple_client *client, struct str_slice key)
{
    struct str_slice res = db_get(key);

    if (!res.ptr)
        res = slice_make(STR_LIT("FAIL"));

    return send_rsp(client, res);
}

static int cmd_del(struct simple_client *client, struct str_slice key)
{
    struct str_slice res;

    if (!key.len)
        res = slice_make(STR_LIT("FAIL"));
    else if (!db_del(key))
        res = slice_make(STR_LIT("FAIL"));
    else 
        res = slice_make(STR_LIT("OK"));

    return send_rsp(client, res);
}

static int cmd_quit(struct simple_client *client, struct str_slice args)
{
    struct str_slice res = slice_make(STR_LIT("OK"));

    send_rsp(client, res);

    return ERR_QUIT;
}

static int cmd_unsupp(struct simple_client *client, struct str_slice args)
{
    struct str_slice res = slice_make(STR_LIT("UNSUPP"));

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
    struct str_slice cmd_args = ltrim(slice_make(args, cmd.len - cmd_len));

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
    int nw = sock_write(&client->sock, &client->write_buf);
    if (nw < 0) {
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
    struct str_slice line;
    int rc;

    rc = sock_read(&client->sock, &client->read_buf);
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
        log_info("Client closing %s", client->name);
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
        client_destroy(client);
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
        return log_errno("epoll_create1 failed");
    }

    // register for incoming connections
    if (poll_ctrl(server, &server->sock, EPOLLIN) != 0) {
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
        // parse
		struct str_slice host = slice_make(argv[1], strlen(argv[1]));
        struct str_slice port = slice_split(&host, ':');
        if (host.len && host.ptr[0] == '[') {
            host.ptr++; host.len--;
            if (host.ptr[host.len] == ']') host.len--;
        }
		// store
		if (host.len) server->host = strndup(host.ptr, host.len);
		if (port.len) server->port = strndup(port.ptr, port.len);
	}

    return 1;
}

static int init_state(struct simple_server *state)
{
    memset(state, 0, sizeof(*state));
    state->sock.is_server = 1;
    list_init(&state->clients);

	state->port = strdup(TCP_PORT_STR);
    if (!state->port) return 0;

    return 1;
}

int main(int argc, char *argv[])
{
    struct simple_server server;

    // XXX prevent write(fd) trigger a signal
    signal(SIGPIPE, SIG_IGN);

    if (!init_state(&server)) return 1;
    if (!parse_cmdline(&server, argc, argv)) return 2;
    if (!setup_database(&server)) return 3;
    if (!setup_listener(&server)) return 4;

    while (1) {
        do_poll(&server);
    }    

    return 0;
}

