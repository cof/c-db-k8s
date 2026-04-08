/*
 * DNS-RESOLV - a DNS resolver API
 * -------------------------------
 * See dns_resolv.h for API description.
 *
 * API sections
 * ------------
 * dns_resolv - resolve hostname/port to array of add 
 *
 */
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

// hosts file
struct dns_hosts {
    size_t store_len;
    size_t num_addr;
    hashmapstr name_toaddr;
    mapstr_entry buffer[DNS_HOSTS_MAXADDR * 4 / 3];
    char store[DNS_HOSTS_MAXSTORE];
    struct dns_sockaddr addrs[DNS_HOSTS_MAXADDR];
};

// resolv.conf
struct dns_config {
    uint32_t attempts;
    uint32_t timeout_secs;
    uint32_t ndots;
    size_t num_search;
    size_t num_ns;
    size_t store_len;
    // buffers
    char store[DNS_CFG_MAXSTORE];
    char *search[DNS_CFG_MAXSRCH];
    struct dns_sockaddr ns_addrs[DNS_CFG_MAXNS];
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
};

// conection state
enum dns_state {
    DNS_IDLE = 0,
    DNS_CONN,
    DNS_SEND,
    DNS_RECV,
    DNS_DONE
};

// query state
struct dns_query {
    int fd;
    int last_rc;
    uint16_t qtype;
    uint16_t qclass;
    uint16_t tid;
    size_t pkt_off;
    size_t pkt_len;
    uint8_t pkt_buf[DNS_PKTSIZE];
    unsigned int sock_err : 1;
    unsigned int is_tcp   : 1;
    unsigned int recv_fin : 1;
    unsigned int state    : 3;
};

// nameserver state
struct dns_ns {
    struct str_slice name;
    struct dns_result *res;
    struct dns_sockaddr *addr;
    uint32_t timeout_ms;
    int active;
    struct dns_query v4_query;
    struct dns_query v6_query;
    struct dns_msg msg; 
    unsigned int have_ans  : 1;
    unsigned int have_ip4  : 1;
    unsigned int have_ip6  : 1;
};

static struct dns_config glob_cfg;
static struct dns_hosts  glob_hosts;

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

static inline int need_ip4(uint32_t flags)
{
    if (flags & DNS_IPV4) return 1;
    if (flags & (DNS_IPV6 | DNS_V4MAPPED)) return 1;
    return 0;
}

static inline int need_ip6(uint32_t flags)
{
    return flags & DNS_IPV6 ? 1 : 0;
}

// decode ip-addr str
static uint32_t ipstr_decode(struct str_slice str, uint8_t dst[static 16])
{
    if (ip4_str_decode(str.ptr, str.len, dst)) return DNS_IPV4;
    if (ip6_str_decode(str.ptr, str.len, dst)) return DNS_IPV6;
    return 0;
}

// store ip+port to addr
static int sockaddr_store(struct dns_sockaddr *addr, 
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
        addr->v4.sin_port   = port;
        memcpy(&addr->v4.sin_addr.s_addr, ip, 4);
        return 1;
    case DNS_IPV6:
        addr->sock_type = sock_type;
        addr->len = sizeof(addr->v6);
        addr->v6.sin6_family = AF_INET6;
        addr->v6.sin6_port   = port;
        memcpy(&addr->v6.sin6_addr, ip, 16);
        return 1;
    }

    return 0;
}

static inline int res_isfull(struct dns_result *res)
{
    return res->num_addr >= res->max_addr;
}

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
    default: return 0;
    }

    if (sockaddr_store(&res->addrs[idx], type, ip, ptype, port)) {
        switch(type) {
        case DNS_IPV4: res->num_ip4++; break;
        case DNS_IPV6: res->num_ip6++; break;
        }
        res->num_addr++;
        return 1;
    }

    return 0;
}

