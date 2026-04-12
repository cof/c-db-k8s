/*
 * DNS-RESOLV - a DNS resolver API
 * -------------------------------
 * See dns_resolv.h for API description.
 *
 * TODO
 * - Happy Eyeballs 
 * - EDNS0 
 * - CNAME
 *
 * API sections
 * ------------
 * dns_resolv - resolve hostname/port to array of add 
 *
 * Refs
 * -----
 * rfc1035 - DOMAIN NAMES - IMPLEMENTATION AND SPECIFICATION
 * rfc6981 - Extension Mechanisms for DNS (EDNS(0))
 * rfc7766 - DNS over TCP 
 *
 */
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>

#include "util.h"
#include "log.h"
#include "rwbuf.h"
#include "hashmap.h"
#include "dns_proto.h"
#include "dns_resolv.h"

// helper macros for log_debug
#define ADDR_STR(a) dns_sockaddr_tostr(a)
#define TYPE_STR(t) (t) == DNS_TCP ? "TCP" : "UDP"
#define NEED(r) (r)->need_ip4, (r)->need_ip6
#define CFG(c) (c)->attempts, (c)->timeout_secs, (c)->num_ns
#define CFG_NS(c) (c)->attempts, (c)->timeout_secs, (c)->ndots, (c)->num_ns
#define CFG_QN(c) (c)->attempts, (c)->timeout_secs, (c)->num_ns

// cached dns answers
struct dns_cache {
    size_t store_len;
    size_t num_addr;
    size_t max_addr;
    hashmap64s name_toaddr;
    map64s_entry buffer[DNS_HOSTS_MAXADDR * 4 / 3];
    char store[DNS_HOSTS_MAXSTORE];
    struct dns_sockaddr addrs[DNS_HOSTS_MAXADDR];
};

// hosts file
struct dns_hosts {
    size_t store_len;
    size_t num_addr;
    size_t max_addr;
    hashmap64s name_toaddr;
    map64s_entry buffer[DNS_HOSTS_MAXADDR * 4 / 3];
    char store[DNS_HOSTS_MAXSTORE];
    struct dns_sockaddr addrs[DNS_HOSTS_MAXADDR];
};

// resolv.conf file
struct dns_config {
    uint32_t attempts;
    uint32_t timeout_secs;
    uint32_t ndots;
    size_t num_search;
    size_t num_ns;
    size_t store_len;
    // flags
    unsigned int use_tcp : 1;
    // buffers
    char store[DNS_CFG_MAXSTORE];
    char *search[DNS_CFG_MAXSRCH];
    struct dns_sockaddr ns_addrs[DNS_CFG_MAXNS];
};

// services file
struct dns_services {
    size_t store_len;
    size_t num_svc;
    size_t max_svc;
    hashmap32s name_toport;
    map32s_entry buffer[DNS_SVC_MAXPORT * 4 / 3];
    char store[DNS_SVC_MAXSTORE];
};

// result state
struct dns_result {
    uint32_t flags;
    struct str_slice host;
    struct str_slice port;
    int udp_port;
    int tcp_port;
    int num_ip4;
    int num_ip6;
    // array of results
    int num_addr;
    int max_addr;
    struct dns_sockaddr *addrs;
    // bit fields
    unsigned int need_ip4 : 1;
    unsigned int need_ip6 : 1;
};

// conection state
enum dns_state {
    DNS_IDLE = 0,
    DNS_SEND,
    DNS_RECV,
    DNS_DONE
};

struct dns_sock {
    int fd;
    size_t tx_off;
    size_t tx_len;
    size_t rx_off;
    size_t rx_len;
    uint8_t tx_buf[DNS_PKTSIZE];
    uint8_t rx_buf[DNS_PKTSIZE];
    unsigned int is_open     : 1;
    unsigned int is_tcp      : 1;
    unsigned int in_progress : 1;
    unsigned int is_connect  : 1;
    unsigned int has_error   : 1;
};

struct dns_query {
    struct str_slice qname;
    uint16_t qtype;
    uint16_t qclass;
    uint16_t tid;
    int last_rc;
    int state;
};

// nameserver state
struct dns_ns {
    struct str_slice name;
    struct dns_sockaddr *addr;
    struct dns_result *res;
    struct dns_sock  sock;
    uint32_t timeout_ms;
    int active;
    struct dns_query v4_query;
    struct dns_query v6_query;
    struct dns_msg msg;
    unsigned int use_tcp   : 1;
    unsigned int use_esdn  : 1;
    unsigned int have_ans  : 1;
    unsigned int have_ip4  : 1;
    unsigned int have_ip6  : 1;
};

// resolver config
static struct dns_config  glob_cfg;
static struct dns_hosts   glob_hosts;
static struct dns_services glob_svcs;
static struct dns_cache   glob_cache;
static struct simple_sig  *glob_sig;

const uint8_t ip4_any[4] = { 0 };
const uint8_t ip4_loopback[4] = { 127, 0, 0, 1 };
const uint8_t ip6_any[16] = { 0 };
const uint8_t ip6_loopback[16] = { [15] = 1 };

// generate a tid
static uint16_t gen_tid()
{
    static uint16_t next_tid;
    static int init_done = 0;

    if (!init_done) {
        next_tid = (uint16_t) getpid() ^ (uint16_t) time(NULL);
        init_done = 1;
    }

    return next_tid++;
}

static int64_t get_now_ms(void) 
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        // failed - return ts that will trigger a timout
        return  0x7FFFFFFFFFFFFFFFLL;
    }

    return ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
}

// decode ip-addr str
static uint32_t ipstr_decode(struct str_slice str, uint8_t dst[static 16])
{
    if (ip4_str_decode(str.ptr, str.len, dst)) return DNS_IPV4;
    if (ip6_str_decode(str.ptr, str.len, dst)) return DNS_IPV6;
    return 0;
}

// load addr with ip + port
static int sockaddr_load(struct dns_sockaddr *addr, 
    int type, const void *ip, 
    int ptype, uint16_t port)
{
    if (!addr) return 0;

    // socket type
    int sock_type;
    switch(ptype) {
    case DNS_TCP: sock_type = SOCK_STREAM; break;
    case DNS_UDP: sock_type = SOCK_DGRAM; break;
    default: sock_type = 0;
    }

    memset(addr, 0, sizeof(*addr));

    // socket address
    switch(type) {
    case DNS_IPV4: 
        addr->sock_type = sock_type;
        addr->len = sizeof(addr->v4);
        addr->v4.sin_family = AF_INET;
        addr->v4.sin_port   = __builtin_bswap16(port);
        memcpy(&addr->v4.sin_addr.s_addr, ip, 4);
        return 1;
    case DNS_IPV6:
        addr->sock_type = sock_type;
        addr->len = sizeof(addr->v6);
        addr->v6.sin6_family = AF_INET6;
        addr->v6.sin6_port   = __builtin_bswap16(port);
        memcpy(&addr->v6.sin6_addr, ip, 16);
        return 1;
    }

    return 0;
}

