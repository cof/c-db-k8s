/*
 * RESOLV - a simple resolver API
 * ----------------------------------
 * See resolv.h for API description.
 *
 * API sections
 * ------------
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "util.h"
#include "log.h"
#include "rwbuf.h"
#include "sock.h"
#include "dns_proto.h"
#include "resolv.h"

struct simple_resolv {
    struct simple_sock sock;
    struct dns_msg msg; 
    uint32_t timeout;
    uint16_t tid_sent; // last tid sent
    const char *hostname;
    int num_addr;
    int max_addr;
    struct sock_addr *addrs;
    // packet buffer
    size_t recv_len;
    size_t pkt_len;
    uint8_t pkt_buf[DNS_MAX_PDUSIZE];
    unsigned int use_tcp  : 1; 
};


/*
static int map_file(const char *file_path, struct str_slice *str)
{
	int fd = open(file_path, O_RDONLY);
	if (fd < 0) return log_errno_rf("open %s failed", file_path);

	struct stat st;
	if (fstat(fd, &st) == -1) {
		close(fd);
		return log_ec_rf("fstat %d failed", fd);
	}

	char *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd); 
	if (map == MAP_FAILED) return log_errno_rf("mmap %s failed", file_path);

    *str = STRBU_INIT(map, st.st_size);

    return 0;
}

static int unmap_file(struct str_slice *map)
{
	if (munmap(map.ptr, map.len) != 0) {
	    return log_errno_rf("munmap %p %zu failed", map.ptr, map.len);
    }
    
    // clear ref
    map->ptr = NULL;
    map->len = 0;

    return 0;
}

static int read_nameservers(char *file_path,
    int max_ns, struct sock_addr ns_addrs[max_ns])
{
    struct str_slice map;
    int rc = map_file(file_path, &map);
    if (rc) return rc;

    struct rwbuf  buf = RWBUF_INIT(map.ptr, map.len);
    struct strbuf str = STRBUF_INIT(wbuf, wlen);
    struct str_slice line;
    int num_ns = 0;
    
    while ( (rc = rwbuf_readline(&buf, &line, 0, 0)) > 0) {
        slice_trim(&line);
        if (line.len == 0 || *line.ptr == '#') continue;
        struct str_slice key = slice_copy(key);
        struct str_slice val = slice_split(&key, ' ');  
        slice_trim(&key);
        slice_trim(&val);
        if (slice_cmp_cstr(key, STRLIT("nameserver")) && set_ns_addr(&ns_addrs[num_ns])) {
            num_ns++;
        }
    }

    rc = unmap_file(&map);
    if (rc) return rc;

    return ns;
}
*/

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

static int find_service(char *file_path, struct str_slice service, struct str_slice proto)
{
    int fd = open(file_path, O_RDONLY);
    if (fd == -1) return log_errno_rf("open %s failed", file_path);

    uint8_t tmp[1024];
    struct rwbuf buf = RWBUF_INIT(tmp, sizeof(tmp));
    int rc;
    int port_no = 0;

    while ((rc = read_block(fd, &buf)) >= 0) {
        struct str_slice line;
        int flags = rc == 0 ? RWBUF_EOF : 0;
        // name port/protocol alias
        while ( (rc = rwbuf_readline(&buf, &line, 0, flags)) > 0) {
            slice_chop(&line, '#');
            slice_trim(&line);
            if (line.len == 0) continue;
            struct str_slice name = slice_copy(line);
            struct str_slice args = slice_split(&name, ' ');  
            slice_trim(&name);
            if (!slice_cmp(name, service)) continue;
            // found service name
            struct str_slice port = slice_copy(args);
            //struct str_slice alias = slice_split(&port, ' ');  
            slice_trim(&port);
            struct str_slice proto_port = slice_split(&port, '/');
            if (!slice_cmp(proto_port, proto)) continue;
            if (!slice_isnumeric(port)) continue; 
            //found it
            port_no = (int) slice_tou32(port);
            break;
        }
        if (rc <= 0 || port_no) break;
    }

    close(fd);
    if (rc < 0) return rc;

    return port_no;
}

