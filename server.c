#include <stdio.h>
#include <stdlib.h> 
#include <string.h> 
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#define TCP_PORT 6379

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

int handle_client(int fd)
{
    char buf[MAX_LINE];
    char *pos = buf;
    int rem = sizeof(buf);

    while (1) {
        ssize_t nr = read(fd, pos, rem);
        if (nr <= 0) {
            close(fd);
            return -1;
        }
        pos += nr;
        rem -= nr;
        char *eol = memchr(buf, '\n', pos - buf);
        if (!eol) {
            if (rem <= 0) {
                perror("readline too big");
                close(fd);
                return -1;
            }
        }
        else {
            *eol = '\0';
            process_line(buf);
            // deal with partial line
            eol++;
            int len = pos - eol;
            memmove(buf, eol, len);
            pos = buf + len;
            rem = sizeof(buf) - len;
        }
    }

    return fd;
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