static inline int res_isfull(struct dns_result *res)
{
    return res->num_addr >= res->max_addr;
}

// return index where stored else -1
static int res_add_ip_port(struct dns_result *res, 
    int type, const void *ip, 
    int ptype, uint16_t port)
{
    if (res_isfull(res)) return 0;

    // get slot
    int idx;
    switch(type) {
    case DNS_IPV4: idx = res->max_addr - res->num_ip4 - 1; break;
    case DNS_IPV6: idx = res->num_ip6; break;
    default: return -1;
    }

    if (sockaddr_load(&res->addrs[idx], type, ip, ptype, port)) {
        switch(type) {
        case DNS_IPV4: res->num_ip4++; break;
        case DNS_IPV6: res->num_ip6++; break;
        }
        res->num_addr++;
        return idx;
    }

    return -1;
}

static int res_add_ip(struct dns_result *res, int type, const void *ip)
{
    int add_tcp = res->flags & DNS_TCP;
    int add_udp = res->flags & DNS_UDP;
    int rc, idx = -1;

    if (add_tcp) {
        rc = res_add_ip_port(res, type, ip, DNS_TCP, res->tcp_port);
        if (rc != -1) idx = rc;
    }
    if (add_udp) {
        rc = res_add_ip_port(res, type, ip, DNS_UDP, res->udp_port);
        if (rc != -1) idx = rc;
    }

    return idx;
}

static int res_add_addr(struct dns_result *res, struct dns_sockaddr *addr)
{
    switch (addr->sa.sa_family) {
    case AF_INET:
        if (addr->len != sizeof(addr->v4)) break;
        return res_add_ip(res, DNS_IPV4, &addr->v4.sin_addr.s_addr);
    case AF_INET6: 
        if (addr->len != sizeof(addr->v6)) break;
        return res_add_ip(res, DNS_IPV6, addr->v6.sin6_addr.s6_addr);
    }

    return -1;
}

static int fixup_addrs(struct dns_result *res)
{
    uint32_t flags = res->flags & (DNS_V4MAPPED | DNS_ALL);
    int keep_ip4 = flags == (DNS_V4MAPPED | DNS_ALL) ? 1 : 0;

    log_debug("nip4=%d nip6=%d kip4=%d", res->num_ip4, res->num_ip6, keep_ip4);

    if (res->num_ip6 > 0 && !keep_ip4) {
        res->num_addr -= res->num_ip4;
        res->num_ip4 = 0;
    }

    for (int i = 0; i < res->num_ip4; i++) {
        struct dns_sockaddr *src = &res->addrs[res->max_addr - 1 - i];
        struct dns_sockaddr *dst = &res->addrs[res->num_ip6 + i];
        // map IPv4 to ::ffff:a.b.c.d 
        if (res->flags & DNS_V4MAPPED) {
            int sock_type = src->sock_type;
            uint32_t ip4  = src->v4.sin_addr.s_addr;
            uint16_t port = src->v4.sin_port;
            memset(&dst->v6, 0, sizeof(dst->v6));
            dst->sock_type = sock_type;
            dst->len = sizeof(dst->v6);
            dst->v6.sin6_family = AF_INET6;
            dst->v6.sin6_port =  port;
            dst->v6.sin6_addr.s6_addr[10] = 0xff;
            dst->v6.sin6_addr.s6_addr[11] = 0xff;
            memcpy(&dst->v6.sin6_addr.s6_addr[12], &ip4, 4);
            continue;
        }
        if (dst != src) *dst = *src;
    }

    return 0;
}

static int read_line(int fd, struct rwbuf *buf, struct str_slice *line)
{
    if (fd == -1) return 0;
    int flags = rwbuf_used(buf) ? 0 : 1;
again:
    if (flags) {
       // read block from file
       void *mem = rwbuf_wptr(buf);
       size_t space = rwbuf_space(buf);
       ssize_t nread = read(fd, mem, space);
       if (nread < 0) return -1;
       flags = nread == 0 ? RWBUF_EOF : 0;
       buf->widx += nread;
    }
    int rc = rwbuf_readline(buf, line, buf->size, flags);
    if (rc || !rwbuf_used(buf)) return rc;
    flags = 1;
    goto again;
}


// options
#define OPT_NDOTS    1
#define OPT_TIMEOUT  2
#define OPT_ATTEMPTS 3
#define OPT_USEVC    4
int str_toopt(struct str_slice str)
{
    if (!slice_cmpmem(str, STR_LIT("ndots"))) return OPT_NDOTS;
    if (!slice_cmpmem(str, STR_LIT("timeout"))) return OPT_TIMEOUT;
    if (!slice_cmpmem(str, STR_LIT("attempts"))) return OPT_ATTEMPTS;
    if (!slice_cmpmem(str, STR_LIT("use-vc"))) return OPT_USEVC;
    return 0;
}

static void cfg_add_options(struct dns_config *cfg, struct str_slice str)
{
    while (str.len) {
        struct str_slice val = slice_splitset(&str, STR_LIT(" \t"));
        struct str_slice key = slice_splitch(&val, ':');
        switch(str_toopt(key)) {
        case OPT_NDOTS:    cfg->ndots = slice_tou32(val); break;
        case OPT_TIMEOUT:  cfg->timeout_secs = slice_tou32(val); break;
        case OPT_ATTEMPTS: cfg->attempts = slice_tou32(val); break;
        case OPT_USEVC:    cfg->use_tcp = 1; break;
       }
    }
}

static void cfg_add_search(struct dns_config *cfg, struct str_slice str)
{
    while (str.len && cfg->num_search < ARR_LEN(cfg->search)) {
       struct str_slice name = slice_splitset(&str, STR_LIT(" \t"));
       if (cfg->store_len + name.len + 1 > sizeof(cfg->store)) return;
        // copy name
        char *str = cfg->store + cfg->store_len;
        memcpy(str, name.ptr, name.len);
        str[name.len] = '\0';
        cfg->store_len += name.len + 1;
        // add to search list
        cfg->search[cfg->num_search++] = str;
    }
}

static int cfg_add_nameserver(struct dns_config *cfg, struct str_slice ip_str)
{ 
    if (cfg->num_ns >= ARR_LEN(cfg->ns_addrs)) return 0;
    struct dns_sockaddr *ns_addr = &cfg->ns_addrs[cfg->num_ns];

    // get addr
    uint8_t ip_addr[16];
    int addr_type = ipstr_decode(ip_str, ip_addr);
    if (addr_type == 0) return log_error_rc(0, "Invalid nameserver %.*s", SLICE(ip_str));

    // add nameserver
    if (!sockaddr_load(ns_addr, addr_type, ip_addr, 0, 53)) return 0;

    cfg->num_ns++;
    return 1;
}