static int set_ns_addr(struct sock_addr *ns_addr, struct str_slice str)
{ 
    if (!sock_ipstr_decode(str, ns_addr)) {
       return log_error_rc(0, "Invalid nameserver value %.*s", SLICE(str));
    }

    ns_addr->port = __builtin_bswap16(53);

    return 1;
}

static int read_nameservers(char *file_path,
    int max_ns, struct sock_addr ns_addrs[max_ns])
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
            struct str_slice key = slice_copy(key);
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

static const char *get_recv_estr(int rc)
{
    switch(rc) {
    case SOCK_CLOSED:  return "closed";
    case SOCK_ERROR:   return "rejected/ignored";
    case SOCK_TIMEOUT: return "Timeout";
    case SOCK_DATA:    return "read/write failed";
    default:           return "Unknown";
    }
}

static int resolv_recv_dnspkt(struct simple_resolv *resolv)
{
    size_t read_len = sizeof(resolv->pkt_buf);
    ssize_t rc;

    if (resolv->use_tcp) {
        // read 2-byte prefix
        uint16_t dns_len;
        rc = sock_read_data(&resolv->sock, &dns_len, sizeof(dns_len));
        if (rc != sizeof(dns_len)) return log_error_rf("sock_read failed %s", get_recv_estr(rc));
        read_len = ntohs(dns_len);
    }

    // read pkt
    rc = sock_read_data(&resolv->sock, resolv->pkt_buf, read_len);
    if (rc <= 0) return log_error_rf("sock_read failed %s", get_recv_estr(rc));

    // record pkt len
    resolv->pkt_len = rc;

    // read a message
    return 0;
}

// send dns packet to server
static int resolv_send_dnspkt(struct simple_resolv *resolv)
{
    uint16_t dns_len;
    struct iovec iovs[2];
    int num_iov = 0;

    // add 2-byte length prefix if TCP
    if (resolv->use_tcp) {
        dns_len = ntohs(resolv->pkt_len);
        iov_load(iovs + 0, &dns_len, sizeof(dns_len));
        num_iov++;
    }

    // add the encoded packet
    iov_load(iovs + num_iov, resolv->pkt_buf, resolv->pkt_len);
    num_iov++;

    // send pdu
    ssize_t rc = sock_write_iovs(&resolv->sock, num_iov, iovs);
    if (rc >= 0) rc = 0;

    return rc;
}

static int resolv_dec_dnspkt(struct simple_resolv *resolv)
{
    struct dns_msg *msg = &resolv->msg;

    dns_msg_reset(msg);
    return dns_msg_decode(msg, resolv->pkt_buf, resolv->pkt_len);
}

static int resolv_enc_dnspkt(struct simple_resolv *resolv)
{
    void *buf = resolv->pkt_buf;
    size_t space = sizeof(resolv->pkt_buf);

    ssize_t pkt_len = dns_msg_encode(&resolv->msg, buf, space);
    if (pkt_len <= 0) return log_error_rf("encode DNS pkt failed");

    resolv->pkt_len = pkt_len;

    return 0;
}

static int resolv_add_ans(struct simple_resolv *resolv)  
{
    if (resolv->num_addr >= resolv->max_addr) return 0;

    struct dns_msg *msg = &resolv->msg;
    struct dns_sect *sect = &msg->an_recs;

    for (size_t i = 0; i < sect->num_rec; i++) {
        struct sock_addr *addr = &resolv->addrs[resolv->num_addr];
        struct dns_rec *rec = &sect->rec[i];
        switch(rec->type) {
        case DNS_TYPE_A: 
             addr->u32[0] = *(uint32_t *) rec->rdata.a;
             break;
        case DNS_TYPE_AAAA:
             memcpy(addr->v6, rec->rdata.aaaa, 16);
             break;
        default: 
            continue;
        }
        resolv->num_addr++;
        if (resolv->num_addr >= resolv->max_addr) break;
    }

    return 0;
}

