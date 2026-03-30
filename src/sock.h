/*
 * SOCK - A simple socket layer API
 * --------------------------------
 * A simple API for the linux socket ABI featuring
 * - Structure-composable: built for inline embedding, object compostion & memory locality
 * - Flexible memory: Support dynamic or user supplied static buffers
 * - FD-agnostic: allows seamless handling of sockets, pipes and standard I/O
 * - Multiplexor-agnostic : pure I/O primitives for poll, epoll or blocking loops
 * - Non-blocking I/O : built-in state tracking  asynchronous, event-driven flows
 * - Line-bufferd I/O : integrated readline and writeline with max length enforcement
 * - Integratd DNS : resolver support for hostname and service port lookups
 * - Zero-copy scatter-gather - vectorzed tranfers via writev
 * - Diagnostics: robust error capture and trace logging
 * - Clean termination - graceful half close (FIN) and shutdown management
 * - Human-readable formatting: string repr of socket address or file descriptor
 *
 * Example usage:
 * -------------
 * 
 *  char line[BUFSIZ];
 *  struct simple_sock sock;
 *  sock_client(&sock, SOCK_TCP, "localhost", 10000);
 *  sock_send_data(&sock, STR_LIT("hello world\n"));
 *  sock_recv_data(&sock, line, sizeof(line));
 *  sock_close(&sock);
 *  printf("%s", buf);
 *
 * See full API below.
 *
 * API sections
 * ------------
 * Init       : Init sock state
 * Connection : Create or close client|server socket connections
 * State      : Update socket mode or fd state
 * FD I/O     : Read|Write memory buffers to|from file descriptor
 * buffer I/O : send|recv sock buffers to|from file descriptor
 * line   I/O : Read and write lines
 * Status     : Socket status and info
 */
#ifndef _SOCK_H_
#define _SOCK_H_

#include "rwbuf.h"

// socket errors
#define SOCK_OK       0 // read return 0 - nothing to do
#define SOCK_DATA     1 // read or write worked
#define SOCK_ERROR   -1 // open|read|write|close error
#define SOCK_AGAIN   -2 // read would block (EAGAIN|EWOULDBLOCK)
#define SOCK_CLOSED  -3 // read returned 0
#define SOCK_TIMEOUT -4 // read|write timeout

#define MAX_EVENTS    10
#define BUF_MINSIZE 4096

// big enough for "[" host "]" :" port + null
#define MAX_HOSTPORT (4 + NI_MAXHOST + NI_MAXSERV)

/*
 * Mode is bit-wise mask of the following flags:
 * - type     : SOCK_TCP|SOCK_UDP|SOCK_FILE
 * - behavior : SOCK_NONBLK|SOCK_RESUSE|SOCK_UDPCON|SOCK_PASSIVE
 * - resolver : SOCK_ANY|SOCK_IPV4|SOCK_IPV6|SOCK_NUMSERV
 */
// type 
#define SOCK_TCP      (1u << 0)  // stream network socket
#define SOCK_UDP      (1u << 1)  // datagram nework socket
#define SOCK_FILE     (1u << 2)  // pipe or standard I/O fd
// behavior
#define SOCK_NONBLK   (1u << 3)  // enable non-blocking state
#define SOCK_REUSE    (1u << 4)  // set SO_RESUSEADDR for fast server restart
#define SOCK_UDPCON   (1u << 5)  // Connected UDP socket (fixed remote peer)
#define SOCK_PASSIVE  (1u << 6)  // Bind for inbound listener (server mode) 
// resolver
#define SOCK_ANY      (1u << 7)  // Use IPv4, IPv6 or V4 mapped
#define SOCK_IPV4     (1u << 8)  // IPv4 only
#define SOCK_IPV6     (1u << 9)  // IPv6 only
#define SOCK_NUMSERV  (1u << 10) // disable name resolution on port str

// helper for server
#define SOCK_LISTEN (SOCK_REUSE | SOCK_ANY | SOCK_NUMSERV | SOCK_PASSIVE)

