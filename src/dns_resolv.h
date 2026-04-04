/*
 * DNS-RESOLV - a DNS resolver API
 * -------------------------------
 *
 * API sections
 * ------------
 */
#ifndef _DNS_RESOLV_H_
#define _DNS_RESOLV_H_

// need sockaddr
#include <netinet/in.h>

#define DNS_RESOLV "resolv"
#define DNS_SERVICES "/etc/services"
#define DNS_RESOLV_CONF "/etc/resolv.conf"

#define DNS_MAX_ADDRS 16 
#define DNS_MAXNS 3
#define DNS_PORT 53
#define DNS_PKTSIZE 1280
#define DNS_TIMEOUT_MS 5000
#define DNS_ATTEMPTS 2

// resolv flags
#define DNS_TCP      (1 << 0)
#define DNS_UDP      (1 << 1)
#define DNS_IPV4     (1 << 2)
#define DNS_IPV6     (1 << 3)
#define DNS_PASSIVE  (1 << 4)
#define DNS_NUMPORT  (1 << 5)
#define DNS_V4MAPPED (1 << 6)
#define DNS_ALL      (1 << 7)

// error codes
#define DNS_ERR     -1
#define DNS_INTR    -2
#define DNS_TIMEOUT -3
#define DNS_EAGAIN  -4
#define DNS_CLOSED  -5

// address
struct dns_sockaddr {
    int sock_type;
    socklen_t len;
    union {
        struct sockaddr     sa; // 16 bytes (base)
        struct sockaddr_in  v4; // 16 bytes
        struct sockaddr_in6 v6; // 28 bytes
    };
};

// resolve hostname,port to array of resolv_addr - returns num-addr or error
int dns_resolv(uint32_t flags, 
    const char *hostname, const char *port,
    int max_addr, struct dns_sockaddr addrs[max_addr]);

#endif
