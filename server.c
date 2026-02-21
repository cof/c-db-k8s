#include <stdio.h>
#include <stdlib.h> 
#include <string.h> 
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>

#define TCP_PORT 6379
#define ERR_READSOCK -1
#define ERR_CLOSESOCK -2
#define ERR_READLINE -3


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

    return fd;
}

int accept_client(int sfd)
{
    struct sockaddr_storage caddr;
    socklen_t clen = sizeof(caddr);

    int cfd = accept(sfd, (struct sockaddr *) &caddr, &clen);

    return cfd;
}

#define MAX_LINE 256

void process_line(const char *line)
{
    puts(line);
}


struct strvec {
    char *base;
    size_t len;
};

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

int read_line(struct rwbuf *buf, struct strvec *line)
{
    char *eol = memchr(buf->rptr, '\n', buf->len);
    if (eol) {
        // found line
        size_t len = eol - buf->rptr + 1;
        char *str = buf->rptr;
        // remove from buffer
        buf->len -= len ;
        buf->rptr += len;
        // chop cr/lf
        if (len && str[len - 1] == '\n') len--;
        if (len && str[len - 1] == '\r') len--;
        str[len] = '\0';
        // store line
        line->base = str;
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

    return 0;
}


void handle_client(int fd)
{
    struct rwbuf buf;
    struct strvec line;
    int rc;

    init_rwbuf(&buf);

    while (1) {
        rc = read_sock(fd, &buf);
        if (rc < 0) break;
        while ((rc = read_line(&buf, &line)) > 0) {
            process_line(line.base);
        }
        if (rc < 0) break;
    }

    close(fd);
}

int main(int argc, char *argv[])
{
    int sfd = setup_listener();

    while (1) {
        int fd = accept_client(sfd);
        handle_client(fd);
    }

    return 0;
}