#define CFG_SERVER  1
#define CFG_SEARCH  2
#define CFG_OPTIONS 3
int str_tocfg(struct str_slice str)
{
    if (!slice_cmpmem(str, STR_LIT("nameserver"))) return CFG_SERVER;
    if (!slice_cmpmem(str, STR_LIT("search")))     return CFG_SEARCH;
    if (!slice_cmpmem(str, STR_LIT("options")))    return CFG_OPTIONS;
    return 0;
}

static void init_config(struct dns_config *cfg)
{
    uint8_t tmp[1024];
    struct rwbuf buf = RWBUF_INIT(tmp, sizeof(tmp));
    struct str_slice line;
    int rc;

    int fd = open(DNS_RESOLV_CONF, O_RDONLY);
    while ( (rc = read_line(fd, &buf, &line)) > 0) {
        slice_chop(&line, '#');
        slice_trim(&line);
        if (line.len == 0) continue;
        struct str_slice key = slice_splitset(&line, STR_LIT(" \t"));  
        switch(str_tocfg(key)) {
        case CFG_SERVER:  cfg_add_nameserver(cfg, line); break;
        case CFG_SEARCH:  cfg_add_search(cfg, line); break;
        case CFG_OPTIONS: cfg_add_options(cfg, line); break;
        }
    }
    if (fd != -1) close(fd);

    // set defaults
    if (!cfg->attempts) cfg->attempts = DNS_ATTEMPTS;
    if (!cfg->timeout_secs) cfg->timeout_secs = DNS_TIMEOUT_SECS;
    if (!cfg->ndots) cfg->ndots = 1;
    if (!cfg->num_ns) cfg_add_nameserver(cfg, slice_make(STR_LIT("127.0.0.1")));
}

struct dns_sockaddr *hosts_get(struct dns_hosts *hosts, struct str_slice hostname)
{
    struct dns_sockaddr *addr = NULL;

    // lowercase name
    char name[DNS_HOSTS_MAXNAME];
    size_t name_len = slice_tomem(hostname, name, ARR_LEN(name));
    if (!name_len) return addr;
    str_tolower(name, name_len);

    uint32_t idx = map_get(&hosts->name_toaddr, name);
    if (idx == map_end(&hosts->name_toaddr)) return addr;
    addr = mkmem(map_val(&hosts->name_toaddr, idx));

    return addr;
}

static int hosts_put(struct dns_hosts *hosts, struct str_slice hostname, struct dns_sockaddr *addr)
{
    if (hosts->num_addr >= hosts->max_addr) return 0;

    // lowercase hostname
    char name[DNS_HOSTS_MAXNAME];
    size_t name_len = slice_tomem(hostname, name, ARR_LEN(name));
    if (!name_len) return 0;
    str_tolower(name, name_len);

    uint32_t idx = map_get(&hosts->name_toaddr, name);
    if (idx == map_end(&hosts->name_toaddr)) {
        // new name
        if (hosts->store_len + name_len + 1 > sizeof(hosts->store)) return 0;
        char *str = hosts->store + hosts->store_len;
        memcpy(str, name, name_len);
        str[name_len] = '\0';
        // add to map
        struct dns_sockaddr *sa = &hosts->addrs[hosts->num_addr];
        idx = map_put(&hosts->name_toaddr, str, umkmem(sa));
        if (idx == map_end(&hosts->name_toaddr)) return 0;
        // added
        hosts->store_len += name_len + 1;
        hosts->num_addr++;
    }

    // update
    struct dns_sockaddr *sa = mkmem(map_val(&hosts->name_toaddr, idx));
    *sa = *addr;

    return 1;
}

static int hosts_add(struct dns_hosts *hosts,
    struct str_slice ip, struct str_slice hostname, struct str_slice aliases)
{
    if (hosts->num_addr >= ARR_LEN(hosts->addrs)) return 0;
    struct dns_sockaddr *addr = &hosts->addrs[hosts->num_addr];

    log_debug("ip=%.*s h=%.*s a=%.*s", SLICE(ip), SLICE(hostname), SLICE(aliases));

    // decode ip-addr
    uint8_t ip_addr[16];
    int type = ipstr_decode(ip, ip_addr);
    if (type == 0) return log_error_rc(0, "Invalid ip-addr %.*s", SLICE(ip));
    if (!sockaddr_load(addr, type, ip_addr, 0, 0)) return 0;

    // add hostname
    if (!hosts_put(hosts, hostname, addr)) return 0;

    // add aliases
    int num_add = 1;
    while (aliases.len) {
        struct str_slice alias = slice_splitset(&aliases, STR_LIT(" \t"));
        if (hosts_put(hosts, alias, addr)) num_add++;
    }

    return num_add;
}

// load hosts file - IP_address canonical_hostname [aliases...]
static void init_hosts(struct dns_hosts *hosts)
{
    uint8_t tmp[1024];
    struct rwbuf buf = RWBUF_INIT(tmp, sizeof(tmp));
    struct str_slice line;
    int rc;

    hosts->max_addr = ARR_LEN(hosts->addrs);
    size_t lines = 0, total= 0, num_add = 0;

    int fd = open(DNS_HOSTS, O_RDONLY);
    while ( (rc = read_line(fd, &buf, &line)) > 0) {
        lines++;
        // skip blank lines
        slice_chop(&line, '#');
        slice_trim(&line);
        if (line.len == 0) continue;
        total++;
        struct str_slice addr = slice_splitset(&line, STR_LIT(" \t"));  
        struct str_slice name = slice_splitset(&line, STR_LIT(" \t"));  
        if (addr.len == 0 || name.len == 0) continue;
        if (hosts_add(hosts, addr, name, line)) num_add++;
    }
    if (fd != -1) close(fd);

    log_debug("lines %zu services %zu added %zu", lines, total, num_add);
}

static uint32_t services_get(struct dns_services *svcs, struct str_slice port)
{
    char name[DNS_SVC_MAXNAME];
    size_t len = slice_tomem(port, name, ARR_LEN(name));
    if (!len) return 0;
    str_tolower(name, len);

    uint32_t idx = map_get(&svcs->name_toport, name);
    return map_val(&svcs->name_toport, idx);
}

static int services_put(struct dns_services *svcs,
    struct str_slice service, int ptype, int16_t port)
{
    char name[DNS_SVC_MAXNAME];
    size_t name_len = slice_tomem(service, name, sizeof(name));
    if (!name_len) return 0;
    str_tolower(name, name_len);

    uint32_t idx = map_get(&svcs->name_toport, name);
    if (idx == map_end(&svcs->name_toport)) {
        // new service name
        if (svcs->store_len + name_len + 1 > sizeof(svcs->store)) return 0;
        char *str = svcs->store + svcs->store_len;
        memcpy(str, name, name_len);
        str[name_len] = '\0';
        idx = map_put(&svcs->name_toport, str, 0);
        if (idx == map_end(&svcs->name_toport)) return 0;
        svcs->store_len += name_len + 1;
        svcs->num_svc++;
    }