// state structure
struct simple_sock {
    int fd;                   // managed file descriptor
    uint32_t mode;            // bitmask of SOCK_* flags
    struct sockaddr_in6 addr; // the socket adress
    struct rwbuf send_buf;    // send buffer
    struct rwbuf recv_buf;    // receive buffer
    size_t max_line;          // maximum line length
    size_t min_size;          // minium buffer space for I/O
    // active state flags
    unsigned int is_server    : 1; // 1 = simple_server, 0= simple_client
    unsigned int is_epoll     : 1; // 1 = registered
    unsigned int send_timeout : 1; // SO_SNDTIMEO set
    unsigned int recv_timeout : 1; // SO_RCVTIMEO set
    unsigned int recv_fin     : 1; // got read 0
    unsigned int send_fin     : 1; // shutdown writes when write buffer empty
    unsigned int fin_sent     : 1; // shutdown write sent
    unsigned int close_now    : 1; // ignore write buffer - close now
    unsigned int wait_write   : 1; // waiting for writeable event
    unsigned int sys_err      : 1; // read/write error
};

/* Init - Init socket state
 * ------------------------
 * - SOCK_INIT(fd, max_line, min_size, buf1, len1, buf2, len2)  - init state for file or pipe stream
 * - sock_init(sock, fd, addr, max_line, buf_size, min_size, max_size) -  init state for new or accepted fd
 * - sock_deinit(sock, can_log) : close fd, free buffers
 */
#define SOCK_INIT(_fd, _max_line,  _min_size, _buf1, _len1, _buf2, _len2) { \
    .fd = _fd, \
    .max_line = _max_line, \
    .min_size = _min_size, \
    .recv_buf = RWBUF_INIT(_buf1, _len1), \
    .send_buf = RWBUF_INIT(_buf2, _len2) \
}

int sock_init(struct simple_sock *sock,
    int fd, struct sockaddr_in6 *addr,
    size_t max_line, size_t buf_size,
    size_t min_size, size_t max_size);

// close sock, free memory
void sock_deinit(struct simple_sock *sock, int can_log);

/* Connection : Create or close client|server socket connections
 * -------------------------------------------------------------
 * - sock_client(sock, mode, hostname, port) : client connect to hostname and port 
 * - sock_server(sock, mode, hostname, port) : listen|bind on hostname and port
 * - sock_accept(sock, addr) : accept a pending fd from listener queue
 * - sock_sendfin : half close - send a TCP fin (if possible)
 * - sock_close   : close socket fd
 */
int sock_client(struct simple_sock *sock, uint32_t mode, const char *host, const char *port);
int sock_server(struct simple_sock *sock, uint32_t mode, const char *host, const char *port);
int sock_accept(struct simple_sock *sock, struct sockaddr_in6 *addr);
int sock_sendfin(struct simple_sock *sock);
int sock_close(struct simple_sock *sock, int can_log);

/* State : Change sock mode or fd state
 * ------------------------------------
 * sock_set_mode(sock, mode) : Update mode flags
 * sock_set_nonblk(sock)     : Enable non-blocking on fd
 * sock_set_sndto(sock, ms)  : Set SO_SNDTIMEO timeout in milliseconds
 * sock_set_rcvto(sock, ms)  : Set SO_RCVTIMEO timeout in milliseconds
 */
int sock_set_mode(struct simple_sock *sock, uint32_t mode);
int sock_set_nonblk(struct simple_sock *sock);
int sock_set_sndto(struct simple_sock *sock, uint32_t ms);
int sock_set_rcvto(struct simple_sock *sock, uint32_t ms);

/* FD I/O - Read|Write memory blocks to|from file descriptor
 * ------------------------------------------------------------
 * sock_send_data(sock, buf, len) : write memory buffer to fd
 * sock_recv_data(sock, bufd len) : read into memory buffer from fd 
 * sock_send_iovs(sock, niov, iovs) : scatter-gather writev to fd
 */
ssize_t sock_send_data(struct simple_sock *sock, void *buf, size_t len);
ssize_t sock_recv_data(struct simple_sock *sock, void *buf, size_t len);
ssize_t sock_send_iovs(struct simple_sock *sock, int niov, struct iovec iovs[static niov]);

/*
 * buffer I/O - send and recv buffers to|from fd
 * ---------------------------------------------
 * sock_send(sock)                : write from send-buffer to fd
 * sock_recv(sock)                : read to recv-buffer from fd
 * sock_write_mem(sock, buf, len) : append memory to send-buffer
 * sock_write_str(sock, str)      : append str to send-buffer
 * sock_send_mem(sock, buf, len)  : write send buffer + mem to fd, buffer remaining
 * sock_send_str(sock, str)       : write send buffer + str to fd, buffer remaining
 */
