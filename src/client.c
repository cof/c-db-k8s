/*
 * client - a telnet like TCP client
 *
 * Usage: client hostname[:port]
 *
 *  hostname - hostname to connect to
 *  port - port number (default 6379)
 *
 * e.g.
 *  $ client 127.0.0.1
 *
 * Refs:
 * - man 3 getaddrinfo - getaddrinfo/connect
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "config.h"
#include "util.h"
#include "log.h"

// big enough for "[" host "]" :" port + null
#define MAX_HOSTPORT (4 + NI_MAXHOST + NI_MAXSERV)
static int mysockaddr_tostr(struct sockaddr *addr, socklen_t addr_len, char *buf, int len)
{
    // convert address/port to string
    char host[NI_MAXHOST];
    char port[NI_MAXSERV];

    int rc = getnameinfo(addr, addr_len, 
        host, sizeof(host),
        port, sizeof(port),
        NI_NUMERICHOST | NI_NUMERICSERV
    );

    if (rc != 0) {
        fprintf(stderr, "getnameinfo failed - %s", gai_strerror(rc));
        return -1;
    }

    // finaly load addr:port string
    char *dst = buf;
    if (addr->sa_family == AF_INET6) *dst++ = '[';
    dst = mempcpy(dst, host, strlen(host));
    if (addr->sa_family == AF_INET6) *dst++ = ']';
    *dst++ = ':';
    dst = mempcpy(dst, port, strlen(port));
    *dst = '\0';

    return dst - buf;
}

int main(int argc, char *argv[])
{
    // parse cmd line
    if (argc < 2) {
        fatal_error("Missing hostname");
    }

    char *hostname = strdup(argv[1]);
    char *port = strrchr(hostname, ':');
    if (port) *port++ = '\0';
    if (strlen(hostname) == 0) {
        fatal_error("hostname cannot be blank");
    }
    if (!port || strlen(port) == 0) port = TCP_PORT_STR;

    // resolve hostname+ port string to list of (ip+port)
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(hostname, port, &hints, &res);
    if (rc != 0) {
        fatal_error("getaddrinfo(%s,%s) : %s\n", hostname, port, gai_strerror(rc));
    }

    // try to connect 
    int sock_fd = -1;
    char name[MAX_HOSTPORT];

    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock_fd == -1) continue;
        mysockaddr_tostr(ai->ai_addr, ai->ai_addrlen, name, sizeof(name));
        rc = connect(sock_fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != -1) break;
        log_errno("connect(%s) failed", name);
        close(sock_fd);
        sock_fd = -1;
    }
    freeaddrinfo(res);
    if (sock_fd == -1) {
        fatal_error("No connection");
    }

    // wrap the fd into FILE
    FILE *server = fdopen(sock_fd, "r+");
    if (!server) {
        fatal_error("Failed to create in/out");
    }

    // at this stage safe to proceed
    log_info("Connectivity test: OK");
    
    // loop until user hits ctrl-d or server closes
    char buf[4096];
    while (1) {
        // prompt
        fprintf(stdout, "> "); 
        fflush(stdout);

        // read line 
        if (fgets(buf, sizeof(buf), stdin) == NULL)  {
            //  error or eof
            break;
        }

        // send line to server
        fputs(buf, server); 
        fflush(server);

        // recv response from server
        if (fgets(buf, sizeof(buf), server) == NULL)  {
            // error or close
            log_info("Connection closed by foreign host.");
            break;
        }
        fprintf(stdout, "%s", buf);
        fflush(stdout);

        // check for server eof
        char ch;
        int rc = recv(fileno(server), &ch, 1, MSG_PEEK | MSG_DONTWAIT);
        if (rc == 0) {
            log_info("Connection closed by foreign host.");
            break;
        }
        if (rc < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            log_errno("connecton(%s) failed", name);
            fatal_error("Lost connection");
        }
    }

    fclose(server);

    return 0;
}