    // update ptype and port
    uint32_t val = map_val(&svcs->name_toport, idx);
    if (ptype == DNS_TCP) val |= port;
    if (ptype == DNS_UDP) val |= port << 16;
    map_set(&svcs->name_toport, idx, val);

    return 1;
}

static int services_add(struct dns_services *svcs,
    struct str_slice name, struct str_slice pp, struct str_slice aliases)
{
    log_debug("n=%.*s p=%.*s as%.*s", SLICE(name), SLICE(pp), SLICE(aliases));

    if (svcs->num_svc >= svcs->max_svc) return 0;

    // get port and ptype
    struct str_slice port_str = slice_splitch(&pp, '/');
    uint16_t port = slice_tou32(port_str);
    int ptype = 0;
    if (slice_cmpmem(pp, STR_LIT("tcp"))) ptype = DNS_TCP;
    if (slice_cmpmem(pp, STR_LIT("udp"))) ptype = DNS_UDP;
    if (!ptype || !port) return 0;

    return services_put(svcs, name, ptype, port);
}

// load services file : service-name  port/protocol  [aliases ...]
static void init_services(struct dns_services *svcs)
{
    uint8_t tmp[1024];
    struct rwbuf buf = RWBUF_INIT(tmp, sizeof(tmp));
    struct str_slice line;
    int rc;

    if (!map_attach(&svcs->name_toport, svcs->buffer, DNS_SVC_MAXPORT)) return;
    svcs->max_svc = DNS_SVC_MAXPORT;
    size_t lines=0,total=0,num_add = 0;

    int fd = open(DNS_SERVICES, O_RDONLY);
    while ( (rc = read_line(fd, &buf, &line)) > 0) {
        lines++;
        // skip blank lines
        slice_chop(&line, '#');
        slice_trim(&line);
        if (line.len == 0) continue;
        // add service 
        total++;
        struct str_slice name = slice_splitset(&line, STR_LIT(" \t"));  
        struct str_slice pp = slice_splitset(&line, STR_LIT(" \t"));  
        if (name.len == 0 || pp.len == 0) continue;
        if (services_add(svcs, name, pp, line)) num_add++;
    }
    if (fd != -1) close(fd);

    log_debug("lines %zu services %zu added %zu", lines, total, num_add);
}

static struct dns_sockaddr *cache_get(struct dns_cache *cache,
    int type, struct str_slice hostname)
{
    struct dns_sockaddr *addr = NULL;

    log_debug("t=%d n=%.*s", type, SLICE(hostname));

    // lowercase name
    char name[DNS_HOSTS_MAXNAME];
    name[0] = type & DNS_IPV4 ? '4' : '6';
    size_t name_len = slice_tomem(hostname, name + 1, ARR_LEN(name) - 1);
    if (!name_len) return addr;
    name_len++;
    str_tolower(name, name_len);

    uint32_t idx = map_get(&cache->name_toaddr, name);
    if (idx == map_end(&cache->name_toaddr)) return addr;

    addr = mkmem(map_val(&cache->name_toaddr, idx));
    uint32_t now_secs = get_now_ms() / 1000;
    uint32_t expiry = addr->sock_type;

    if (DNS_TIME_GEQ(now_secs, expiry)) {
        map_rem(&cache->name_toaddr, idx);
        addr->sock_type = 0;
        cache->num_addr--;
        return NULL;
    }

    return addr;
}

static int cache_put(struct dns_cache *cache, 
    int type, struct str_slice hostname,
    struct dns_sockaddr *addr, uint32_t ttl)
{
    log_debug("t=%d n=%.*s a=%s ttl=%u", type, SLICE(hostname), ADDR_STR(addr), ttl);

    if (cache->num_addr >= cache->max_addr) return 0;
    if (ttl == 0 || ttl & 0x80000000) return 0;

    // lowercase name
    char name[DNS_HOSTS_MAXNAME];
    name[0] = type & DNS_IPV4 ? '4' : '6';
    size_t name_len = slice_tomem(hostname, name + 1, ARR_LEN(name) - 1);
    if (!name_len) return 0;
    name_len++;
    str_tolower(name, name_len);

    uint32_t idx = map_get(&cache->name_toaddr, name);
    if (idx == map_end(&cache->name_toaddr)) {
        // new name
        if (cache->store_len + name_len + 1 > sizeof(cache->store)) return 0;
        char *str = cache->store + cache->store_len;
        memcpy(str, name, name_len);
        str[name_len] = '\0';
        // find free slot - TODO need array/list
        struct dns_sockaddr *sa = NULL;
        for (size_t i = 0; i < cache->max_addr; i++) {
            if (cache->addrs[i].sock_type == 0) {
                sa = &cache->addrs[i];
                break;
            }
        }
        if (!sa) return 0;
        // add to map
        idx = map_put(&cache->name_toaddr, str, umkmem(sa));
        if (idx == map_end(&cache->name_toaddr)) return 0;
        // added
        cache->store_len += name_len + 1;
        cache->num_addr++;
    }

    // update
    struct dns_sockaddr *sa = mkmem(map_val(&cache->name_toaddr, idx));
    *sa = *addr;
    sa->sock_type = get_now_ms() / 1000 + ttl;

    log_debug("expiry in %u", sa->sock_type);

    return 1;
}

static int init_cache(struct dns_cache *cache)
{
    cache->max_addr = ARR_LEN(cache->addrs);

    return 0;
}

static inline bool dns_sock_is_ready(struct dns_sock *s)
{
    if (!s->is_open || s->has_error) return 0;
    if (!s->is_tcp) return 1;
    return !s->in_progress;
}

static inline void sock_close(struct dns_sock *s)
{
    if (s->is_open) {
        close(s->fd);
        s->is_open = 0;
    }

    // clear state
    s->is_tcp = 0;
    s->in_progress = 0;;
    s->is_connect  = 0;;
    s->has_error = 0;
}

static struct dns_query *get_query(struct dns_ns *ns, uint16_t qtype)
{
    struct dns_result *res = ns->res;

    switch(qtype) {
    case DNS_TYPE_A:    if (res->need_ip4) return &ns->v4_query; break;
    case DNS_TYPE_AAAA: if (res->need_ip6) return &ns->v6_query; break;
    }

    return NULL;
}

static int check_error(struct dns_sock *s)
{
    int ec = 0;
    socklen_t len = sizeof(ec);

    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &ec, &len) < 0) {
        return log_errno_rf("getsockopt(%d) failed", s->fd);
    }

    if (ec != 0) {
        // connect failed
        s->has_error = 0;
        return log_ec_rf(ec, "connect (%d) failed", s->fd);
    }

    // connected
    return 0;
}