static int res_add_ip(struct dns_result *res, int type, const void *ip)
{
    int add_tcp = res->flags & DNS_TCP;
    int add_udp = res->flags & DNS_UDP;
    int add = 0;

    if (add_tcp && res_add_ip_port(res, type, ip, DNS_TCP, res->tcp_port)) add++;
    if (add_udp && res_add_ip_port(res, type, ip, DNS_UDP, res->udp_port)) add++;

    return add;
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

    return 0;
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

static int read_block(int fd, struct rwbuf *buf)
{
    void *mem = rwbuf_wptr(buf);
    size_t space = rwbuf_space(buf);

    ssize_t nread = read(fd, mem, space);
    if (nread < 0) return -1;
    if (nread == 0) return 0;
    buf->widx += nread;

    return 1;
}

static int try_services(char *file_path, int flags, struct str_slice name, struct dns_result *res)
{
    int fd = open(file_path, O_RDONLY);
    if (fd == -1) return log_errno_rf("open %s failed", file_path);

    uint8_t tmp[1024];
    struct rwbuf buf = RWBUF_INIT(tmp, sizeof(tmp));

    int need_tcp = flags & DNS_TCP;
    int need_udp = flags & DNS_UDP;
    int rc;

    while ((rc = read_block(fd, &buf)) >= 0) {
        struct str_slice line;
        int flags = rc == 0 ? RWBUF_EOF : 0;
        // name port/protocol alias
        while ( (rc = rwbuf_readline(&buf, &line, 0, flags)) > 0) {
            slice_chop(&line, '#');
            slice_trim(&line);
            if (line.len == 0) continue;
            struct str_slice service = slice_copy(line);
            struct str_slice args = slice_split(&name, ' ');  
            slice_trim(&name);
            if (!slice_eq(service, name)) continue;
            // found service name
            struct str_slice port = slice_copy(args);
            //struct str_slice alias = slice_split(&port, ' ');  
            slice_trim(&port);
            struct str_slice proto = slice_split(&port, '/');
            if (!slice_isnumeric(port)) continue; 
            uint16_t port_no = slice_tou32(port);
            if (slice_eqmem(proto, STR_LIT("tcp")) && need_tcp) {
                res->tcp_port = port_no; 
                need_tcp = 0;
            }
            if (slice_eqmem(proto, STR_LIT("udp")) && need_udp) {
                res->udp_port = port_no;
                need_udp = 0;
            }
            if (!need_tcp && !need_udp) break;
        }
        if (rc <= 0 || (!need_tcp && !need_udp)) break;
    }

    close(fd);
    if (rc < 0) return rc;

    return 0;
}

// options
#define OPT_NDOTS    1
#define OPT_TIMEOUT  2
#define OPT_ATTEMPTS 3
int str_toopt(struct str_slice str)
{
    if (slice_eqmem(str, STR_LIT("ndots"))) return OPT_NDOTS;
    if (slice_eqmem(str, STR_LIT("timeout"))) return OPT_TIMEOUT;
    if (slice_eqmem(str, STR_LIT("attempts"))) return OPT_ATTEMPTS;
    return 0;
}

static void add_options(struct dns_config *cfg, struct str_slice str)
{
    while (str.len) {
        struct str_slice key = slice_consume(&str, ' ');
        slice_trim(&key);
        struct str_slice val = slice_split(&key, ':');
        switch(str_toopt(key)) {
        case OPT_NDOTS:    cfg->ndots = slice_tou32(val); break;
        case OPT_TIMEOUT:  cfg->timeout_secs = slice_tou32(val); break;
        case OPT_ATTEMPTS: cfg->attempts = slice_tou32(val); break;
       }
    }
}

static void add_search(struct dns_config *cfg, struct str_slice str)
{
    while (str.len && cfg->num_search < ARR_LEN(cfg->search)) {
       struct str_slice name = slice_consume(&str, ' ');  
       slice_trim(&name);
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

static int add_server(struct dns_config *cfg, struct str_slice str)
{ 
    if (cfg->num_ns >= ARR_LEN(cfg->ns_addrs)) return 0;
    struct dns_sockaddr *addr = &cfg->ns_addrs[cfg->num_ns];

    // get addr
    uint8_t ip_addr[16];
    int type = ipstr_decode(str, ip_addr);
    if (type == 0) return log_error_rc(0, "Invalid nameserver %.*s", SLICE(str));

    // add
    int ptype = SOCK_DGRAM;
    uint16_t port = __builtin_bswap16(53);
    if (!sockaddr_store(addr, type, ip_addr, ptype, port)) return 0;

    cfg->num_ns++;
    return 1;
}

#define CFG_SERVER  1
#define CFG_SEARCH  2
#define CFG_OPTIONS 3
int str_tocfg(struct str_slice str)
{
    if (slice_eqmem(str, STR_LIT("nameserver"))) return CFG_SERVER;
    if (slice_eqmem(str, STR_LIT("search")))     return CFG_SEARCH;
    if (slice_eqmem(str, STR_LIT("options")))    return CFG_OPTIONS;
    return 0;
}

static int load_resolv_conf(char *file_path, struct dns_config *cfg)
{
    int fd = open(file_path, O_RDONLY);
    if (fd == -1) return log_errno_rf("open %s failed", file_path);

    uint8_t tmp[1024];
    struct rwbuf buf = RWBUF_INIT(tmp, sizeof(tmp));
    int num_ns = 0;
    int rc;
    
    while ((rc = read_block(fd, &buf)) >= 0) {
        struct str_slice line;
        int flags = rc == 0 ? RWBUF_EOF : 0;
        while ( (rc = rwbuf_readline(&buf, &line, 0, flags)) > 0) {
            slice_chop(&line, '#');
            slice_trim(&line);
            if (line.len == 0) continue;
            struct str_slice key = slice_copy(line);
            struct str_slice val = slice_split(&key, ' ');  
            slice_trim(&key);
            slice_trim(&val);
            switch(str_tocfg(key)) {
            case CFG_SERVER:  add_server(cfg, val); break;
            case CFG_SEARCH:  add_search(cfg, val); break;
            case CFG_OPTIONS: add_options(cfg, val); break;
            }
        }
        if (rc <= 0) break;
    }
   
    close(fd);
    if (rc < 0) return rc;

    return num_ns;
}

struct dns_sockaddr *hosts_find(struct dns_hosts *hosts, struct str_slice host)
{
    uint32_t idx = map_get(&hosts->name_toaddr, unmake_mem(host.ptr));
    if (idx == map_end(&hosts->name_toaddr)) return NULL;
    return make_mem(map_val(&hosts->name_toaddr, idx));
}

static int add_hosts(struct dns_hosts *hosts, struct str_slice ip_str, struct str_slice names)
{
    if (hosts->num_addr >= ARR_LEN(hosts->addrs)) return 0;
    struct dns_sockaddr *addr = &hosts->addrs[hosts->num_addr];

    log_debug("ip_str=%.*s names=%.*s", SLICE(ip_str), SLICE(names));

    // decode ip-addr
    uint8_t ip_addr[16];
    int type = ipstr_decode(ip_str, ip_addr);
    if (type == 0) return log_error_rc(0, "Invalid ip-addr %.*s", SLICE(ip_str));
    if (!sockaddr_store(addr, type, ip_addr, 0, 0)) return 0;

    int num_add = 0;
    while (names.len) {
        struct str_slice name = slice_consume(&names, ' ');
        slice_trim(&name);
        if (name.len == 0) continue;
        if (hosts->store_len + name.len + 1 > sizeof(hosts->store)) continue;
        // store name
        char *str = hosts->store + hosts->store_len;
        memcpy(str, name.ptr, name.len);
        str[name.len] = '\0';
        hosts->store_len += name.len + 1;
        // add name:ip-addr
        uint32_t idx = map_put(&hosts->name_toaddr, unmake_mem(str), unmake_mem(addr));
        if (idx != map_end(&hosts->name_toaddr)) num_add++;
    }

    if (num_add) {
        hosts->num_addr++;
        return 1;
    }
    
    return 0;
}

static int load_hosts(const char *file_path, struct dns_hosts *hosts)
{
    int fd = open(file_path, O_RDONLY);
    if (fd == -1) return log_errno_rf("open %s failed", file_path);

    uint8_t tmp[1024];
    struct rwbuf buf = RWBUF_INIT(tmp, sizeof(tmp));
    int rc;
    
    while ((rc = read_block(fd, &buf)) >= 0) {
        struct str_slice line;
        int flags = rc == 0 ? RWBUF_EOF : 0;
        while ( (rc = rwbuf_readline(&buf, &line, 0, flags)) > 0) {
            slice_chop(&line, '#');
            slice_trim(&line);
            if (line.len == 0) continue;
            struct str_slice key = slice_copy(line);
            struct str_slice val = slice_split(&key, ' ');  
            slice_trim(&key);
            slice_trim(&val);
            if (key.len == 0 || val.len == 0) continue;
            add_hosts(hosts, key, val);
        }
        if (rc <= 0) break;
    }
    close(fd);
    if (rc < 0) return rc;

    return 0;
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

static inline void query_close(struct dns_query *q)
{
    if (q->fd >= 0) {
        close(q->fd);
        q->fd = -1;
    }
    // reset state
    q->state = DNS_IDLE;
}

static struct dns_query *get_query(struct dns_ns *ns, uint16_t qtype)
{
    struct dns_result *res = ns->res;

    switch(qtype) {
    case DNS_TYPE_A:    if (need_ip4(res->flags)) return &ns->v4_query; break;
    case DNS_TYPE_AAAA: if (need_ip6(res->flags)) return &ns->v6_query; break;
    }

    return NULL;
}

static int chk_connect(struct dns_query *q)
{
    int error = 0;
    socklen_t len = sizeof(error);

    if (getsockopt(q->fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        q->sock_err = 1;
        return log_errno_rf("getsockopt(%d) failed", q->fd);
    }

    if (error != 0) {
        // connect failed
        q->sock_err = -1;
        return log_ec_rf(error, "connect (%d) failed", q->fd);
    }

    // connected
    return 0;
}

static int rcv_dnspkt(struct dns_query *q)
{
    size_t rlen = sizeof(q->pkt_buf);

    if (q->is_tcp) {
        rlen = q->pkt_off < 2
            ? 2 - q->pkt_off
            : q->pkt_len - (q->pkt_off - 2);
    }

retry:
    log_debug("read fd=%d len=%zu", q->fd, rlen);
    uint8_t *rptr = q->pkt_buf + q->pkt_off;
    ssize_t nread = read(q->fd, rptr, rlen);
    if (nread == -1) {
        // read failed
        if (errno == EINTR) goto retry;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return DNS_EAGAIN;
        return log_errno_rf("read fd=%d failed", q->fd);
    }
    if (nread == 0) return DNS_ECLOSED;
    q->pkt_off += nread;

    // UDP done
    if (!q->is_tcp) {
        q->pkt_len = q->pkt_off;
        return 0;
    }

    // TCP 2-byte prefix
    if (q->pkt_off == 2) {
        q->pkt_len = dec_u16(q->pkt_buf);
        if (q->pkt_len + 2 > sizeof(q->pkt_buf)) {
            return log_error_rf("prefix %zu too big", q->pkt_len);
        }
        rlen = q->pkt_len;
        goto retry;
    }

    // TCP done
    if (q->pkt_off > 2 && q->pkt_off - 2 == q->pkt_len) {
        return 0;
    }

    return DNS_EAGAIN;
}

static int snd_dnspkt(struct dns_query *q)
{
    uint8_t *wptr = q->pkt_buf + q->pkt_off;
    size_t wlen = q->pkt_len - q->pkt_off;

retry:
    log_debug("write fd=%d len=%zu", q->fd, wlen);
    ssize_t nw = write(q->fd, wptr, wlen);
    if (nw == -1) {
        // write failed
        if (errno == EINTR) goto retry;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return DNS_EAGAIN;
        return log_errno_rf("write fd=%d failed", q->fd);
    }

    q->pkt_off += nw;

    // pkt-sent
    if (q->pkt_off == q->pkt_len) {
        q->pkt_off = 0;
        return 0;
    }

    return q->is_tcp ? DNS_EAGAIN : DNS_ERR;
}

static int dec_dnspkt(struct dns_ns *ns, struct dns_query *q)
{
    uint8_t *buf = q->pkt_buf;
    size_t len = q->pkt_len;

    // tcp - skip length prefix
    if (q->is_tcp) {
        buf += 2;
        len -= 2;
    }

    log_debug("pkt_len=%zu msg_len=%zu is_tcp=%d", q->pkt_len, len, q->is_tcp);

    return dns_msg_decode(&ns->msg, buf, len);
}

static int enc_dnspkt(struct dns_ns *ns, struct dns_query *q)
{
    uint8_t *wbuf = q->pkt_buf;
    size_t wlen = sizeof(q->pkt_buf);

    // tcp - reserve space for prefix
    if (q->is_tcp) {
        wbuf += 2;
        wlen -= 2;
    }

    // encode dns message
    ssize_t len = dns_msg_encode(&ns->msg, wbuf, wlen);
    if (len <= 0) return log_error_rf("DNS msg failed with");
    q->pkt_len = len;

    // tcp - encode length prefix
    if (q->is_tcp) {
        enc_u16(q->pkt_buf, len);
        q->pkt_len += 2;
    }

    log_debug("pkt_len=%zu msg_len=%zu is_tcp=%d", q->pkt_len, len, q->is_tcp);

    return 0;
}

// check msg is valid response
static int chk_dnsmsg(struct dns_ns *ns, struct dns_query *q)
{
    struct dns_msg *msg = &ns->msg;
    struct dns_hdr *hdr = &msg->hdr;
    int is_rsp = hdr->flags & DNS_FLAGS_QR ? 1 : 0;
    int rcode = hdr->flags & DNS_FLAGS_RCODE;

    log_debug("id:0x%04x qr:%d opcode:%s rcode:%s qd:%zu an:%zu qname=%.*s %s %s",
        hdr->id, hdr->flags & DNS_FLAGS_QR ? 1 : 0,
        opcode_tostr((hdr->flags & DNS_FLAGS_OPCODE) >> 11),
        is_rsp ? rcode_tostr(rcode) : "?", 
        msg->num_qd, msg->an_recs.rr_count,
        SLICE(ns->name),
        dns_class_tostr(q->qclass),
        dns_type_tostr(q->qtype));

    // check qr flag
    if (!is_rsp) {
        return log_error_rf("Unexpected msg ID: 0x%04x Flags: 0x%04x Len %zu",
           hdr->id, hdr->flags, q->pkt_len);
    }

    // check transaction id - TID
    if (hdr->id != q->tid) {
        return log_error_rf("rsp ID 0x%04x does not match query ID 0x%04x", 
            hdr->id, q->tid);
    }

    // check result code - RCODE
    if (rcode != DNS_RCODE_NOERROR) {
        return log_error_rc(rcode,
            "rsp for query id:0x%04x %s %s failed with error %s", 
            hdr->id, dns_class_tostr(q->qclass), dns_type_tostr(q->qtype), 
            rcode_tostr(rcode));
    }

    // validate question
    if (msg->num_qd != 1) return log_error_rf("recv-resp ID 0x%04x missing question", hdr->id);
    struct dns_quest *quest = &msg->qd_recs[0];
    struct str_slice name = ns->name;
    if (slice_endswith(name, '.')) name.len--;
    if (!slice_eqstri(name, quest->qname)) return log_error_rf("recv-resp ID 0x%04x qname mismatch", hdr->id);
    if (quest->qclass != q->qclass) return log_error_rf("recv-resp ID 0x%04x qclass mismatch", hdr->id);
    if (quest->qtype != q->qtype) return log_error_rf("recv-resp ID 0x%04x qqtype mismatch", hdr->id);

    // got okay resp
    return 0;
}

// setup request
static int set_dnsmsg(struct dns_ns *ns, struct dns_query *q)
{
    struct dns_msg *msg = &ns->msg;
    struct dns_hdr *hdr = &msg->hdr;
    struct str_slice name = ns->name;

    q->tid = gen_tid();
    dns_msg_init(msg, q->tid, DNS_FLAGS_RD);
    int rc = dns_add_qdn(msg, name.ptr, name.len, q->qtype, q->qclass);
    if (rc) return rc;

    log_debug("id:0x%04x qr:%d opcode:%s rd:%d qd:%zu qname=%.*s %s %s",
        hdr->id, hdr->flags & DNS_FLAGS_QR ? 1 : 0,
        opcode_tostr((hdr->flags & DNS_FLAGS_OPCODE) >> 11),
        hdr->flags & DNS_FLAGS_RD ? 1 : 0,
        msg->num_qd, SLICE(name),
        dns_class_tostr(q->qclass),
        dns_type_tostr(q->qtype));

    return 0;
}

static int ns_add_ans(struct dns_ns *ns)  
{
    struct dns_msg *msg = &ns->msg;
    struct dns_sect *ans = &msg->an_recs;

    // a query worked
    ns->have_ans = 1;

    for (size_t i = 0; i < ans->rr_count && !res_isfull(ns->res); i++) {
        struct dns_rr *rr =  &ans->rrs[i];
        switch(rr->type) {
        case DNS_TYPE_A:
            if (res_add_ip(ns->res, DNS_IPV4, rr->rdata.a)) ns->have_ip4 = 1;
            break;
        case DNS_TYPE_AAAA: 
            if (res_add_ip(ns->res, DNS_IPV6, rr->rdata.aaaa)) ns->have_ip6 = 1;
            break;
        }
    }

    log_debug("addr=%s type=%s ans=%d ip4=%d ip6=%d",
        dns_sockaddr_tostr(ns->addr), dns_socktype_tostr(ns->addr),
        ns->have_ans, ns->have_ip4, ns->have_ip6);

    return 0;
}

static int do_resp(struct dns_ns *ns, struct dns_query *q)
{
    int rc;

    if ((rc = dec_dnspkt(ns, q))) return rc;
    if ((rc = chk_dnsmsg(ns, q))) return rc;
    if ((rc = ns_add_ans(ns))) return rc;

    q->state = DNS_DONE;
    return 0;
}

static int do_send(struct dns_query *q)
{
   int rc = snd_dnspkt(q);
   if (rc != 0) return rc == DNS_EAGAIN ? 0 : rc;
   q->state = DNS_RECV;

   return 0;
}

static int do_recv(struct dns_ns *ns, struct dns_query *q)
{
    int rc = rcv_dnspkt(q);
    if (rc != 0) return rc == DNS_EAGAIN ? 0 : rc;
    return do_resp(ns, q);
}

static int do_query(struct dns_ns *ns, struct dns_query *q)
{
    int rc;

    if ((rc = set_dnsmsg(ns, q))) return rc;
    if ((rc = enc_dnspkt(ns, q))) return rc;
    if ((rc = do_send(q))) return rc;

    return 0;
}

static int do_conn(struct dns_ns *ns, struct dns_query *q)
{
    int rc = chk_connect(q);
    if (rc) return rc;

    q->state = DNS_SEND;
    return do_query(ns, q);
}

static int do_connect(struct dns_query *q, struct dns_sockaddr *addr)
{
    // socket
    int sock_type = addr->sock_type | SOCK_NONBLOCK | SOCK_CLOEXEC;
    q->fd = socket(addr->sa.sa_family, sock_type, 0);
    if (q->fd == -1) return log_errno_rf("create_socket(%u) failed", addr->sock_type);
    q->is_tcp = sock_type & SOCK_STREAM;

    // connect
    int next_state = DNS_SEND;
    int rc = connect(q->fd, &addr->sa, addr->len);
    if (rc == -1) {
        if (errno != EINPROGRESS) {
            query_close(q);
            return log_errno_rf("connect(%d) failed", addr->sock_type);
        }
        next_state = DNS_CONN;
    }

    q->state = next_state;

    return 0;
}

static void try_query(struct dns_ns *ns, uint16_t qtype)
{
    struct dns_query *q = get_query(ns, qtype);
    if (!q || q->state != DNS_SEND) return;

    int rc = do_query(ns, q);
    if (rc) {
        q->last_rc = rc;
        q->state = DNS_DONE;
        ns->active--;
    }
}

static void try_connect(struct dns_ns *ns, uint16_t qtype)
{
    struct dns_query *q = get_query(ns, qtype);
    if (!q || q->state != DNS_IDLE) return;

    q->qtype = qtype;
    q->qclass = DNS_CLASS_IN;

    int rc = do_connect(q, ns->addr);

    log_debug("rc=%d state=%d fd=%d addr=%s type=%s qclass=%s qtype=%s",
        rc, q->state, q->fd,
        dns_sockaddr_tostr(ns->addr), dns_socktype_tostr(ns->addr),
        dns_class_tostr(q->qclass), dns_type_tostr(q->qtype));

    if (rc) {
        q->last_rc = rc;
        q->state = DNS_DONE;
        return;
    }
    ns->active++;
}

static void close_all(struct dns_ns *ns)
{
    query_close(&ns->v4_query);
    query_close(&ns->v6_query);
}

static void resp_all(struct dns_ns *ns, struct pollfd fds[static 2])
{
    for (int i = 0; i < 2; i++) {
        if (fds[i].fd < 0) continue;
        uint16_t qtype = i == 0 ? DNS_TYPE_A : DNS_TYPE_AAAA;
        struct dns_query *q = get_query(ns, qtype);
        if (!q) continue;
        int rc = -1;
        switch(q->state) {
        case DNS_CONN: rc = do_conn(ns, q); break;
        case DNS_SEND: rc = do_send(q); break;
        case DNS_RECV: rc = do_recv(ns, q); break;
        }
        if (rc || q->state == DNS_DONE) {
            q->last_rc = rc;
            ns->active--;
            fds[i].fd = -1;
        }
        else { 
            int events = q->state == DNS_RECV ? POLLIN : POLLOUT;
            fds[i].events = events;
        }
    }
}

static void setup_fds(struct dns_ns *ns, struct pollfd fds[static 2])
{
    for (int i = 0; i < 2; i++) {
        uint16_t qtype = i == 0 ? DNS_TYPE_A : DNS_TYPE_AAAA;
        struct dns_query *q = get_query(ns, qtype);
        if (!q) {
            fds[i].fd = -1;
            continue;
        }
        int events;
        switch(q->state) {
        case DNS_IDLE: events = 0; break;
        case DNS_CONN: events = POLLOUT; break;
        case DNS_SEND: events = POLLOUT; break;
        case DNS_RECV: events = POLLIN;  break;
        case DNS_DONE: events = 0; break;
        default: events = 0;
        }

        fds[i].fd = q->fd;
        fds[i].events = events;
    }
}

static void query_all(struct dns_ns *ns)
{
    try_query(ns, DNS_TYPE_A);
    try_query(ns, DNS_TYPE_AAAA);
}

static void connect_all(struct dns_ns *ns)
{
    try_connect(ns, DNS_TYPE_A);
    try_connect(ns, DNS_TYPE_AAAA);
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
    default: rc = DNS_ERR;
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
    struct dns_sockaddr *addr, uint32_t timeout_secs,
    struct dns_result *res)
{
    struct dns_ns ns = {
        .name = name,
        .res =  res,
        .addr = addr,
        .timeout_ms = timeout_secs * 1000
    };

    log_debug("ns_addr:%s ip4=%d ip6=%d", dns_sockaddr_tostr(addr),
        need_ip4(res->flags), need_ip6(res->flags));

    connect_all(&ns);
    query_all(&ns);

    struct pollfd fds[2];
    setup_fds(&ns, fds);
    int64_t deadline_ms = get_deadline_ms(&ns);
    int rc = DNS_OK;

    while (ns.active) {
        int ms = get_rem_ms(deadline_ms);
        rc = poll(fds, ARR_LEN(fds), ms);
        if (rc <= 0) {
            if (rc == 0) rc = DNS_ETIMEOUT;
            if (errno == EINTR) continue;
            if (rc < 1) log_errno("poll failed");
            break;
        }
        resp_all(&ns, fds);
    }
    close_all(&ns);

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

static int query_name(struct str_slice name, struct dns_config *cfg, struct dns_result *res)
{
    int rc = DNS_ERR;

    log_debug("name:%.*s attempts:%d timeout:%u num_ns:%zu",
        SLICE(name), cfg->attempts, cfg->timeout_secs, cfg->num_ns);

    for (size_t a = 0; a < cfg->attempts; a++) {
        for (size_t n = 0; n < cfg->num_ns; n++) {
            struct dns_sockaddr *addr = &cfg->ns_addrs[n];
            rc = try_nameserver(name, addr, cfg->timeout_secs, res);
            if (stop_query(rc)) return rc;
        }
    }

    return rc;
}

static int query_search(struct str_slice name, struct dns_config *cfg, struct dns_result *res)
{
    int rc = DNS_NXDOMAIN;
    char tmp[DNS_MAXNAME];
    struct strbuf buf = STRBUF_INIT(tmp, sizeof(tmp));

    log_debug("name:%.*s nsearch:%zu", SLICE(name), cfg->num_search);

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

    log_debug("attempts:%u timeout:%u ndots:%u num_ns:%zu",
        cfg->attempts, cfg->timeout_secs, cfg->ndots, cfg->num_ns);

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
    if ((res->flags & (DNS_TCP | DNS_UDP)) == 0) {
        res->flags |= (DNS_TCP | DNS_UDP);
    }

    // empty port-str
    struct str_slice port = res->port;
    if (!port.len) return 0;

    // numeric port-str
    if (slice_isnumeric(port)) {
        uint32_t portno = slice_tou32(port);
        if (portno > 65535) return log_error_rf("port '%.*s' too big", SLICE(port));
        portno = __builtin_bswap16(portno);
        if (res->flags & DNS_TCP) res->tcp_port = portno;
        if (res->flags & DNS_UDP) res->udp_port = portno;
        return 0;
    }

    if (res->flags & DNS_NUMPORT) return log_error_rf("port '%.*s' not a number", SLICE(port));

    // services file
    return try_services(DNS_SERVICES, res->flags, port, res);
}

static int try_hostname(struct dns_result *res)
{
    // unspecified address (AF_UNSPEC)
    if ((res->flags & (DNS_IPV4 | DNS_IPV6)) == 0) {
        res->flags |= (DNS_IPV4 | DNS_IPV6);
    }

    struct str_slice host = res->host;

    // empty hostname
    if (!host.len) {
        if (res->flags & DNS_PASSIVE) {
            if (need_ip4(res->flags)) res_add_ip(res, DNS_IPV4, ip4_any);
            if (need_ip6(res->flags)) res_add_ip(res, DNS_IPV6, ip6_any);
        }
        else {
            if (need_ip4(res->flags)) res_add_ip(res, DNS_IPV4, ip4_loopback);
            if (need_ip6(res->flags)) res_add_ip(res, DNS_IPV6, ip6_loopback);
        }
        fixup_addrs(res);
        return res->num_addr ?: -1;
    }

    // decode ip-addr
    uint8_t ip_addr[16];
    uint32_t addr_type = ipstr_decode(host, ip_addr);
    if (!addr_type) return 0; // assume DNS name

    // check allowed
    uint32_t allow = res->flags & (DNS_IPV4 | DNS_IPV6 | DNS_V4MAPPED);
    if (allow & DNS_V4MAPPED) allow |= DNS_IPV4;
    if ((allow & addr_type)  == 0) {
        return log_error_rf("ip-addr type %d match %d failed for %.*s",
            addr_type, allow, SLICE(host));
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
    struct dns_sockaddr *addr = hosts_find(hosts, res->host);
    log_debug("lookup host=%.*s addr=%s", SLICE(res->host), dns_sockaddr_tostr(addr));
    if (!addr) return 0;

    // add hosts entry
    res_add_addr(res, addr);
    fixup_addrs(res);

    return res->num_addr ?: -1;
}

int dns_init(void)
{
    // resolv.conf
    struct dns_config *cfg = &glob_cfg;
    load_resolv_conf(DNS_RESOLV_CONF, cfg);

    // set defaults
    if (!cfg->attempts) cfg->attempts = DNS_ATTEMPTS;
    if (!cfg->timeout_secs) cfg->timeout_secs = DNS_TIMEOUT_SECS;
    if (!cfg->ndots) cfg->ndots = 1;
    if (!cfg->num_ns) {
        add_server(cfg, slice_make(STR_LIT("127.0.0.1")));
    }

    struct dns_hosts *hosts = &glob_hosts;
    load_hosts(DNS_HOSTS, hosts);

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

    char *buf = bufs[idx];
    size_t len = sizeof(bufs[0]);
    idx = (idx + 1) & 15;
    char *str = "<null>";
    if (!addr) return str;

    switch (addr->sa.sa_family) {
    case AF_INET: // a.b.c.d:port
        if (addr->len != sizeof(addr->v4)) break;
        str = buf;
        str += ip4_str_encode((void *) &addr->v4.sin_addr.s_addr, str, len); 
        *str++ = ':'; 
        str = uint16_toa(str, __builtin_bswap16(addr->v4.sin_port));
        str = '\0';
        break;
    case AF_INET6: // [::]:port
        if (addr->len != sizeof(addr->v6)) break;
        str = buf;
        str = buf;
        str += ip6_str_encode(addr->v6.sin6_addr.s6_addr, IP6_STR_ADDBRACK | IP6_STR_STRIPV4, str, len);
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
