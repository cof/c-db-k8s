/*
 * client  : telnet like TCP client
 * Usage   : ./client --help
 * Example : ./client --hostname 127.0.0.1
 *
 * Overview
 * --------
 * Implements a TCP wrapper or socket bridge between stdio/socket.
 * Client acts as 4 way pipe reading lines from stdin, sending them to socket
 * and reading lines from the socket and sending them to stdout.
 *
 *  stdin -> socket write (local to remote)
 *  socket read -> stdout (remote to local)
 *
 * Notes:
 * - code is single threaded 
 * - code is using sockets fd via sock API
 * - code wraps stdin and stdout into sock API
 * - code is using poll for fd activity
 */
#include <netdb.h>
#include <poll.h>

#include "config.h"
#include "util.h"
#include "log.h"
#include "sock.h"

// pipe state
struct my_pipe {
    struct simple_sock socks[2];
    struct simple_sock *send_sock;
    struct simple_sock *recv_sock;
    int poll_in;
    int poll_out;
    uint8_t rdata[BUFSIZ];
    uint8_t wdata[BUFSIZ];
};

#define MY_PIPE_INIT(_conn, _rfd, _wfd) \
    { \
      .socks[0] = SOCK_INIT(_rfd, MAX_LINE, 0, _conn.rdata, sizeof(_conn.rdata), _conn.wdata, sizeof(_conn.wdata)), \
      .socks[1] = SOCK_INIT(_wfd, MAX_LINE, 0, _conn.rdata, sizeof(_conn.rdata), _conn.wdata, sizeof(_conn.wdata)), \
      .send_sock = &_conn.socks[0], \
      .recv_sock = &_conn.socks[1], \
      .poll_in  = -1, \
      .poll_out = -1, \
      .rdata = { 0 }, \
      .wdata = { 0 } \
    }

// true if send-sock is busy or send_sock must be closed
static inline int my_pipe_isbusy(struct my_pipe *conn)
{
    return sock_isbusy(conn->send_sock) && !sock_mustclose(conn->send_sock);
}

// true if recv-sock finised reading or must close
static inline int my_pipe_iseof(struct my_pipe *conn)
{
    return sock_read_done(conn->recv_sock) || sock_mustclose(conn->recv_sock);
}

// pipe read from src and write to dst
static void my_pipe_readwrite(struct my_pipe *src, struct my_pipe *dst, 
    struct pollfd *fds, const char *sender, const char *log_line)
{
    int rc;

    while ((rc = sock_recv(src->recv_sock)) == SOCK_DATA) {
        // send src-data to dst
        struct str_slice str = sock_recv_str(src->recv_sock);
        rc = sock_send_str(dst->send_sock, str);
        if (rc < 0) {
            // dst write error - bail
            fds[dst->poll_out].fd = -1;
            return;
        }
        // enable write poll if backlog data
        fds[dst->poll_out].events = sock_sendbuf_used(dst->send_sock) ? POLLOUT : 0;
        if (!log_line) {
            // discard buffer
            sock_recvbuf_consume(src->recv_sock, str.len);
            continue;
        }
        // log lines - reuse recv buffer
        int is_eof = sock_iseof(src->recv_sock);
        while ((rc = sock_recv_line(src->recv_sock, &str, is_eof)) > 0) {
            log_info("LOG", "%s: %.*s", log_line, SLICE(str));
        }
        if (rc < 0) break;
        str = sock_recv_str(src->recv_sock);
        if (slice_cmp_cstr(str, STR_LIT("> "))) {
            // discard prompt
            sock_recvbuf_consume(src->recv_sock, str.len);
        }
    }

    if (rc < 0) {
        // error
        if (rc == SOCK_AGAIN) return;
        fds[src->poll_in].fd = -1;
        if (rc != SOCK_CLOSED) return;
        // push eof/es to dst
        sock_write_close(dst->send_sock, 0);
        log_info("+", "Connection closed by %s", sender);
    }
}

// pipe check if send-sock buffer is drained
static void my_pipe_drain(struct my_pipe *conn, struct pollfd *fds)
{
    int rc = sock_send(conn->send_sock);
    if (rc < 0) {
        // write error
        fds[conn->poll_out].fd = -1;
    }

    // enable writes if backlog has data
    fds[conn->poll_out].events = sock_sendbuf_used(conn->send_sock) ? POLLOUT : 0;
}

