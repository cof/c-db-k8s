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

struct my_conn {
    struct simple_sock socks[2];
    struct simple_sock *sock_send;
    struct simple_sock *sock_recv;
    int poll_in;
    int poll_out;
    struct rwbuf rbuf;
    struct rwbuf wbuf;
    char rdata[BUFSIZ];
    char wdata[BUFSIZ];
};

static struct get_opt opts[] = {
    { "help",     "This help",             0, 'e' },
    { "hostname", "hostname to listen on", 1, 'h' },
    { "port",     "port to listen on",     1, 'p', GETOPT_DEFSTR(TCP_PORT_STR) },
    { "log",      "log request/response",  0, 'l' },
    { "argv",     "Dump argv to stdout",   0, 'a' }
};

static char *examples[] = {
    "--hostname locahost --port 6379"
};

#define MY_CONN_INIT(_sock, _rfd, _wfd) \
    { \
      .socks[0].fd = (_rfd), \
      .socks[1].fd = (_wfd), \
      .sock_send = &_sock.socks[0], \
      .sock_recv = &_sock.socks[1], \
      .poll_in  = -1, \
      .poll_out = -1, \
      .rbuf = RWBUF_LOAD(_sock.rdata, sizeof(_sock.rdata)), \
      .wbuf = RWBUF_LOAD(_sock.wdata, sizeof(_sock.wdata)), \
      .rdata = { 0 }, \
      .wdata = { 0 } \
    }

static inline int my_conn_isbusy(struct my_conn *conn)
{
    return conn->wbuf.len > 0 || sock_isbusy(conn->sock_send);
}
static inline int my_conn_iseof(struct my_conn *conn)
{
    return conn->rbuf.len == 0 && sock_recveof(conn->sock_recv);
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
    int rc = sock_read(src->sock_recv, &src->rbuf);
    if (rc < 0) {
        // read error or eof
        fds[src->poll_in].fd = -1;
        if (rc != SOCK_CLOSED) return;
        read_eof = 1;
        // push eof/es to dst
        sock_wrclose(dst->sock_send, 0);
        log_info("+", "Connection closed by %s", sender);
    }

    struct str_slice line;

    while ((rc = read_line(&src->rbuf, &line, read_eof)) > 0) {
        if (log_line) {
            log_info("LOG", "%s: %.*s", log_line, SLICE(line));
        }
        rc = sock_write_line(dst->sock_send, &dst->wbuf, line);
        if (rc < 0) {
            // write error
            fds[dst->poll_out].fd = -1;
            return;
        }
        // enable writes if backlog has data
        fds[dst->poll_out].events = dst->wbuf.len ? POLLOUT : 0;
        if (prompt && dst->wbuf.len == 0) *prompt = 1;
    }

    if (rc < 0) {
        // line too big fail it
        sock_seterr(src->sock_recv);
    }
}

static void my_conn_drain(struct my_conn *conn, struct pollfd *fds)
{
    int rc = sock_write(conn->sock_send, &conn->wbuf);
    if (rc < 0) {
        // write error
        fds[conn->poll_out].fd = -1;
    }

    // enable writes if backlog has data
    fds[conn->poll_out].events = conn->wbuf.len ? POLLOUT : 0;
}

static int my_conn_stop(struct my_conn *user, struct my_conn *serv, struct pollfd *fds)
{
    if (!my_conn_isactive(user) || !my_conn_isactive(serv)) return 1;

    if (sock_isclosing(serv->sock_send) && serv->wbuf.len == 0) {
        // nothing left to write
        int rc = sock_sendfin(serv->sock_send);
        if (rc) return 1;
        fds[serv->poll_out].fd = -1;
    }

    if (sock_isclosing(user->sock_send) && user->wbuf.len == 0) {
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
    sock_write_data(conn->sock_send, &conn->wbuf, prompt);
}

// signal handling
volatile sig_atomic_t keep_running = 1;
volatile sig_atomic_t caught_signo = 0; 
volatile sig_atomic_t sender_pid = 0; 
volatile sig_atomic_t sender_uid = 0; 

static void handle_signal(int signo, siginfo_t *info, void *ucontext)
{
    (void) ucontext;
    caught_signo = signo;

    sender_pid = 0;
    sender_uid = 0;

    if (info->si_code <= 0) {
        sender_pid = info->si_pid;
        sender_uid = info->si_uid;
    }

    keep_running = 0;
}

static int setup_signals(void)
{
    struct sigaction sa = { 0 };

    sa.sa_sigaction = handle_signal;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        return log_errno_rf("setup sigint");
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        return log_errno_rf("setup sigterm");
    }

    // XXX prevent write(fd) trigger a signal
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        return log_errno_rf("setup SIGPIPE");
    }

    return 0;
}

int main(int argc, char *argv[])
{
    const char *hostname = NULL;
    const char *port = TCP_PORT_STR;
    int log = 0;

    // process cmd-line options
    struct getopt_parse parse;
    int rc = getopt_init(&parse, argc, argv, ARRAY(opts));
    if (rc) fatal_error("cmd-line parser failed");
    while ((rc = getopt_next(&parse)) >= 0) {
        switch(rc) {
        case 'e': print_usage(argv[0], ARRAY(opts), ARRAY(examples)); return 0;
        case 'h': hostname = getopt_str(&parse); break;
        case 'p': port = getopt_str(&parse); break;
        case 'l': log = 1; break;
        case 'a': log_argv("+", argc, argv); break;
        }
    }
    if (rc != GETOPT_EOF) return -1;
    if (!hostname) fatal_error("Missing hostname");

    rc = setup_signals();
    if (rc) fatal_error("setup signals");

    // server connect
    struct my_conn serv = MY_CONN_INIT(serv, -1, -1);
    rc = sock_connect_hostport(&serv.socks[0], hostname, port);
    if (rc) fatal_error("No connection");
    serv.sock_send = serv.sock_recv = serv.socks;

    // at this stage safe to proceed
    log_info("+", "Connectivity test: OK");

    const char *log_req = log ? "send req" : NULL;
    const char *log_rsp = log ? "recv rsp" : NULL;
        
    // setup stdout,stdin for send,recv
    struct my_conn user = MY_CONN_INIT(user, STDOUT_FILENO, STDIN_FILENO);
    rc = sock_set_noblock(user.sock_recv);
    if (rc) fatal_errno("set stdin non-block failed");
    rc = sock_set_noblock(user.sock_send);
    if (rc) fatal_errno("set stdout  non-block failed");

    struct pollfd fds[4];
    user.poll_in  = set_fd(user.sock_recv, 0, fds, POLLIN);
    user.poll_out = set_fd(user.sock_send, 1, fds, 0);
    serv.poll_in  = set_fd(serv.sock_recv, 2, fds, POLLIN);
    serv.poll_out = set_fd(serv.sock_send, 3, fds, 0);

    int prompt = 0;
    my_conn_prompt(&user);

    while (keep_running) {
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