static int rcv_dnspkt(struct dns_sock *s)
{
    uint8_t *buf = s->rx_buf + s->rx_off;
    size_t len = sizeof(s->rx_buf) - s->rx_off;

    log_debug("read fd=%d len=%zu", s->fd, len);
again:
    ssize_t nread = read(s->fd, buf, len);
    if (nread == -1) {
        // read failed
        if (errno == EINTR) goto again;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return DNS_EAGAIN;
        return log_errno_rf("read fd=%d failed", s->fd);
    }
    if (nread == 0) return DNS_ERR;

    s->rx_off += nread;

    if (!s->is_tcp) {
        // UDP done
        s->rx_len = s->rx_off;
        return 0;
    }

    // TCP - 2-byte prefix
    if (s->rx_len == 0 && s->rx_off >= 2) {
        s->rx_len = s->rx_buf[0] << 8 | s->rx_buf[1];
        if (s->rx_len + 2 > sizeof(s->rx_buf)) {
            return log_error_rf("prefix %zu too big", s->rx_len);
        }
    }

    if (s->rx_off >= 2 + s->rx_len) {
        // TCP done
        return 0;
    }

    return DNS_EAGAIN;
}

static int snd_dnspkt(struct dns_sock *s)
{
    uint8_t *buf = s->tx_buf + s->tx_off;
    size_t len = s->tx_len - s->tx_off;

again:
    log_debug("write fd=%d len=%zu", s->fd, len);
    ssize_t nw = write(s->fd, buf, len);
    if (nw == -1) {
        // write failed
        if (errno == EINTR) goto again;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return DNS_EAGAIN;
        return log_errno_rf("write fd=%d failed", s->fd);
    }

    // pkt-sent
    s->tx_off += nw;
    if (s->tx_off == s->tx_len) {
        s->tx_len = 0;
        s->tx_off = 0;
        return 0;
    }

    return s->is_tcp ? DNS_EAGAIN : DNS_ERR;
}

static int dec_dnsmsg(struct dns_sock *s, struct dns_msg *msg)
{
    uint8_t *buf = s->rx_buf;
    size_t len = s->rx_len;

    // tcp - skip length prefix
    if (s->is_tcp) {
        buf += 2;
        len -= 2;
    }

    log_debug("pkt_len=%zu msg_len=%zu is_tcp=%d", s->rx_len, len, s->is_tcp);

    return dns_msg_decode(msg, buf, len);
}

static inline bool dns_sock_pending(struct dns_sock *s)
{
    return s->tx_len > 0;
}

static inline void dns_sock_flush(struct dns_sock *s)
{
    s->rx_off = 0;
    s->rx_len = 0;
}

static struct dns_query *find_query(struct dns_ns *ns, uint16_t tid)
{
    struct dns_result *res = ns->res;

    if (res->need_ip4) {
        struct dns_query *q = &ns->v4_query;
        if (q->tid == tid) return q;
    }

    if (res->need_ip6) {
        struct dns_query *q = &ns->v6_query;
        if (q->tid == tid) return q;
    }

    return NULL;
}

static int dec_dnshdr(struct dns_sock *s, struct dns_hdr *hdr)
{
    uint8_t *buf = s->rx_buf;
    size_t len = s->rx_len;

    if (s->is_tcp) {
        buf += 2;
        len -= 2;
    }
    return dns_hdr_decode(hdr, buf, len) ? DNS_EBADHDR : 0;
}

static int enc_dnspkt(struct dns_sock *s, struct dns_msg *msg)
{
    uint8_t *buf = s->tx_buf;
    size_t space = sizeof(s->tx_buf);

    // tcp - reserve space for prefix
    if (s->is_tcp) {
        buf += 2;
        space -= 2;
    }

    // encode dns message
    ssize_t len = dns_msg_encode(msg, buf, space);
    if (len <= 0) return log_error_rf("DNS msg failed with");
    s->tx_len = len;

    // tcp - encode prefix
    if (s->is_tcp) {
        s->tx_buf[0] = len >> 8;
        s->tx_buf[1] = len;
        s->tx_len += 2;
    }

    log_debug("pkt_len=%zu msg_len=%zu is_tcp=%d", s->tx_len, len, s->is_tcp);

    return 0;
}

// setup request
static int set_dnsreq(struct dns_ns *ns, struct dns_query *q)
{
    struct dns_msg *msg = &ns->msg;
    struct dns_hdr *hdr = &msg->hdr;
    struct str_slice name = ns->name;

    q->tid = gen_tid();
    q->qname = ns->name;
    q->qclass = DNS_CLASS_IN;
    dns_msg_init(msg, q->tid, DNS_FLAGS_RD);
    int rc = dns_add_qdn(msg, name.ptr, name.len, q->qtype, q->qclass);
    if (rc) return rc;

    if (ns->use_esdn) {
        // add ESDN0
        struct dns_rr rr = {
            .type = DNS_TYPE_OPT,
            .rdata.opt.udp_size = min(sizeof(ns->sock.rx_buf), 1232)
        };
        rc = dns_add_rr(msg, DNS_MSG_AR, &rr);
        if (rc) return rc;
    }

    log_debug("qclass:%s qtype:%s id:0x%04x opcode:%s rd:%d qd:%u",
        dns_class_tostr(q->qclass), dns_type_tostr(q->qtype),
        hdr->id, opcode_tostr((hdr->flags & DNS_FLAGS_OPCODE) >> 11),
        hdr->flags & DNS_FLAGS_RD ? 1 : 0, msg->qd_len);

    return 0;
}

static int add_dnsans(struct dns_ns *ns)  
{
    struct dns_msg *msg = &ns->msg;
    struct dns_cache *cache = &glob_cache;
    struct dns_result *res = ns->res;
    int idx;

    // a query worked
    ns->have_ans = 1;

    for (int i = 0; i < msg->an_len && !res_isfull(ns->res); i++) {
        struct dns_rr *rr = &msg->an[i];
        switch(rr->type) {
        case DNS_TYPE_A:
            idx = res_add_ip(res, DNS_IPV4, rr->rdata.a);
            if (idx != -1) {
                ns->have_ip4 = 1;
                cache_put(cache, DNS_IPV4, ns->name, &res->addrs[idx], rr->ttl);
            }
            break;
        case DNS_TYPE_AAAA: 
            idx = res_add_ip(res, DNS_IPV6, rr->rdata.aaaa);
            if (idx != -1) {
                ns->have_ip6 = 1;
                cache_put(cache, DNS_IPV6, ns->name, &res->addrs[idx], rr->ttl);
            }
            break;
        }
    }

    log_debug("addr=%s ans=%d ip4=%d ip6=%d", ADDR_STR(ns->addr), ns->have_ans, ns->have_ip4, ns->have_ip6);

    return 0;
}