static int resolv_chk_rsp(struct simple_resolv *resolv)
{
    // check msg is valid response
    struct dns_msg *rsp = &resolv->msg;
    struct dns_header *hdr = &rsp->hdr;

    if ((hdr->flags & DNS_FLAGS_QR) == 0) {
        if (log_level) {
            log_info("dns-gen", 
                "Unexpected DNS message ID: 0x%04x Flags: 0x%04x Len %zu",
                hdr->id, hdr->flags, resolv->recv_len);
        }
        return -1;
    }

    // check Transaction ID
    if (hdr->id != resolv->tid_sent) {
        if (log_level) {
            log_info("dns-gen",
                "Response ID 0x%04x does not match Request ID 0x%04x",
                hdr->id, resolv->tid_sent);
        }
        return -1;
    }

    // check Result Code
    int rcode = hdr->flags & DNS_FLAGS_RCODE;
    if (rcode != DNS_RCODE_NOERROR) {
        if (log_level) {
            log_info("dng-gen", 
            "Response ID 0x%04x failed with error %s", 
            hdr->id, rcode_tostr(rcode));
        }
        return -1;
    }

    return 0;
}

// set question fields for query
static int resolv_set_query(struct simple_resolv *resolv)
{
    struct dns_msg *msg = &resolv->msg;

    dns_msg_reset(msg);
    uint16_t tid = rand() % 65536;
    dns_msg_set_id_flags(msg, tid, DNS_FLAGS_RD);
    int rc = dns_msg_add_qd(msg, resolv->hostname, DNS_TYPE_A, DNS_CLASS_IN);
    if (rc) return rc;

    if (log_level) {
        log_info(RESOLV, 
            "Send query (%s) ID:0x%04x for %s %s %s",
            resolv->use_tcp ? "TCP" : "UDP",
            tid, resolv->hostname,
            dns_class_tostr(DNS_TYPE_A),
            dns_type_tostr(DNS_CLASS_IN));
    }

    return 0;
}

// recv query rsp from server
static int resolv_recv_resp(struct simple_resolv *resolv)
{
    int rc;

    if ((rc = resolv_recv_dnspkt(resolv))) return rc;
    if ((rc = resolv_dec_dnspkt(resolv))) return rc;
    if ((rc = resolv_chk_rsp(resolv))) return rc;
    if ((rc = resolv_add_ans(resolv))) return rc;

    // all done
    return 0;
}

// send query msg to server
static int resolv_send_query(struct simple_resolv *resolv)
{
    int rc;

    if ((rc = resolv_set_query(resolv))) return rc;
    if ((rc = resolv_enc_dnspkt(resolv))) return rc;
    if ((rc = resolv_send_dnspkt(resolv))) return rc;

    // sent - as far as we know
    //resolv->tid_sent = tid;

    // all done
    return 0;
}

static int resolv_connect(struct simple_resolv *resolv, struct sock_addr *serv)
{
    uint32_t mode = SOCK_UDP | SOCK_UDPCON;

    int rc;
    if ((rc = sock_connect(&resolv->sock, mode, serv))) return rc;
    if ((rc = sock_set_sndto(&resolv->sock, resolv->timeout))) return rc;
    if ((rc = sock_set_rcvto(&resolv->sock, resolv->timeout))) return rc;

    return 0;
}

static int resolv_query(struct simple_resolv *resolv, struct sock_addr *serv)
{
    int rc;
    if ((rc = resolv_connect(resolv, serv))) return rc;
    if ((rc = resolv_send_query(resolv))) return rc;
    if ((rc = resolv_recv_resp(resolv))) return rc;

    return 0;
}

