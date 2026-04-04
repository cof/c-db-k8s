/*
 * DNS-RESOLV - a DNS resolver API
 * -------------------------------
 * See dns_resolv.h for API description.
 *
 * API sections
 * ------------
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>

#include "util.h"
#include "log.h"
#include "rwbuf.h"
#include "sock.h"
#include "dns_proto.h"
#include "dns_resolv.h"

// result state
struct dns_result {
    uint32_t flags;
    const char *hostname;
    const char *port;
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
    uint16_t qtype;
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
    int active;
    struct dns_sockaddr *addr;
    struct dns_query v4_query;
    struct dns_query v6_query;
    struct dns_msg msg; 
    uint32_t timeout;
    struct dns_result *res;
};

const uint8_t ip4_any[4] = { 0 };
const uint8_t ip4_loopback[4] = { 127, 0, 0, 1 };
const uint8_t ip6_any[16] = { 0 };
const uint8_t ip6_loopback[16] = { [15] = 1 };

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

static void res_add_ip(struct dns_result *res, int type, const void *ip)
{
    int ptype = res->flags & (DNS_TCP | DNS_UDP);

    if (ptype & DNS_TCP) {
        res_add_ip_port(res, type, ip, DNS_TCP, res->tcp_port);
    }

    if (ptype & DNS_UDP) {
        res_add_ip_port(res, type, ip, DNS_UDP, res->tcp_port);
    }
}

static void res_add_rec(struct dns_result *res, struct dns_rec *rec)
{
    switch(rec->type) {
    case DNS_TYPE_A:    return res_add_ip(res, DNS_IPV4, rec->rdata.a);
    case DNS_TYPE_AAAA: return res_add_ip(res, DNS_IPV6, rec->rdata.aaaa);
    }
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

static int try_services(char *file_path, int type, struct str_slice name, struct dns_result *res)
{
    int fd = open(file_path, O_RDONLY);
    if (fd == -1) return log_errno_rf("open %s failed", file_path);

    uint8_t tmp[1024];
    struct rwbuf buf = RWBUF_INIT(tmp, sizeof(tmp));

    int need_tcp = !type || type & DNS_TCP;
    int need_udp = !type || type & DNS_UDP;
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
            if (!slice_cmp(service, name)) continue;
            // found service name
            struct str_slice port = slice_copy(args);
            //struct str_slice alias = slice_split(&port, ' ');  
            slice_trim(&port);
            struct str_slice proto = slice_split(&port, '/');
            if (!slice_isnumeric(port)) continue; 
            uint16_t port_no = slice_tou32(port);
            if (slice_cmp_cstr(proto, STR_LIT("tcp")) && need_tcp) {
                res->tcp_port = port_no; 
                need_tcp = 0;
            }
            if (slice_cmp_cstr(proto, STR_LIT("udp")) && need_udp) {
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

// set ns_addr - return 1 if done else 0
static int set_ns_addr(struct dns_sockaddr *ns_addr, struct str_slice str)
{ 
    uint8_t ip[16];
    int type = ipstr_decode(str, ip);
    if (type == 0) return log_error_rc(0, "Invalid nameserver %.*s", SLICE(str));
    return sockaddr_store(ns_addr, type, ip, SOCK_DGRAM, __builtin_bswap16(53));
}

static int read_nameservers(char *file_path,
    int max_ns, struct dns_sockaddr ns_addrs[max_ns])
{
    if (max_ns <= 0) return 0;
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
            slice_trim(&line);
            if (line.len == 0 || *line.ptr == '#') continue;
            struct str_slice key = slice_copy(line);
            struct str_slice val = slice_split(&key, ' ');  
            slice_trim(&key);
            slice_trim(&val);
            if (!slice_cmp_cstr(key, STR_LIT("nameserver"))) continue;
            if (set_ns_addr(&ns_addrs[num_ns], val)) num_ns++;
            if (num_ns >= max_ns) break;
        }
        if (rc <= 0 || num_ns >= max_ns) break;
    }
   
    close(fd);
    if (rc < 0) return rc;

    return num_ns;
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
    q->pkt_len = 0;
}

static struct dns_query *get_query(struct dns_ns *ns, uint16_t qtype)
{
    struct dns_result *res = ns->res;

    switch(qtype) {
    case DNS_TYPE_A:
        if (res->flags & DNS_IPV4) return &ns->v4_query;
        if (res->flags & (DNS_IPV6 | DNS_V4MAPPED)) return &ns->v4_query;
        break;
    case DNS_TYPE_AAAA: 
        if (res->flags & DNS_IPV6) return &ns->v6_query;
        break;
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
    uint8_t *rptr = q->pkt_buf + q->pkt_off;
    ssize_t nread = read(q->fd, rptr, rlen);
    if (nread == -1) {
        // read failed
        if (errno == EINTR) goto retry;
        return q->is_tcp && (errno == EAGAIN || errno == EWOULDBLOCK)
            ? DNS_EAGAIN 
            : DNS_ERR;
    }
    if (nread == 0) return DNS_CLOSED;
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
    ssize_t nw = write(q->fd, wptr, wlen);
    if (nw == -1) {
        // write failed
        if (errno == EINTR) goto retry;
        return q->is_tcp && (errno == EAGAIN || errno == EWOULDBLOCK)
            ? DNS_EAGAIN
            : DNS_ERR;
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
    uint8_t *rbuf = q->pkt_buf;
    size_t rlen = q->pkt_len;

    // skip 2-byte prefix
    if (q->is_tcp) {
        rbuf += 2;
        rlen -= 2;
    }

    return dns_msg_decode(&ns->msg, rbuf, rlen);
}

static int enc_dnspkt(struct dns_ns *ns, struct dns_query *q)
{
    uint8_t *wbuf = q->pkt_buf;
    size_t wlen = sizeof(q->pkt_buf);

    // reserve space for TCP length prefix
    if (q->is_tcp) {
        wbuf += 2;
        wlen -= 2;
    }

    ssize_t pkt_len = dns_msg_encode(&ns->msg, wbuf, wlen);
    if (pkt_len <= 0) return log_error_rf("encode DNS pkt failed");

    // if tcp set 2-byte length prefix
    if (q->is_tcp) {
        enc_u16(q->pkt_buf, pkt_len);
        pkt_len += 2;
    }
    q->pkt_len = pkt_len;

    return 0;
}

// check msg is valid response
static int chk_dnsmsg(struct dns_ns *ns, struct dns_query *q)
{
    struct dns_msg *rsp = &ns->msg;
    struct dns_header *hdr = &rsp->hdr;

    if ((hdr->flags & DNS_FLAGS_QR) == 0) {
        return log_error_rf("Unexpected DNS message ID: 0x%04x Flags: 0x%04x Len %zu",
           hdr->id, hdr->flags, q->pkt_len);
    }

    // check Transaction ID
    if (hdr->id != q->tid) {
        return log_error_rf("Response ID 0x%04x does not match Request ID 0x%04x", 
            hdr->id, q->tid);
    }

    // check Result Code
    int rcode = hdr->flags & DNS_FLAGS_RCODE;
    if (rcode != DNS_RCODE_NOERROR) {
        return log_error_rf("Response ID 0x%04x failed with error %s", 
            hdr->id, rcode_tostr(rcode));
    }

    return 0;
}

// setup requst
static int set_dnsmsg(struct dns_ns *ns, struct dns_query *q)
{
    struct dns_result *res = ns->res;
    struct dns_msg *msg = &ns->msg;

    dns_msg_reset(msg);
    q->tid = rand() % 65536;
    dns_msg_set_id_flags(msg, q->tid, DNS_FLAGS_RD);
    int rc = dns_msg_add_qd(msg, res->hostname, q->qtype, DNS_CLASS_IN);
    if (rc) return rc;

    log_debug("Send query (%s) ID:0x%04x for %s %s %s",
        q->is_tcp ? "TCP" : "UDP",
        q->tid, res->hostname,
        dns_class_tostr(q->qtype),
        dns_type_tostr(DNS_CLASS_IN));

    return 0;
}

static int ns_add_ans(struct dns_ns *ns)  
{
    struct dns_msg *msg = &ns->msg;
    struct dns_sect *ans = &msg->an_recs;

    for (size_t i = 0; i < ans->num_rec && !res_isfull(ns->res); i++) {
        res_add_rec(ns->res, &ans->rec[i]);
    }

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

static int try_query(struct dns_ns *ns, uint16_t qtype)
{
    struct dns_query *q = get_query(ns, qtype);
    if (!q || q->state != DNS_SEND) return 0;

    int rc = do_query(ns, q);
    if (rc) q->state = DNS_DONE;

    return rc;
}

static int do_conn(struct dns_ns *ns, struct dns_query *q)
{
    int rc = chk_connect(q);
    if (rc) return rc;

    q->state = DNS_SEND;
    rc = do_query(ns, q);
    if (rc) q->state = DNS_DONE;

    return rc;
}

static int try_connect(struct dns_ns *ns, uint16_t qtype)
{
    struct dns_query *q = get_query(ns, qtype);
    if (!q || q->state != DNS_IDLE) return 0;

    // create non-blocking socket
    struct dns_sockaddr *addr = ns->addr;
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

    q->qtype = qtype;
    q->state = next_state;

    // all done
    return 1;
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
        int rc = -1;
        switch(q->state) {
        case DNS_CONN: rc = do_conn(ns, q); break;
        case DNS_SEND: rc = do_send(q); break;
        case DNS_RECV: rc = do_recv(ns, q); break;
        }
        if (rc || q->state == DNS_DONE) {
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
    if (try_query(ns, DNS_TYPE_A) != 0) ns->active--;
    if (try_query(ns, DNS_TYPE_AAAA) != 0) ns->active--;
}

static void connect_all(struct dns_ns *ns)
{
    if (try_connect(ns, DNS_TYPE_A) == 1) ns->active++;
    if (try_connect(ns, DNS_TYPE_AAAA) == 1) ns->active++;
}

static void try_nameserver(struct dns_ns *ns, struct dns_sockaddr *addr)
{
    ns->active = 0;
    ns->addr = addr;
    ns->timeout = DNS_TIMEOUT_MS;

    connect_all(ns);
    query_all(ns);

    struct pollfd fds[2];
    setup_fds(ns, fds);

    int64_t deadline_ms = get_now_ms() + ns->timeout;

    while (ns->active) {
        int64_t now_ms = get_now_ms();
        int remain_ms = (int) (deadline_ms - now_ms);
        int rc = poll(fds, ARR_LEN(fds), remain_ms);
        if (rc <= 0) {
            if (rc == 0) break; // timeout
            if (errno == EINTR) continue;
            break;
        }
        resp_all(ns, fds);
    }

    close_all(ns);
}

static int fixup_addrs(struct dns_result *res)
{
    int skip_v4 = res->flags & DNS_V4MAPPED && !(res->flags & DNS_ALL);

    if (res->num_ip6 > 0 && skip_v4) {
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

static int try_nameservers(struct dns_result *res)
{
    // fetch name servers
    struct dns_sockaddr ns_addrs[DNS_MAXNS];
    int num_ns = read_nameservers(DNS_RESOLV_CONF, ARRAY(ns_addrs));
    if (num_ns <= 0) return num_ns;

    struct dns_ns ns = {
        .res = res
    };

    // try them all
    for (int i = 0; i < num_ns; i++) {
        try_nameserver(&ns, &ns_addrs[i]);
    }

    fixup_addrs(res);

    return res->num_addr;
}

static int try_port(struct dns_result *res)
{
    // unspecified protocol 
    int type = res->flags & (DNS_TCP | DNS_UDP);
    if (!type) {
        type |= (DNS_TCP | DNS_UDP);
        res->flags |= type;
    }

    if (!res->port) return 0;

    struct str_slice port = slice_make_cstr(res->port);

    // empty port-name
    if (port.len == 0) return log_error_rf("port '%s' is empty", res->port);

    // numeric port-name
    if (slice_isnumeric(port)) {
        uint32_t portno = slice_tou32(port);
        if (portno > 65535) return log_error_rf("port '%s' too big", res->port);
        portno = __builtin_bswap16(portno);
        if (type & DNS_TCP) res->tcp_port = portno;
        if (type & DNS_UDP) res->udp_port = portno;
        return 0;
    }

    if (res->flags & DNS_NUMPORT) return log_error_rf("port '%s' not a number", res->port);

    // services file
    return try_services(DNS_SERVICES, type, port, res);
}


static int try_hostname(struct dns_result *res)
{
    // unspecified address type (AF_UNSPEC)
    if ((res->flags & (DNS_IPV4 | DNS_IPV6)) == 0) {
        res->flags |= (DNS_IPV4 | DNS_IPV6);
    }

    // set hostname
    if (!res->hostname) {
        uint32_t type = res->flags & (DNS_IPV4 | DNS_IPV6);
        if (res->flags & DNS_PASSIVE) {
            if (type & DNS_IPV6) res_add_ip(res, DNS_IPV6, ip6_any);
            if (type & DNS_IPV4) res_add_ip(res, DNS_IPV4, ip4_any);
        }
        else {
            if (type & DNS_IPV6) res_add_ip(res, DNS_IPV6, ip6_loopback);
            if (type & DNS_IPV4) res_add_ip(res, DNS_IPV4, ip4_loopback);
        }
        fixup_addrs(res);
        return res->num_addr ?: -1;
    }

    // decode ip-addr str
    uint8_t ip[16];
    struct str_slice host = slice_make_cstr(res->hostname);
    uint32_t addr_type = ipstr_decode(host, ip);
    if (!addr_type) return 0; // assume DNS name

    uint32_t allow = res->flags & (DNS_IPV4 | DNS_IPV6 | DNS_V4MAPPED);
    if (allow & DNS_V4MAPPED) allow |= DNS_IPV4;
    if ((allow & addr_type)  == 0) {
        return log_error_rf("ip-addr type %d match %d failed for %s",
            addr_type, allow, res->hostname);
    }
    res_add_ip(res, addr_type, ip);
    fixup_addrs(res);

    return res->num_addr ?: -1;
}

// resolve hostname,port to array of reolv_sockaddr - returns num-addr or error
int dns_resolv(uint32_t flags,
    const char *hostname, const char *port,
    int max_addr, struct dns_sockaddr addrs[max_addr])
{
    if (max_addr <= 0) return 0;

    struct dns_result res = {
        .flags    = flags,
        .hostname = hostname,
        .port     = port,
        .max_addr = max_addr,
        .addrs    = addrs
    };

    int rc;

    if ((rc = try_port(&res))) return rc;
    if ((rc = try_hostname(&res))) return rc;
    if ((rc = try_nameservers(&res))) return rc;

    // no match
    return 0;
}
