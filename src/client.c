/*
 * client  : telnet like TCP client
 * Usage   : ./client --help
 * Example : ./client --hostname 127.0.0.1
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>

#include "config.h"
#include "util.h"
#include "log.h"
#include "sock.h"

struct client_config {
    const char *prog_name;
    const char *hostname;
    const char *port;
    int log;
};

struct my_conn {
    struct simple_sock socks[2];
    struct simple_sock *sock_send;
    struct simple_sock *sock_recv;
    int poll_in;
    int poll_out;
    uint8_t rdata[BUFSIZ];
    uint8_t wdata[BUFSIZ];
};

#define MY_CONN_INIT(_conn, _rfd, _wfd) \
    { \
      .socks[0] = SOCK_INIT(_rfd, 0, _conn.rdata, sizeof(_conn.rdata), _conn.wdata, sizeof(_conn.wdata)), \
      .socks[1] = SOCK_INIT(_wfd, 0, _conn.rdata, sizeof(_conn.rdata), _conn.wdata, sizeof(_conn.wdata)), \
      .sock_send = &_conn.socks[0], \
      .sock_recv = &_conn.socks[1], \
      .poll_in  = -1, \
      .poll_out = -1, \
      .rdata = { 0 }, \
      .wdata = { 0 } \
    }

static inline int my_conn_isbusy(struct my_conn *conn)
{
    return sock_isbusy(conn->sock_send);
}

static inline int my_conn_iseof(struct my_conn *conn)
{
    return sock_recveof(conn->sock_recv);
}

static inline int my_conn_isactive(struct my_conn *conn)
{
    return sock_isactive(conn->sock_recv) || sock_isactive(conn->sock_send);
}

static void my_conn_pipe(struct my_conn *src, struct my_conn *dst, 
    struct pollfd *fds, int *prompt, 
    const char *sender,
    const char *log_line)
{
    int read_eof = 0;
    int rc = sock_read(src->sock_recv);
    if (rc < 0) {
        // read error (SOCK_ERR|SOCK_CLOSED|SOCK_AGAIN)
        if (rc == SOCK_AGAIN) return;
        fds[src->poll_in].fd = -1;
        if (rc != SOCK_CLOSED) return;
        read_eof = 1;
        // push eof/es to dst
        sock_wrclose(dst->sock_send, 0);
        log_info("+", "Connection closed by %s", sender);
    }

    struct str_slice line;
    int sent_line = 0;

    while ((rc = sock_readline(src->sock_recv, &line, read_eof)) > 0) {
        if (log_line) {
            log_info("LOG", "%s: %.*s", log_line, SLICE(line));
        }
        rc = sock_sendline(dst->sock_send, line);
        if (rc < 0) {
            // write error
            fds[dst->poll_out].fd = -1;
            return;
        }
        sent_line = sock_sendbuf_used(dst->sock_send) == 0 ? 1 : 0;
        // enable writes if backlog has data
        fds[dst->poll_out].events = sent_line ? 0: POLLOUT;
    }

    if (prompt && sent_line) {
        *prompt = 1;
    }
}

static void my_conn_drain(struct my_conn *conn, struct pollfd *fds)
{
    int rc = sock_write(conn->sock_send);
    if (rc < 0) {
        // write error
        fds[conn->poll_out].fd = -1;
    }

    // enable writes if backlog has data
    fds[conn->poll_out].events = sock_sendbuf_used(conn->sock_send) ? POLLOUT : 0;
}

static int my_conn_stop(struct my_conn *user, struct my_conn *serv, struct pollfd *fds)
{
    if (!my_conn_isactive(user))  return 1;
    if (!my_conn_isactive(serv)) return 1;

    if (sock_wr_done(serv->sock_send)) {
        // nothing left to write
        int rc = sock_sendfin(serv->sock_send);
        if (rc) return 1;
        fds[serv->poll_out].fd = -1;
    }

    if (sock_wr_done(user->sock_send)) {
        // nothing left to write
        int rc = sock_sendfin(user->sock_send);
        if (rc) return 1;
        fds[user->poll_out].fd = -1;
    }

    int ingress_done = my_conn_iseof(user) && !my_conn_isbusy(serv);
    int egress_done  = my_conn_iseof(serv) && !my_conn_isbusy(user);

    if (egress_done && !ingress_done) {
        // server path gone
        ingress_done = 1;
    }

    return ingress_done && egress_done;
}

static int set_fd(struct simple_sock *sock, int idx, struct pollfd *fds, int events)
{
    fds[idx].fd = sock->fd;
    fds[idx].events = events;

    return idx;
}

static void my_conn_prompt(struct my_conn *conn)
{
    struct str_slice prompt = slice_make(STR_LIT("> "));
    sock_send_data(conn->sock_send, prompt);
}


/* cmd-line */
enum { SET_HELP = 0, SET_HOST, SET_PORT, SET_LOG };
static int set_opt(void *arg, size_t flag, const char *name, const char *val);

