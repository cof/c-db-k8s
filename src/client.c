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

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "config.h"
#include "util.h"
#include "log.h"
#include "sock.h"

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

    // resolve hostname+ port string to list of (ip+port)
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(hostname, port, &hints, &res);
    if (rc != 0) {
        fatal_error("getaddrinfo(%s,%s) : %s\n", hostname, port, gai_strerror(rc));
    }

    // try to connect 
    int sock_fd = -1;
    const char *addr_str = NULL;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock_fd == -1) continue;
        addr_str = sockaddr_tostr(ai->ai_addr, ai->ai_addrlen);
        rc = connect(sock_fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != -1) break;
        log_errno("connect(%s) failed", addr_str);
        close(sock_fd);
        sock_fd = -1;
    }
    freeaddrinfo(res);
    if (sock_fd == -1) {
        fatal_error("No connection");
    }

    // wrap the fd into FILE
    FILE *server = fdopen(sock_fd, "r+");
    if (!server) fatal_error("Failed to create in/out");

    // at this stage safe to proceed
    log_info("+", "Connectivity test: OK");
    
    // loop until user hits ctrl-d or server closes
    // TODO replace this with simple_sock
    char buf[4096];
    while (1) {
        // prompt
        fprintf(stdout, "> "); 
        fflush(stdout);


        // read request-line 
        char *line = fgets(buf, sizeof(buf), stdin);
        if (!line) {
            log_info("+", "stdin closed");
            break;
        }

        line[strcspn(line, "\r\n")] = '\0';
        if (log) log_info("+", "send req: %s", line);

        // send request to server
        fprintf(server, "%s\n", line);
        //fflush(server);

        // recv response from server
        line = fgets(buf, sizeof(buf), server);
        if (!line) {
            // error or close
            log_info("+", "Connection closed by server");
            break;
        }

        line[strcspn(line, "\r\n")] = '\0';
        if (log) log_info("+", "recv rsp: %s", line);
        fprintf(stdout, "%s\n", line);
        //fflush(stdout);

        // check for server eof
        char ch;
        int rc = recv(fileno(server), &ch, 1, MSG_PEEK | MSG_DONTWAIT);
        if (rc == 0) {
            log_info("+", "Connection closed by server");
            break;
        }
        if (rc < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            log_errno("connecton(%s) failed", addr_str);
            fatal_error("Lost connection");
        }
    }

    fclose(server);

    return 0;
}