static int try_nameservers(const char *hostname, 
    int max_addr, struct sock_addr addrs[max_addr])
{
    // fetch name-server list
    struct sock_addr ns_addrs[RESOLV_MAXNS];
    int num_ns = read_nameservers(RESOLV_CONF, ARRAY(ns_addrs));
    if (num_ns <= 0) return num_ns;

    struct simple_resolv resolv = {
        .hostname = hostname,
        .num_addr = 0,
        .max_addr = max_addr,
        .addrs   =  addrs
    };

    // try them all
    for (int i = 0; i < num_ns; i++) {
        int rc = resolv_query(&resolv, &ns_addrs[i]);
        if (rc) return rc;
    }

    return resolv.num_addr;
}

static int resolv_port(const char *port, uint32_t mode, struct sock_addr *addr)
{
    struct str_slice service = slice_make_cstr(port);

    // check numeric
    if (mode & SOCK_NUMSERV) {
        if (!slice_isnumeric(service)) {
            return log_error_rf("resolv-port %s to mode %d failed", port, mode & SOCK_NUMSERV);
        }
        return slice_tou32(service);
    }

    // check-type
    int type = mode & (SOCK_TCP | SOCK_UDP);
    if (type == 0 || type & (SOCK_TCP | SOCK_UDP)) {
        return log_error_rf("resolv-port mode bad-type %d", type);
    }
    struct str_slice proto = type & SOCK_TCP 
        ? slice_make(STR_LIT("tcp"))
        : slice_make(STR_LIT("udp"));

    // try services file
    int rc = find_service(RESOLV_SERVICES, service, proto);
    if (rc < 0) return rc;
    if (rc == 0) return log_error_rf("resolv-port %s to numeric failed", port);

    addr->port = __builtin_bswap16(rc);

    // resolved
    return 0;
}

static int try_hostname(const char *hostname, uint32_t mode, struct sock_addr *addr)
{
    int domain = mode & (SOCK_IPV4 | SOCK_IPV6);

    // set hostname
    if (!hostname) {
        switch(domain) {
        case SOCK_IPV4: hostname = mode & SOCK_PASSIVE ? "0.0.0.0" : "127.0.0.1"; break;
        case SOCK_IPV6: hostname = mode & SOCK_PASSIVE ? "::" : "::1"; break;
        default:
            log_debug("%s need-hostname for domain 0x%x", __func__, domain);
            return -1;
        }
    }

    // decode ip-addr
    struct str_slice host = slice_make_cstr(hostname);
    int addr_type = sock_ipstr_decode(host, addr);
    if (addr_type == 0) return 0; 

    // domain match
    if (addr_type != domain) {
        log_debug("type %d domain-match %d failed for %s", addr_type, domain, hostname);
        return 0;
    }

    // match
    return 1;
}

// joker checks
static int mode_check(uint32_t mode)
{
    int type = mode & (SOCK_TCP | SOCK_UDP);
    if (type == 0 || type & (SOCK_TCP | SOCK_UDP)) {
        if (log_level) log_info(RESOLV, "mode-check bad-type 0x%x", type);
        return -1;
    }

    int domain = mode & (SOCK_ANY | SOCK_IPV4 | SOCK_IPV6);
    if (domain == 0 || domain & (SOCK_ANY | SOCK_IPV4 | SOCK_IPV6)) {
        if (log_level) log_info(RESOLV, "mode-check bad-domain 0x%x", domain);
        return -1;
    }

    return 0;
}

// resolve hostname,port to sock_addr - returns num-addr or error
int resolv_addr(const char *hostname, const char *port, uint32_t mode, 
    int max_addr, struct sock_addr addrs[max_addr])
{
    if (max_addr <= 0) return 0;

    int rc;
    if ((rc = mode_check(mode))) return rc;
    if ((rc = resolv_port(port, mode, addrs))) return rc;
    if ((rc = try_hostname(hostname, mode, addrs))) return rc;
    if ((rc = try_nameservers(hostname, max_addr, addrs))) return rc;

    // no match
    return 0;
}
