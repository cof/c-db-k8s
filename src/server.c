#include <stdio.h>
#include <stdlib.h> 
#include <stdarg.h>
#include <string.h> 
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <unistd.h>
#include <errno.h>

#include "util.h"
#include "db.h"


#define MAX_LINE 256
#define TCP_PORT 6379
#define ERR_READSOCK -1
#define ERR_WRITESOCK -2
#define ERR_CLOSESOCK -3
#define ERR_READLINE -4
#define ERR_BUFSIZE -5
#define ERR_SOCKNAME -6
#define ERR_QUIT -7

#define MAX_HOSTPORT (3 + NI_MAXHOST + NI_MAXSERV)

int get_addr(int sockfd, char *dst, int dst_len)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    int rc;

    rc = getsockname(sockfd, (struct sockaddr *)&addr, &len);
    if (rc != 0) return ERR_SOCKNAME;

    char host[NI_MAXHOST];
    char port[NI_MAXSERV];

    rc = getnameinfo((struct sockaddr *) &addr, len, 
        host, sizeof(host),
        port, sizeof(port),
        NI_NUMERICHOST | NI_NUMERICSERV
    );

    if (rc != 0) return ERR_SOCKNAME;

    if (addr.ss_family == AF_INET6) {
        rc = snprintf(dst, dst_len, "[%s]:%s", host, port);
    }
    else {
        rc = snprintf(dst, dst_len, "%s:%s", host, port);
    }

    return rc > 0 ? 0 : ERR_SOCKNAME;
}


void log_info(const char *fmt, ...)
{
    va_list args;  

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fprintf(stdout, "\n");
}

int log_err(int ec, const char *fmt, ...)
{
    va_list args;  

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");

    return ec;
}


int setup_listener(void)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0); 
    if (fd == -1) {
        perror("create listen socket");
        return -1;
    }

    int opt = 0;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)) == -1) {
        perror("setsocktopt IPPROTO_IPV6");
        return -1;
    }

    opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsocktopt SO_REUSEADDR");
        return -1;
    }

    struct sockaddr_in6 addr = { 0 };
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(TCP_PORT);

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        perror("bind addr");
        return -1;
    }

    if (listen(fd, SOMAXCONN) == -1) {
        perror("listn addr");
        return -1;
    }

    char tmp[MAX_HOSTPORT];
    if (get_addr(fd, tmp, sizeof(tmp)) != 0) {
        perror("get_addr failed");
        return -1;
    }

    log_info("Database listening on %s", tmp);

    return fd;
}

int accept_client(int sfd)
{
    struct sockaddr_storage caddr;
    socklen_t clen = sizeof(caddr);

    int cfd = accept(sfd, (struct sockaddr *) &caddr, &clen);

    return cfd;
}



struct rwbuf {
    size_t cap; // fixed size 
    size_t len; // bytes avail to read
    char *rptr;  // start of bytes to read
    char data[4096];
};

void init_rwbuf(struct rwbuf *buf)
{
    buf->cap = sizeof(buf->data);
    buf->len = 0;
    buf->rptr = buf->data;
}

int read_sock(int fd, struct rwbuf *buf)
{
    int tr = 0;
    char *rptr = buf->rptr + buf->len;
    size_t rem = buf->data + buf->cap - rptr;

    while (1) {
        
        // read as much as we can
        ssize_t nr = read(fd, rptr, rem);

        if (nr == -1)  {
            // read failed
           if (errno == EINTR) continue; // interrupt ?
           if (errno != EAGAIN) tr = ERR_READSOCK;
           break;
        }
        if (nr == 0) {
            // closed ?
            if (tr == 0) tr = ERR_CLOSESOCK;
            break;
        }

        tr += nr;

        rptr += nr;
        rem -= nr;
        buf->len += nr;

        // socket has less 
        break;
    }

    // total read or errcode
    return tr;
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
        return len > MAX_LINE ? ERR_READLINE : (int) len;
    }

    // incomplete line
    if (buf->len >= MAX_LINE) return ERR_READLINE;

    if (buf->rptr > buf->data) {
        // ensure partial line at buffer start
        memmove(buf->data, buf->rptr, buf->len);
        buf->rptr = buf->data;
    }

    // wait for eol
    return 0;
}


static int send_str(int fd, const char *str, size_t len)
{
    char buf[100];
    if (len > sizeof(buf) -2) return ERR_BUFSIZE;

    char *dst = buf;
    dst += sprintf(dst, "%.*s", (int) len, str);
    *dst++ = '\r';
    *dst++ = '\n';
    len = dst - buf;

    // TODO buffer up writes in case full
    size_t rc = write(fd, buf, len);
    return rc == -1  ? ERR_WRITESOCK : (int) rc;
}

static int send_slice(int fd, struct str_slice str)
{
    return send_str(fd, str.ptr, str.len);
}

static int cmd_set(int fd, struct str_slice args)
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

    return send_slice(fd, res);
}

static int cmd_get(int fd, struct str_slice key)
{
    struct str_slice res = db_get(key);

    if (!res.ptr)
        res = make_slice(STR_LIT("FAIL"));

    return send_slice(fd, res);
}

static int cmd_del(int fd, struct str_slice key)
{
    struct str_slice res;

    if (!key.len)
        res = make_slice(STR_LIT("FAIL"));
    else if (!db_del(key))
        res = make_slice(STR_LIT("FAIL"));
    else 
        res = make_slice(STR_LIT("OK"));

    return send_slice(fd, res);
}

static int cmd_quit(int fd, struct str_slice args)
{
    send_str(fd, STR_LIT("OK"));
    return ERR_QUIT;
}

static int cmd_unsupp(int fd, struct str_slice args)
{
    return send_str(fd, STR_LIT("UNSUPP"));
}

static struct {
    const char *cmd;
    size_t len;
    int (*func)(int fd, struct str_slice args);
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

int process_line(int fd, char *cmd_str, int len)
{
    char *args = memchr(cmd_str, ' ', len);
    int cmd_len = args ? args - cmd_str : len;
    struct str_slice cmd_args = ltrim(make_slice(args, len - cmd_len));

    str2upper(cmd_str, cmd_len);
    int cmd_idx = find_cmd(cmd_str, cmd_len);
    int rc;

    if (cmd_idx != -1) {
        rc = cmds[cmd_idx].func(fd, cmd_args);
    }
    else {
        rc = cmd_unsupp(fd, cmd_args); 
    }

    return rc;
}


void handle_client(int fd)
{
    struct rwbuf buf;
    struct str_slice line;
    int rc;

    init_rwbuf(&buf);

    while (1) {
        rc = read_sock(fd, &buf);
        if (rc < 0) break;
        while ((rc = read_line(&buf, &line)) > 0) {
            rc = process_line(fd, line.ptr, line.len);
            if (rc != 0) break;
        }
        if (rc < 0) break;
    }

    close(fd);
}

int main(int argc, char *argv[])
{
    if (!db_init()) return log_err(-1, "db_init failed");
    int sfd = setup_listener();
    if (sfd < 0) return log_err(-1, "setup_listener failed");

    while (1) {
        int fd = accept_client(sfd);
        handle_client(fd);
    }

    return 0;
}

