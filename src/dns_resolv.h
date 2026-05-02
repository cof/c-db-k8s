/*
 * DNS-RESOLV - a DNS subsystem
 * ----------------------------
 * This is full DNS subsystem designed as a getaddrinfo() replacement.
 * Features include non-blocking I/O, parallel queries and static memory use.
 *
 * The aim is for a simple/fast/static memory implementation that does not
 * require a require massive shared library code base that can be easily
 * integrated into any application socket I/O frameworks.
 *
 * General idea is you pass the resolver an array of dns_sockaddr and a
 * bit-wise mask of flag values that the resolver will use to fill out the
 * array with IP address and port number structures values that can be passed
 * directly to connect() or bind().
 *
 * The dns_sockaddr strutcure is defined as:
 *
 *   struct dns_sockaddr {
 *       int sock_type; // e.g SOCK_STREAM, SOCK_DGRAM
 *       socklen_t len; // size of encoded addr
 *       union {
 *           struct sockaddr     sa; // 16 bytes (base)
 *           struct sockaddr_in  v4; // 16 bytes
 *           struct sockaddr_in6 v6; // 28 bytes
 *       };
 *   };
 *
 * Flags is bit-wise mask of the following values:
 *
 *  DNS_TCP      : resolve port for SOCK_STREAM type
 *  DNS_UDP      : resolve port for SOCK_DGRAM type
 *  DNS_IPV4     : resolve hostname to AF_INET (IPv4) address
 *  DNS_IPV6     : resolve hostname to AF_INET6 (IPv6) address
 *  DNS_PASSIVE  : set IP address that can be used with bind()
 *  DNS_NUMPORT  : numeric port; do not resolve via /etc/services
 *  DNS_V4MAPPED : if DNS_IPV6 is set map all IPv4 address to IPv6
 *  DNS_ALL      : if DNS_IPV6|DNS_VMAPPED set keep IPv4 mapped else discard
 *
 * Example:
 *
 *  dns_init(0, 0, NULL);
 *  struct dns_sockaddr addrs[16];
 *  int flags = DNS_IPV4|DNS_TCP;
 *
 *  int nr = dns_resolv(flags, "example.com", "80", addr, 16);
 *  for (int i = 0; i < nr; i++) {
 *      struct dns_sockaddr *addr = &addrs[i];
 *      fd = socket(addr->sa.sa_family, addr->sock_type, 0);
 *      rc = connect(fd, &addr->sa, addr->len);
 *      if (rc != -1) break;
 *      close(fd);
 *  }
 *
 * API
 * ---------
 * dns_init(hosts_size, cache_size, sig) : load services/hosts/resolv.conf
 * dns_resolv(flags, hostname, port, max_addr, addrs) : resolve hostname/port to array of addr
 * dns_sockaddr_tostr(addr) : convert sock-addr to text form
 * dns_socktype_tostr(addr) : convert sock-type to text form
 */
#ifndef _DNS_RESOLV_H_
#define _DNS_RESOLV_H_

// need sockaddr
#include <netinet/in.h>

#define DNS_RESOLV "resolv"
#define DNS_SERVICES "/etc/services"
#define DNS_RESOLV_CONF "/etc/resolv.conf"
#define DNS_HOSTS "/etc/hosts"

#define DNS_MAXADDR 16  // default result

// uses 31-bit serial number (rfc1982) window good for 68 years
#define DNS_TIME_GEQ(a,b) ((int32_t)((a)-(b)) >= 0)

#define DNS_PORT 53
#define DNS_PKTSIZE ALIGN_UP(1232, 16)
#define DNS_ATTEMPTS 2
#define DNS_TIMEOUT_SECS 5

// resolv.conf limits
#define DNS_CFG_MAXNS    3
#define DNS_CFG_MAXSRCH  8
#define DNS_CFG_MAXSTORE 256

// /etc/hosts limits
#define DNS_HOSTS_MAXNAME 256
#define DNS_HOSTS_MAXADDR 128
#define DNS_HOSTS_MAXSTORE BUFSIZ

// /etc/service limits
#define DNS_SVC_MAXNAME 32
#define DNS_SVC_MAXPORT 256
#define DNS_SVC_MAXSTORE BUFSIZ

// resolver flags
#define DNS_TCP      (1 << 0)  // resolve port for SOCK_STREAM type
#define DNS_UDP      (1 << 1)  // resolve port for SOCK_DGRAM type
#define DNS_IPV4     (1 << 2)  // resolve hostname to AF_INET (IPv4) address
#define DNS_IPV6     (1 << 3)  // resolve hostname to AF_INET6 (IPv6) address
#define DNS_PASSIVE  (1 << 4)  // set IP address that can be used with bind()
#define DNS_NUMPORT  (1 << 5)  // numeric port; do not resolve via /etc/services
#define DNS_V4MAPPED (1 << 6)  // if DNS_IPV6 is set map all IPv4 address to IPv6
#define DNS_ALL      (1 << 7)  // if DNS_IPV6|DNS_VMAPPED set keep IPv4 mapped else discard

// error codes
#define DNS_OK        0
#define DNS_ERR      -1
#define DNS_EBADHDR  -2
#define DNS_ENOTRSP  -3
#define DNS_ETID     -4
#define DNS_EBADMSG  -5
#define DNS_EQUEST   -6
#define DNS_ETRUNC   -7
#define DNS_EPROTO   -8
#define DNS_EINTR    -9
#define DNS_ETIMEOUT -10
#define DNS_EAGAIN   -11
#define DNS_NODATA   -12
#define DNS_FORMERR  -13
#define DNS_SERVFAIL -14
#define DNS_NXDOMAIN -15
#define DNS_NOTIMP   -16
#define DNS_REFUSED  -17

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

int dns_init(uint32_t hosts_size, uint32_t cache_size, struct simple_sig *sig);
int dns_resolv(uint32_t flags,
    const char *hostname, const char *port,
    int max_addr, struct dns_sockaddr addrs[max_addr]);
char *dns_sockaddr_tostr(struct dns_sockaddr *addr);
char *dns_socktype_tostr(struct dns_sockaddr *addr);

#endif
