#ifndef _CONFIG_H_
#define _CONFIG_H_

// common config
#define TCP_PORT_STR "6379"

#define MAX_LINE 256

// socket limits
#define SOCK_INIT_BUFSIZE 4096
#define SOCK_MIN_BUFSIZE (MAX_LINE * 2)
#define SOCK_MAX_BUFSIZE 8192

#endif