static int do_connect(struct dns_sock *s, int type, struct dns_sockaddr *addr)
{
    // socket
    int sock_type = type == DNS_UDP ? SOCK_DGRAM : SOCK_STREAM;
    sock_type |= SOCK_NONBLOCK | SOCK_CLOEXEC;
    s->fd = socket(addr->sa.sa_family, sock_type, 0);
    if (s->fd == -1) return log_errno_rf("create_socket(%u) failed", sock_type);
    s->is_open = 1;
    s->is_tcp = type == DNS_TCP;

    if (!addr) return 0;

    // connect
    int rc = connect(s->fd, &addr->sa, addr->len);
    if (rc == -1) {
        if (errno != EINPROGRESS) return log_errno_rf("connect(%s) failed", ADDR_STR(addr));
        s->in_progress = 1;
    }

    s->is_connect = 1;

    return 0;
}

static int try_query(struct dns_ns *ns, uint16_t qtype)
{
    struct dns_query *q = get_query(ns, qtype);
    if (!q) return DNS_IDLE;

    log_debug("state=%d qtype=%s", q->state, dns_type_tostr(qtype));

    switch(q->state) {
    case DNS_IDLE:
        if (dns_sock_pending(&ns->sock)) break;
        ns->active++;
        q->qtype = qtype;
        q->state = DNS_SEND;
        q->last_rc = set_dnsreq(ns, q);
        if (q->last_rc) break;
        q->last_rc = enc_dnspkt(&ns->sock, &ns->msg);
        if (q->last_rc) break;
        // fall thru
    case DNS_SEND: 
        q->last_rc = snd_dnspkt(&ns->sock);
        if (!q->last_rc) q->state = DNS_RECV;
        break;
    case DNS_RECV: 
        break;
    case DNS_DONE: 
        break;
    }

    if (q->last_rc && q->last_rc != DNS_EAGAIN) {
        ns->active--;
        q->state = DNS_DONE;
    }

    return q->state;
}

// check question section matches what we sent
static bool chk_dnsqd(struct dns_query *query, struct dns_msg *rsp)
{
    if (rsp->qd_len != 1) return false;

    struct dns_qd *qd = &rsp->qd[0];
    struct str_slice qname = query->qname;
    if (slice_endswith(qname, '.')) qname.len--;

    if (slice_casecmpstr(qname, qd->qname)) false;
    if (query->qclass != qd->qclass) return false;
    if (query->qtype != qd->qtype) return false;

    return true;
}

// check for OPT
static bool chk_dnsopt(struct dns_query *query, struct dns_msg *rsp)
{
    for (int i = 0; i < rsp->ar_len; i++) {
        struct dns_rr *rr = &rsp->ar[i];
        if (rr->type != DNS_TYPE_OPT) continue;
        query->last_rc = (rr->rdata.opt.ext_rcode << 4) | (query->last_rc & 0xf);
        // updated rcode
        return true;
    }

    return false;
}

// check response is valid
static int chk_dnsrsp(struct dns_ns *ns, struct dns_hdr *hdr, struct dns_query *q)
{
    log_debug("qclass:%s qtype:%s id:0x%04x opcode:%s rcode:%s qd:%d an:%d",
        dns_class_tostr(q->qclass), dns_type_tostr(q->qtype),
        hdr->id, opcode_tostr((hdr->flags & DNS_FLAGS_OPCODE) >> 11),
        rcode_tostr(hdr->flags & DNS_FLAGS_RCODE), hdr->qd_count, hdr->an_count);

    ns->active--;
    q->state = DNS_DONE; 
    q->last_rc = hdr->flags & DNS_FLAGS_RCODE;
    if (hdr->flags & DNS_FLAGS_TC) return ns->use_tcp ? DNS_EPROTO : DNS_ETRUNC;
    if (q->last_rc) return q->last_rc;

    if (dec_dnsmsg(&ns->sock, &ns->msg)) return DNS_EBADMSG;
    if (!chk_dnsqd(q, &ns->msg)) return DNS_EQUEST;
    if (chk_dnsopt(q, &ns->msg)) return q->last_rc;

    // got okay resp
    return 0;
}

static int chk_dnspkt(struct dns_ns *ns)
{
    struct dns_hdr hdr;
    struct dns_query *q;

    if (dec_dnshdr(&ns->sock, &hdr)) return DNS_EBADHDR;
    if (!(hdr.flags & DNS_FLAGS_QR)) return DNS_ENOTRSP;
    if (!(q = find_query(ns, hdr.id))) return DNS_ETID;

    return chk_dnsrsp(ns, &hdr, q);
}

static int do_resp(struct dns_ns *ns)
{
    int rc;

    if ((rc = rcv_dnspkt(&ns->sock))) return rc;
    if ((rc = chk_dnspkt(ns))) return rc;
    if ((rc = add_dnsans(ns))) return rc;

    return 0;
}

static void recv_resp(struct dns_ns *ns, struct pollfd *pfd)
{
    int rc = do_resp(ns);

    if (rc == DNS_EAGAIN) return;
    if (rc == DNS_ERR || (rc == DNS_EBADHDR && ns->sock.is_tcp)) {
        // fail socket
        ns->active = 0;
        pfd->fd = -1;
    }

    dns_sock_flush(&ns->sock);
}

static void send_query(struct dns_ns *ns, struct pollfd *pfd)
{
    struct dns_sock *sock = &ns->sock;

    if (sock->in_progress) {
        // connect finished
        sock->in_progress = 0;
        ns->active--;
        int rc = check_error(sock);
        if (rc) return;
    }

    int do_ip4 = try_query(ns, DNS_TYPE_A);
    int do_ip6 = try_query(ns, DNS_TYPE_AAAA);

    int events = 0;
    if (do_ip4 == DNS_SEND || do_ip6 == DNS_SEND) events |= POLLOUT;
    if (do_ip4 == DNS_RECV || do_ip6 == DNS_RECV) events |= POLLIN;

    pfd->fd = sock->fd;
    pfd->events = events;
}

static bool connect_now(struct dns_ns *ns, struct pollfd *pfd)
{
    int type = ns->use_tcp ? DNS_TCP : DNS_UDP;
    struct dns_sock *sock = &ns->sock;
    int rc = do_connect(sock, type, ns->addr);

    log_debug("rc=%d addr=%s type=%s", rc, ADDR_STR(ns->addr), TYPE_STR(type));
    if (rc) return false;

    if (sock->in_progress) {
        // wait for connect
        ns->active++;
        pfd->fd = sock->fd;
        pfd->events = POLLOUT;
        return false;
    }

    return true;
}

static int get_rcode(struct dns_ns *ns, int qtype)
{
    struct dns_query *q = get_query(ns, qtype);
    if (!q) return 0;

    // normalize rc
    int rc;
    switch(q->last_rc) {
    case DNS_RCODE_NOERROR:  rc = DNS_OK; break;
    case DNS_RCODE_FORMERR:  rc = DNS_FORMERR; break;
    case DNS_RCODE_SERVFAIL: rc = DNS_SERVFAIL; break;
    case DNS_RCODE_NXDOMAIN: rc = DNS_NXDOMAIN; break;
    case DNS_RCODE_NOTIMP:   rc = DNS_NOTIMP;  break;
    case DNS_RCODE_REFUSED:  rc = DNS_REFUSED; break;
    default:
        rc = q->last_rc;
        if (rc != DNS_ETRUNC) rc = DNS_ERR;
        break;
    }

    return rc;
}

