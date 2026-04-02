/*
 * RESOLV - a simple resolver API
 * ----------------------------------
 * See resolv.h for API description.
 *
 * API sections
 * ------------
 */
#ifndef _RESOLV_H_
#define _RESOLV_H_

#define RESOLV "resolv"
#define RESOLV_SERVICES "/etc/services"
#define RESOLV_CONF "/etc/resolv.conf"
#define RESOLV_MAXNS 3
#define RESOLV_PORT 53

int resolv_addr(const char *hostname, const char *port, uint32_t mode, 
    int max_addr, struct sock_addr addrs[max_addr]);

#endif