// check if ingress and egress write paths done
static int my_pipe_stop(struct my_pipe *user, struct my_pipe *serv, struct pollfd *fds)
{
    if (sock_write_done(serv->send_sock)) {
        // nothing left to write
        int rc = sock_sendfin(serv->send_sock);
        if (rc) return 1;
        fds[serv->poll_out].fd = -1;
    }

    if (sock_write_done(user->send_sock)) {
        // nothing left to write
        int rc = sock_sendfin(user->send_sock);
        if (rc) return 1;
        fds[user->poll_out].fd = -1;
    }

    int ingress_done = my_pipe_iseof(user) && !my_pipe_isbusy(serv);
    int egress_done  = my_pipe_iseof(serv) && !my_pipe_isbusy(user);

    if (egress_done && !ingress_done) {
        // server path gone
        ingress_done = 1;
    }

    // all done
    return ingress_done && egress_done;
}

static int set_fd(struct simple_sock *sock, int idx, struct pollfd *fds, int events)
{
    fds[idx].fd = sock->fd;
    fds[idx].events = events;

    return idx;
}

/* cmd-line */
enum { opt_help, opt_host, opt_port, opt_log, opt_argv };

static struct cmd_opt opts[] = {
    // name, desc, def, has_arg
    { "--help",     "This help", 0, 0 },
    { "--hostname", "hostname to connect to", 0, 1 },
    { "--port",     "port to listen on", SERV_PORT_STR, 1 },
    { "--log",      "log request/response", 0, 0 },
    { "--argv",     "Dump argv to stdout",  0,  0 },
    { NULL }
};

static const char *examples[] = {
    "--hostname localhost --port 6379",
    NULL
};

int main(int argc, char *argv[])
{
    const char *hostname = NULL;
    const char *port = SERV_PORT_STR;
    int log = 0;

    // process cmd-line options
    int rc;
    struct cmd_argv parser = { argc, argv, opts };
    while ( (rc = cmd_argv_next(&parser)) >= 0) {
        switch(rc) {
        case opt_help: print_usage(argv[0], opts, examples); exit(0);
        case opt_host: hostname = parser.value; break;
        case opt_port: port = parser.value; break;
        case opt_log:  log = 1; break;
        case opt_argv: log_argv("LOG", argc, argv); break;
        }
    }
    if (rc != OPT_EOF) return -1;
    if (!hostname) fatal_error("Missing hostname");

    // signals
    struct simple_sig sig;
    rc = setup_signals(&sig);
    if (rc) fatal_error("setup signals");

    // server connect
    struct my_pipe serv = MY_PIPE_INIT(serv, -1, -1);
    rc = sock_client(&serv.socks[0], SOCK_TCP | SOCK_NONBLK, hostname, port);
    if (rc) fatal_error("No connection");
    serv.send_sock = serv.recv_sock = serv.socks;

    // at this stage safe to proceed
    log_info("+", "Connectivity test: OK");

    const char *log_req = log ? "send req" : NULL;
    const char *log_rsp = log ? "recv rsp" : NULL;
        
    // setup stdout,stdin for send,recv
    struct my_pipe user = MY_PIPE_INIT(user, STDOUT_FILENO, STDIN_FILENO);
    if (sock_set_mode(user.send_sock, SOCK_FILE | SOCK_NONBLK)) exit(1);
    if (sock_set_mode(user.recv_sock, SOCK_FILE | SOCK_NONBLK)) exit(1);

    struct pollfd fds[4];
    user.poll_in  = set_fd(user.recv_sock, 0, fds, POLLIN);
    user.poll_out = set_fd(user.send_sock, 1, fds, 0);
    serv.poll_in  = set_fd(serv.recv_sock, 2, fds, POLLIN);
    serv.poll_out = set_fd(serv.send_sock, 3, fds, 0);

    while (sig.run) {
        // block until event or signal
        rc = poll(fds, ARR_LEN(fds), -1);
        if (rc == -1) {
            if (errno == EINTR) continue;
            log_errno("poll failed");
            break;
        }

        // handle server events
        if (fds[3].revents & POLLOUT) {
            // backlog
            my_pipe_drain(&serv, fds);
        }
        if (fds[2].revents & (POLLIN | POLLHUP | POLLERR)) {
            // recv server -> send user
            my_pipe_readwrite(&serv, &user, fds, "server", log_rsp);
        }

        // handle user events
        if (fds[1].revents & POLLOUT) {
            // backlog
            my_pipe_drain(&user, fds);
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            // recv user -> send server
            my_pipe_readwrite(&user, &serv, fds, "user", log_req);
            //raise(SIGSTOP);
        }

        if (my_pipe_stop(&user, &serv, fds)) break;
    }

    // close server socket
    sock_close(serv.recv_sock, 1);

    return 0;
}