int sock_recv(struct simple_sock *sock);
int sock_send(struct simple_sock *sock);
int sock_write_mem(struct simple_sock *sock, void *buf, size_t len);
int sock_write_str(struct simple_sock *sock, struct str_slice str);
int sock_send_mem(struct simple_sock *sock, void *mem, size_t len);
int sock_send_str(struct simple_sock *sock, struct str_slice str);

/*
 * line I/O 
 * -------------------------------
 * sock_readline(sock, line, eof) : extract line from recv-buffer (return terminal fragment if eof)
 * sock_writeline(sock, line)     : write line + CRLF to send-buffer
 * sock_sendline(sock, line)      : write send-buffer + line + CRLF to fd, buffer remaining
 */
int sock_readline(struct simple_sock *sock, struct str_slice *line, int eof);
int sock_writeline(struct simple_sock *sock, struct str_slice line);
int sock_sendline(struct simple_sock *sock, struct str_slice line);

/*
 * Status and info
 * ---------------
 * sockaddr_tostr(addr, addr_len) : format addr to address:port str
 * sock_tostr(sock) : format sock to address:port str
 * -
 * sock_sendbuf_used(sock) : return pending bytes in send-buffer
 * sock_recvbuf_used(sock) : return available bytes in recv-buffer
 * sock_write_close(sock, force) : mark socket closed for writes
 * sock_set_err(sock) : mark socket as failed
 * sock_iseof(sock)   : true if peer closed stream (read 0 from fd)
 * sock_dataeof(sock, rc)   : check if both data and eof received
 * sock_read_done(sock)     : true if remote peer is closed and recv-buffer drained
 * sock_write_done(sock)    : true if write-closed and send-buffer drained
 * sock_write_closing(sock) : true if write-closed is set
 * sock_isbusy(sock)    : true if send-buffer is non-empty or pending FIN
 * sock_isclosed(sock)  : true if both local and remote streams are closed
 * sock_mustclose(sock) : true if error, force close, or fully closed
 * sock_canclose(sock)  : true if must-close or the send-buffer is fully drained
 * sock_isactive(sock)  : true if fd is open and state is not must-close
 */
char *sockaddr_tostr(struct sockaddr *addr, socklen_t addr_len);
char *sock_tostr(struct simple_sock *sock);

static inline size_t sock_sendbuf_used(struct simple_sock *sock)
{
    return rwbuf_used(&sock->send_buf);
}

static inline size_t sock_recvbuf_used(struct simple_sock *sock)
{
    return rwbuf_used(&sock->recv_buf);
}

static inline void sock_write_close(struct simple_sock *sock, int force)
{
    sock->send_fin = 1; 
    if (force) sock->close_now = 1;
}

static inline void sock_set_err(struct simple_sock *sock)
{
    sock->sys_err = 1;
}

static inline int sock_iseof(struct simple_sock *sock)
{
    return sock->recv_fin;
}

static inline int sock_dataeof(struct simple_sock *sock, int rc)
{
    return rc == SOCK_DATA && sock_iseof(sock) ? SOCK_CLOSED : rc;
}

static inline int sock_read_done(struct simple_sock *sock)
{
    return sock->recv_fin && sock_recvbuf_used(sock) == 0;
}

static inline int sock_write_done(struct simple_sock *sock)
{
    return sock->send_fin && sock_sendbuf_used(sock) == 0;
}

static inline int sock_write_closing(struct simple_sock *sock)
{
    return sock->send_fin;
}

static inline int sock_isbusy(struct simple_sock *sock)
{
    return rwbuf_used(&sock->send_buf) > 0 || (sock->send_fin && !sock->fin_sent);
}

static inline int sock_isclosed(struct simple_sock *sock)
{
    return sock->recv_fin && sock->fin_sent;
}

static inline int sock_mustclose(struct simple_sock *sock)
{
    return sock->sys_err || sock->close_now || sock_isclosed(sock);
}

static inline int sock_canclose(struct simple_sock *sock)
{
    return sock_mustclose(sock) || (sock->send_fin && rwbuf_used(&sock->send_buf) == 0);
}

static inline int sock_isactive(struct simple_sock *sock)
{
    return sock->fd >= 0 && !sock_mustclose(sock);
}

#endif
