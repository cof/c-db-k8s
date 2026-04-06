/*
 * DNS-RESOLV - a DNS resolver API
 * -------------------------------
 *
 * API sections
 * ------------
 * dns_resolv(flags, hostname, port, max_addr, addrs) : resolve hostname/port to array of addr
 * dns_sockaddr_tostr(addr) : convert addr to text form
 */
#ifndef _DNS_RESOLV_H_
#define _DNS_RESOLV_H_

// need sockaddr
#include <netinet/in.h>

#define DNS_RESOLV "resolv"
#define DNS_SERVICES "/etc/services"
#define DNS_RESOLV_CONF "/etc/resolv.conf"

#define DNS_MAXADDR 16  // default result
#define DNS_MAXNAME 256
#define DNS_MAXSRCH 8
#define DNS_MAXNS 3
#define DNS_PORT 53
#define DNS_PKTSIZE 1280
#define DNS_ATTEMPTS 2
#define DNS_TIMEOUT_SECS 5

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
#define DNS_OK        0
#define DNS_ERR      -1
#define DNS_EINTR    -2
#define DNS_ETIMEOUT -3
#define DNS_EAGAIN   -4
#define DNS_ECLOSED  -5
#define DNS_NODATA   -6
#define DNS_FORMERR  -7
#define DNS_SERVFAIL -8
#define DNS_NXDOMAIN -9
#define DNS_NOTIMP   -10
#define DNS_REFUSED  -11

// address
struct dns_sockaddr {
    int sock_type; // e.g SOCK_STREAM, SOCK_DGRAM
    socklen_t len; // size of encoded addr 
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

char *dns_sockaddr_tostr(struct dns_sockaddr *addr);

#endif