static inline int64_t get_deadline_ms(struct dns_ns *ns)
{
    return get_now_ms() + ns->timeout_ms;
}

static inline int get_rem_ms(int64_t deadline_ms)
{
    int64_t now_ms = get_now_ms();
    return (int) (deadline_ms - now_ms);
}

// merge query rcodes into one
static int ns_getrc(struct dns_ns *ns, int rc)
{
    if (rc == DNS_ETIMEOUT) return rc;
    if (ns->have_ip4 || ns->have_ip6) return DNS_OK;
    if (ns->have_ans) return DNS_NODATA;

    int ip4_rc = get_rcode(ns, DNS_TYPE_A);
    int ip6_rc = get_rcode(ns, DNS_TYPE_AAAA);

    log_debug("ip4_rc:%d ip6_rc=%d", ip4_rc, ip6_rc);

    if (ip4_rc == ip6_rc) return ip4_rc;
    if (ip4_rc == DNS_NXDOMAIN) return ip6_rc;
    if (ip6_rc == DNS_NXDOMAIN) return ip4_rc;

    return ip4_rc;
}

static int try_nameserver(struct str_slice name,
    struct dns_sockaddr *addr, uint32_t timeout_secs, int use_tcp,
    struct dns_result *res)
{
    log_debug("n=%.*s a=%s tout=%d tcp=%d 4=%d 6=%d", 
        SLICE(name), ADDR_STR(addr), timeout_secs, use_tcp, NEED(res));

    struct dns_ns ns = {
        .name = name,
        .addr = addr,
        .res =  res,
        .timeout_ms = timeout_secs * 1000,
        .use_tcp = use_tcp,
        .use_esdn = 1
    };

    struct pollfd fd;
    if (connect_now(&ns, &fd)) {
        send_query(&ns, &fd);
    }

    int64_t deadline_ms = get_deadline_ms(&ns);
    int rc = DNS_OK;

    while (ns.active) {
        int ms = get_rem_ms(deadline_ms);
        rc = poll(&fd, 1, ms);
        if (rc <= 0) {
            if (rc == 0) rc = DNS_ETIMEOUT;
            if (errno == EINTR) {
                if (!glob_sig || !glob_sig->run) break;
                continue;
            }
            log_errno("poll failed");
            break;
        }
        if (fd.events == POLLOUT) send_query(&ns, &fd);
        if (fd.events == POLLIN)  recv_resp(&ns, &fd);
    }

    sock_close(&ns.sock);

    rc = ns_getrc(&ns, rc);
    log_debug("rc=%d", rc);

    return rc;
}

static inline int stop_query(int rc)
{
    return (rc == DNS_OK || rc == DNS_NODATA || rc == DNS_NXDOMAIN) ? 1 : 0;
}

static inline int stop_search(int rc)
{
    return rc == DNS_OK || rc == DNS_NODATA ? 1 : 0;
}

static struct str_slice build_name(struct strbuf *buf,
    struct str_slice name, struct str_slice suffix)
{
    strbuf_reset(buf);
    if (slice_startswith(suffix, '.')) 
        strbuf_putm(buf, name.ptr, name.len);
    else {
        strbuf_putmc(buf, name.ptr, name.len, '.');
    }
    if (slice_endswith(suffix, '.')) {
        strbuf_putm(buf, suffix.ptr, suffix.len);
    }
    else {
        strbuf_putmc(buf, suffix.ptr, suffix.len, '.');
    }

    return slice_make(strbuf_start(buf), strbuf_used(buf));
}

static int try_cache(struct str_slice name, struct dns_result *res)
{
    log_debug("lookup n=%.*s ", SLICE(name));

    struct dns_cache *cache = &glob_cache;

    if (res->need_ip4) {
       struct dns_sockaddr *addr = cache_get(cache, DNS_IPV4, res->host);
       if (addr && res_add_addr(res, addr) != -1) {
           res->need_ip4 = 0;
       }
    }

    if (res->need_ip6) {
       struct dns_sockaddr *addr = cache_get(cache, DNS_IPV6, res->host);
       if (addr && res_add_addr(res, addr) != -1) {
           res->need_ip6 = 0;
       }
    }

    return res->need_ip4 || res->need_ip6;
}

static int query_name(struct str_slice name, struct dns_config *cfg, struct dns_result *res)
{
    log_debug("n %.*s #att %d tmo %u #ns %zu", SLICE(name), CFG_QN(cfg));

    if (!try_cache(name, res)) return 0;

    int rc = DNS_ERR;
    for (size_t a = 0; a < cfg->attempts; a++) {
        for (size_t n = 0; n < cfg->num_ns; n++) {
            struct dns_sockaddr *addr = &cfg->ns_addrs[n];
            rc = try_nameserver(name, addr, cfg->timeout_secs, cfg->use_tcp, res);
            if (rc == DNS_ETRUNC) {
                rc = try_nameserver(name, addr, cfg->timeout_secs, 1, res);
            }
            if (stop_query(rc)) return rc;
        }
    }

    return rc;
}

static int query_search(struct str_slice name, struct dns_config *cfg, struct dns_result *res)
{
    int rc = DNS_NXDOMAIN;
    char tmp[DNS_HOSTS_MAXNAME];
    struct strbuf buf = STRBUF_INIT(tmp, sizeof(tmp));

    log_debug("n=%.*s nsearch=%zu", SLICE(name), cfg->num_search);

    for (size_t s = 0; s < cfg->num_search; s++) {
        struct str_slice suffix = slice_make_cstr(cfg->search[s]);
        struct str_slice search = build_name(&buf, name, suffix);
        rc = query_name(search, cfg, res);
        if (stop_search(rc)) return rc;
    }

    return rc;
}

static int try_nameservers(struct dns_result *res)
{
    struct dns_config *cfg = &glob_cfg;

    log_debug("#att %u tmo:%u ndots %u #ns %zu", CFG_NS(cfg));

    // check fqdn
    struct str_slice name = res->host;
    if (name.len == 0) return 0;
    int is_fqdn = slice_endswith(name, '.');
    size_t ndots = slice_countch(name, '.');

    // send-query
    int rc;
    if (is_fqdn) {
        rc = query_name(name, cfg, res);
    }
    else if (ndots >= cfg->ndots) {
        rc = query_name(name, cfg, res);
        if (rc) rc = query_search(name, cfg, res);
    }
    else {
        rc = query_search(name, cfg, res);
        if (rc) rc = query_name(name, cfg, res);
    }

    // results
    fixup_addrs(res);
    rc = rc ?: res->num_addr;
    log_debug("res=%d", rc);

    return rc;
}

