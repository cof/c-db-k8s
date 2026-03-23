#ifndef __CONFIG_H__
#define __CONFIG_H__

// common config
#define TCP_PORT_STR "6379"

// socket limits
#define MAX_LINE 256
#define INIT_BUF_SIZE 4096
#define MIN_BUF_SIZE (MAX_LINE * 2)
#define MAX_BUF_SIZE 8192

#endif