static struct cmd_opt opts[] = {
    OPT_GEN("--help",     "This help", 0, 0, SET_HELP, set_opt),
    OPT_GEN("--hostname", "hostname to listen on", 0, 1, SET_HOST, set_opt),
    OPT_GEN("--port",     "port to listen on", TCP_PORT_STR, 1, SET_PORT, set_opt),
    OPT_GEN("--log",      "log request/response", 0, 0,  SET_LOG, set_opt),
    { NULL }
};

static const char *examples[] = {
    "--hostname locahost --port 6379",
    NULL
};

static int set_opt(void *arg, size_t flag, const char *name, const char *val)
{
    struct client_config *cfg = arg;
    (void) name;
    (void) val;

    switch(flag) {
    case SET_HELP: print_usage(cfg->prog_name, opts, examples); exit(0);
    case SET_HOST: cfg->hostname = val; break;
    case SET_PORT: cfg->port = val; break;
    case SET_LOG:  cfg->log = 1; break;
    default: return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    struct client_config {
        const char *prog_name;
        const char *hostname;
        const char *port;
        int log;
    } cfg = {
        .prog_name = argv[0],
        .hostname = NULL,
        .port = TCP_PORT_STR,
        .log = 0
    };
    struct simple_sig sig;

    // process cmd-line options
    int rc = parse_argv(argc, argv, opts, &cfg);
    if (!rc) return -1;
    if (!cfg.hostname) fatal_error("Missing hostname");

    rc = setup_signals(&sig);
    if (rc) fatal_error("setup signals");

    // server connect
    struct my_conn serv = MY_CONN_INIT(serv, -1, -1);
    rc = sock_connect_hostport(&serv.socks[0], cfg.hostname, cfg.port);
    if (rc) fatal_error("No connection");
    serv.sock_send = serv.sock_recv = serv.socks;

    // at this stage safe to proceed
    log_info("+", "Connectivity test: OK");

    const char *log_req = cfg.log ? "send req" : NULL;
    const char *log_rsp = cfg.log ? "recv rsp" : NULL;
        
    // setup stdout,stdin for send,recv
    struct my_conn user = MY_CONN_INIT(user, STDOUT_FILENO, STDIN_FILENO);
    rc = sock_set_nonblock(user.sock_recv);
    if (rc) fatal_errno("set stdin non-block failed");
    rc = sock_set_nonblock(user.sock_send);
    if (rc) fatal_errno("set stdout  non-block failed");

    struct pollfd fds[4];
    user.poll_in  = set_fd(user.sock_recv, 0, fds, POLLIN);
    user.poll_out = set_fd(user.sock_send, 1, fds, 0);
    serv.poll_in  = set_fd(serv.sock_recv, 2, fds, POLLIN);
    serv.poll_out = set_fd(serv.sock_send, 3, fds, 0);

    int prompt = 0;
    my_conn_prompt(&user);

    while (sig.run) {
        // block until fd event or signal
        rc = poll(fds, ARR_LEN(fds), -1);
        if (rc == -1) {
            if (errno == EINTR) continue;
            log_errno("poll failed");
            break;
        }

        // server
        if (fds[3].revents & POLLOUT) {
            // backlog
            my_conn_drain(&serv, fds);
        }
        if (fds[2].revents & (POLLIN | POLLHUP | POLLERR)) {
            // recv server -> send user
            my_conn_pipe(&serv, &user, fds, &prompt, "server", log_rsp);
        }

        if (prompt) {
            my_conn_prompt(&user);
            prompt = 0;
        }

        // user
        if (fds[1].revents & POLLOUT) {
            // backlog
            my_conn_drain(&user, fds);
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            // recv user -> send server
            my_conn_pipe(&user, &serv, fds, NULL, "client", log_req);
            //raise(SIGSTOP);
        }

        if (my_conn_stop(&user, &serv, fds)) break;
    }

    sock_close(serv.sock_recv, 1);

    return 0;
}
