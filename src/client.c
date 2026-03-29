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
    struct simple_sock *sock_send;
    struct simple_sock *sock_recv;
    int poll_in;
    int poll_out;
    uint8_t rdata[BUFSIZ];
    uint8_t wdata[BUFSIZ];
};

#define my_pipe_INIT(_conn, _rfd, _wfd) \
    { \
      .socks[0] = SOCK_INIT(_rfd, MAX_LINE, 0, _conn.rdata, sizeof(_conn.rdata), _conn.wdata, sizeof(_conn.wdata)), \
      .socks[1] = SOCK_INIT(_wfd, MAX_LINE, 0, _conn.rdata, sizeof(_conn.rdata), _conn.wdata, sizeof(_conn.wdata)), \
      .sock_send = &_conn.socks[0], \
      .sock_recv = &_conn.socks[1], \
      .poll_in  = -1, \
      .poll_out = -1, \
      .rdata = { 0 }, \
      .wdata = { 0 } \
    }

// true if send-sock is busy or send_sock must be closed
static inline int my_pipe_isbusy(struct my_pipe *conn)
{
    return sock_isbusy(conn->sock_send) && !sock_mustclose(conn->sock_send);
}

// true if recv-sock finised reading or must close
static inline int my_pipe_iseof(struct my_pipe *conn)
{
    return sock_read_done(conn->sock_recv) || sock_mustclose(conn->sock_recv);
}

// pipe read from src and write to dst
static void my_pipe_readwrite(struct my_pipe *src, struct my_pipe *dst, 
    struct pollfd *fds, int *prompt, 
    const char *sender,
    const char *log_line)
{
    int read_eof = 0;
    int rc = sock_recv(src->sock_recv);
    if (rc < 0) {
        // read error (SOCK_ERR|SOCK_CLOSED|SOCK_AGAIN)
        if (rc == SOCK_AGAIN) return;
        fds[src->poll_in].fd = -1;
        if (rc != SOCK_CLOSED) return;
        read_eof = 1;
        // push eof/es to dst
        sock_write_close(dst->sock_send, 0);
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

// pipe check if send-sock buffer is drained
static void my_pipe_drain(struct my_pipe *conn, struct pollfd *fds)
{
    int rc = sock_send(conn->sock_send);
    if (rc < 0) {
        // write error
        fds[conn->poll_out].fd = -1;
    }

    // enable writes if backlog has data
    fds[conn->poll_out].events = sock_sendbuf_used(conn->sock_send) ? POLLOUT : 0;
}

// check if ingress and egress write paths done
static int my_pipe_stop(struct my_pipe *user, struct my_pipe *serv, struct pollfd *fds)
{
    if (sock_write_done(serv->sock_send)) {
        // nothing left to write
        int rc = sock_sendfin(serv->sock_send);
        if (rc) return 1;
        fds[serv->poll_out].fd = -1;
    }

    if (sock_write_done(user->sock_send)) {
        // nothing left to write
        int rc = sock_sendfin(user->sock_send);
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

static void my_pipe_prompt(struct my_pipe *conn)
{
    struct str_slice prompt = slice_make(STR_LIT("> "));
    sock_send_str(conn->sock_send, prompt);
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
    struct my_pipe serv = my_pipe_INIT(serv, -1, -1);
    rc = sock_client(&serv.socks[0], SOCK_TCP | SOCK_NONBLK, hostname, port);
    if (rc) fatal_error("No connection");
    serv.sock_send = serv.sock_recv = serv.socks;

    // at this stage safe to proceed
    log_info("+", "Connectivity test: OK");

    const char *log_req = log ? "send req" : NULL;
    const char *log_rsp = log ? "recv rsp" : NULL;
        
    // setup stdout,stdin for send,recv
    struct my_pipe user = my_pipe_INIT(user, STDOUT_FILENO, STDIN_FILENO);
    if (sock_set_mode(user.sock_send, SOCK_FILE | SOCK_NONBLK)) exit(1);
    if (sock_set_mode(user.sock_recv, SOCK_FILE | SOCK_NONBLK)) exit(1);

    struct pollfd fds[4];
    user.poll_in  = set_fd(user.sock_recv, 0, fds, POLLIN);
    user.poll_out = set_fd(user.sock_send, 1, fds, 0);
    serv.poll_in  = set_fd(serv.sock_recv, 2, fds, POLLIN);
    serv.poll_out = set_fd(serv.sock_send, 3, fds, 0);

    int prompt = 0;
    my_pipe_prompt(&user);

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
            my_pipe_drain(&serv, fds);
        }
        if (fds[2].revents & (POLLIN | POLLHUP | POLLERR)) {
            // recv server -> send user
            my_pipe_readwrite(&serv, &user, fds, &prompt, "server", log_rsp);
        }

        if (prompt) {
            my_pipe_prompt(&user);
            prompt = 0;
        }

        // user
        if (fds[1].revents & POLLOUT) {
            // backlog
            my_pipe_drain(&user, fds);
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            // recv user -> send server
            my_pipe_readwrite(&user, &serv, fds, NULL, "client", log_req);
            //raise(SIGSTOP);
        }

        if (my_pipe_stop(&user, &serv, fds)) break;
    }

    // close server socket
    sock_close(serv.sock_recv, 1);

    return 0;
}