static int try_port(struct dns_result *res)
{
    // unspecified type
    int any = 0;
    if ((res->flags & (DNS_TCP | DNS_UDP)) == 0) {
        res->flags |= (DNS_TCP | DNS_UDP);
        any = 1;
    }

    // empty port-str
    struct str_slice port = res->port;
    if (!port.len) return 0;

    // numeric port-str
    if (slice_isnumeric(port)) {
        uint32_t portno = slice_tou32(port);
        if (portno > 65535) return log_error_rf("port '%.*s' too big", SLICE(port));
        if (res->flags & DNS_TCP) res->tcp_port = portno;
        if (res->flags & DNS_UDP) res->udp_port = portno;
        return 0;
    }

    if (res->flags & DNS_NUMPORT) return log_error_rf("port '%.*s' not a number", SLICE(port));

    // service port-str
    uint32_t pp = services_get(&glob_svcs, port);
    uint16_t tcp_port= pp & 0xffff;
    uint16_t udp_port = pp >> 16;

    uint32_t found = 0;
    if ((res->flags & DNS_TCP) && tcp_port) {
        res->tcp_port = tcp_port;
        found |= DNS_TCP;
    }
    if ((res->flags & DNS_UDP) && udp_port) {
        res->udp_port = udp_port;
        found |= DNS_UDP;
    }

    if (!found) return log_error_rf("port %.*s not-found", SLICE(port));
    uint32_t want = res->flags & (DNS_TCP | DNS_UDP);
    if (!any && want != found) return log_error_rf("port %.*s no-match", SLICE(port));

    return 0;
}

static int try_hostname(struct dns_result *res)
{
    // unspecified address (AF_UNSPEC)
    if ((res->flags & (DNS_IPV4 | DNS_IPV6)) == 0) {
        res->flags |= (DNS_IPV4 | DNS_IPV6);
    }

    // set the name querys we need
    if (res->flags & DNS_IPV6) res->need_ip6 = 1;
    if (res->flags & DNS_IPV4) res->need_ip4 = 1;
    if (res->flags & (DNS_IPV6 | DNS_V4MAPPED)) res->need_ip4 = 1;

    struct str_slice host_str = res->host;

    // empty hostname
    if (!host_str.len) {
        if (res->flags & DNS_PASSIVE) {
            if (res->need_ip4) res_add_ip(res, DNS_IPV4, ip4_any);
            if (res->need_ip6) res_add_ip(res, DNS_IPV6, ip6_any);
        }
        else {
            if (res->need_ip4) res_add_ip(res, DNS_IPV4, ip4_loopback);
            if (res->need_ip6) res_add_ip(res, DNS_IPV6, ip6_loopback);
        }
        fixup_addrs(res);
        return res->num_addr ?: -1;
    }

    // decode ip-addr
    uint8_t ip_addr[16];
    uint32_t addr_type = ipstr_decode(host_str, ip_addr);
    if (!addr_type) return 0; // assume DNS name

    // check allowed
    uint32_t allow = res->flags & (DNS_IPV4 | DNS_IPV6 | DNS_V4MAPPED);
    if (allow & DNS_V4MAPPED) allow |= DNS_IPV4;
    if ((allow & addr_type)  == 0) {
        return log_error_rf("ip-addr type %d match %d failed for %.*s",
            addr_type, allow, SLICE(host_str));
    }

    // add
    res_add_ip(res, addr_type, ip_addr);
    fixup_addrs(res);

    return res->num_addr ?: -1;
}

static int try_hosts(struct dns_result *res)
{
    // lookup host
    struct dns_hosts *hosts = &glob_hosts;
    struct dns_sockaddr *addr = hosts_get(hosts, res->host);

    log_debug("lookup host=%.*s addr=%s", SLICE(res->host), dns_sockaddr_tostr(addr));
    if (!addr) return 0;

    // add hosts entry
    res_add_addr(res, addr);
    fixup_addrs(res);

    return res->num_addr ?: -1;
}

int dns_init(uint32_t hosts_size, uint32_t cache_size, struct simple_sig *sig)
{
    (void) hosts_size;
    (void) cache_size;

    init_config(&glob_cfg);
    init_hosts(&glob_hosts);
    init_services(&glob_svcs);
    init_cache(&glob_cache);

    glob_sig = sig;

    return 0;
}

// resolve hostname,port to array of addr - return num-addr or error
int dns_resolv(uint32_t flags,
    const char *hostname, const char *port,
    int max_addr, struct dns_sockaddr addrs[max_addr])
{
    struct dns_result res = {
        .flags  = flags,
        .host   = slice_make_cstr(hostname),
        .port   = slice_make_cstr(port),
        .max_addr = max_addr,
        .addrs    = addrs
    };

    log_debug("flags=0x%x host=%s port=%s max_addr=%u", 
        flags, hostname, port, max_addr);

    if (max_addr <= 0) return 0;

    int rc;
    if ((rc = try_port(&res))) return rc;
    if ((rc = try_hostname(&res))) return rc;
    if ((rc = try_hosts(&res))) return rc;
    if ((rc = try_nameservers(&res))) return rc;

    // no match
    return 0;
}

// convert addr to text form
char *dns_sockaddr_tostr(struct dns_sockaddr *addr)
{
    static char bufs[16][IP_ADDRPORT_STRLEN];
    static int idx;

    // select buffer
    char *buf = bufs[idx];
    size_t len = sizeof(bufs[0]);
    idx = (idx + 1) & 15;

    char *str = "<null>";
    if (!addr) return str;
    int flags;

    switch (addr->sa.sa_family) {
    case AF_INET: // a.b.c.d:port
        if (addr->len != sizeof(addr->v4)) break;
        str = buf;
        str += ip4_str_encode((void *) &addr->v4.sin_addr.s_addr, str, len); 
        if (!addr->v4.sin_port) break;
        *str++ = ':'; 
        str = uint16_toa(str, __builtin_bswap16(addr->v4.sin_port));
        str = '\0';
        break;
    case AF_INET6: // [::]:port
        if (addr->len != sizeof(addr->v6)) break;
        str = buf;
        str = buf;
        flags = IP6_STR_STRIPV4;
        if (addr->v6.sin6_port) flags |= IP6_STR_ADDBRACK;
        str += ip6_str_encode(addr->v6.sin6_addr.s6_addr, flags, str, len);
        if (!addr->v6.sin6_port) break;
        *str++ = ':'; 
        str = uint16_toa(str, __builtin_bswap16(addr->v6.sin6_port));
        str = '\0';
        break;
    }

    return buf;
}

char *dns_socktype_tostr(struct dns_sockaddr *addr)
{
    switch(addr->sock_type) {
    case SOCK_STREAM: return "TCP";
    case SOCK_DGRAM:  return "UDP";
    default: return "???";
    }
}
