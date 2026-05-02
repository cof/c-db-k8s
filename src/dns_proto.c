/*
 * A DNS message codec API
 * -----------------------
 * See dns_proto.h for full API description.
 *
 * Basic API
 * ----------
 * dns_hdr_decode(hdr, buf, len) : decode buffer into dns header
 * dns_msg_reset(msg) : reset fields to 0
 * dns_msg_decode(msg, buf, len) : decode buffer into DNS message
 * dns_msg_encode(msg, buf, len) : encode DNS message into buffer
 * dns_validate(buf, len, emsg, emsg_len) : check pkt valid and print desc to esmg
 *
 * References
 * ----------
 * rfc1035 - DOMAIN NAMES - IMPLEMENTATION AND SPECIFICATION
 * rfc6891 - Extension Mechanisms for DNS (EDNS(0))
 */
#include <errno.h>
#include <arpa/inet.h>
#include <arpa/inet.h>
#include <stdarg.h>

#include "util.h"
#include "log.h"
#include "dns_proto.h"

// flag decoder error
#define DEC_ERR -1

// error reporting
#define DNS_MAX_EMSG  10
#define DNS_NAME_SIZE 256
#define DNS_MSG_SIZE (4 * DNS_NAME_SIZE + 100) // big enough for names + extra

// error state
struct dns_err {
    int group;
    int field;
    int ec;
};

// dns decoder state
struct dns_dec {
    const uint8_t *pkt_buf;
    size_t pkt_len;
    size_t consumed;
    size_t offset;
    // a simple error stack
    struct dns_err errs[DNS_MAX_EMSG];
    size_t nerr;
    // flags
    unsigned int need_emsg : 1;
    unsigned int load_msg : 1;
    unsigned int got_edns : 1;
    unsigned int dnssec_ok : 1;
    // EDNS0 Options (RFC 6891)
    size_t udp_size;
    uint8_t ext_rcode;
    uint8_t edns_ver;
    struct strbuf emsg; // track what we write into emsg
    struct dns_hdr hdr;
    char msg[DNS_MSG_SIZE]; // decode_name + dns_err_tostr
};

// print a msg to decoder state
static int dns_wmsg(struct dns_dec *dec, const char *fmt, ...) \
    __attribute__((format(printf, 2, 3)));

// decoders

// XXX - use xmacros to ensure both code and string match
#define DNS_ERRORS(X) \
    X(DNS_EOK, "Okay") \
    X(DNS_EHDRLEN, "header len") \
    X(DNS_EBADJMP, "Invalid compression pointer (outside range)") \
    X(DNS_EMAXJMP, "Invalid compression pointer (loop detected)") \
    X(DNS_ENAMELEN, "Name length bigger than pkt size") \
    X(DNS_EOUTLEN, "Name bigger than buf size") \
    X(DNS_ENONULL, "Name missing null char") \
    X(DNS_ETRUNC,  "Truncated packet") \
    X(DNS_ERDATALEN, "RDATA len mismatch") \
    X(DNS_EMSGFMT ,  "field not allowed") \
    X(DNS_ESPACE,    "No space in buffer") \
    X(DNS_EOPCODE,   "Invalid opcode") \
    X(DNS_ERCODE,    "Invalid rcode")


enum dns_dec_error {
    #define X(name, text) name,
    DNS_ERRORS(X)
    #undef X
};

static const char *dns_ec_strs[] = {
    #define X(name, text) [name] = text,
    DNS_ERRORS(X)
    #undef X
};

#define DNS_EC_ISROW 10000

static const char *dns_ec_tostr(int ec)
{
    if (ec < 0) ec = -ec;
    const char *str = ec_tostr(ARRAY(dns_ec_strs), ec, NULL);
    if (str) return str;

    if (ec >= DNS_EC_ISROW) {
        // its section row number
        ec -= DNS_EC_ISROW;
        ec++;
    }

    return int_tostr(ec);
}

// decode error locations - using xmacros
#define DNS_LOCATIONS(X) \
    X(DNS_NONE, "NONE") \
    X(DNS_PDU,  "PDU") \
    X(DNS_HDR, "HDR") \
    X(DNS_QD,  "Question") \
    X(DNS_AN,  "Answer") \
    X(DNS_NS,  "Authority") \
    X(DNS_AR,  "Additional") \
    X(DNS_RR,  "Record") \
    X(DNS_RDATA, "RDATA") \
    X(DNS_NAME,  "Name") \
    X(DNS_A,     "A") \
    X(DNS_TNS,   "NS") \
    X(DNS_CNAME, "CNAME") \
    X(DNS_SOA,   "SOA") \
    X(DNS_PTR,   "PTR") \
    X(DNS_HINFO, "HINFO")\
    X(DNS_MX,    "MX") \
    X(DNS_TXT,   "TXT") \
    X(DNS_AAAA,  "AAAA") \
    X(DNS_SRV,   "SRV") \
    X(DNS_OPT,   "OPT") \
    X(DNS_ANY,   "ANY")

#define LOCATION_NAME(NAME, TEXT) NAME,
#define LOCATION_TEXT(NAME, TEXT) [NAME] = TEXT,

enum dns_location {
    DNS_LOCATIONS(LOCATION_NAME)
};

static const char *dec_code_strs[] = {
    DNS_LOCATIONS(LOCATION_TEXT)
};

static const char *dec_code_tostr(int code)
{
    const char *str = ec_tostr(ARRAY(dec_code_strs), code, NULL);
    if (str) return str;

    return int_tostr(code);
}

const char *dns_class_tostr(int code)
{
    if (code == 1) return "IN";
    if (code == 3) return "CH";
    if (code == 4) return "HS";
    if (code == 254) return "NONE";
    if (code == 255) return "*";

    return int_tostr(code);
};

const char *dns_type_tostr(int code)
{
    if (code == 1)  return "A";
    if (code == 2)  return "NS";
    if (code == 5)  return "CNAME";
    if (code == 6)  return "SOA";
    if (code == 12) return "PTR";
    if (code == 15) return "MX";
    if (code == 16) return "TXT";
    if (code == 28) return "AAAA";
    if (code == 33) return "SRV";
    if (code == 41) return "OPT";
    if (code == 252) return "AXFR";
    if (code == 255)  return "ANY";

    return int_tostr(code);
}

static const char *opcode_strs[] = {
    [DNS_OPCODE_QUERY]  = "QUERY",
    [DNS_OPCODE_IQUERY] = "IQUERY",
    [DNS_OPCODE_STATUS] = "STATUS",
    [DNS_OPCODE_NOTIFY] = "NOTIFY",
    [DNS_OPCODE_UPDATE ] = "UPDATE"
};

static const char *rcode_strs[] = {
    [DNS_RCODE_NOERROR]  = "NoError",
    [DNS_RCODE_FORMERR]  = "FormErr",
    [DNS_RCODE_SERVFAIL] = "ServFail",
    [DNS_RCODE_NXDOMAIN] = "NXDomain",
    [DNS_RCODE_NOTIMP]   = "NotImp",
    [DNS_RCODE_REFUSED]  = "Refused",
    [6] = "YXDomain",
    [7] = "YXRRSet",
    [8] = "NXRRSet",
    [9] = "NotAuth",
    [10] = "NotZone",
    // 11 - 15 Available for assignment
    [16] = "BADSIG",
    [17] = "BADKEYA",
    [18] = "BADTIME",
    [19] = "BADMODE",
    [20] = "BADNAMEA",
    [21] = "BADALGA",
    [22] = "BADTRUC"
};

const char *rcode_tostr(int rcode)
{
    const char *str = ec_tostr(ARRAY(rcode_strs), rcode, NULL);
    if (str) return str;

    return int_tostr(rcode);
}

const char *opcode_tostr(int opcode)
{
    const char *str = ec_tostr(ARRAY(opcode_strs), opcode, NULL);
    if (str) return str;

    return int_tostr(opcode);
}

static int dns_add_str(char *buf, size_t len, const char *str, size_t slen)
{
    if (slen + 1 > len) {
        return log_error_rf("slen %zu too big for len %zu", slen, len);
    }

    memcpy(buf, str, slen);
    buf[slen] = '\0';

    return slen;
}

// append a fmt message to emsg buffer
static int dns_wmsg(struct dns_dec *dec, const char *fmt, ...)
{
    struct strbuf *buf = &dec->emsg;
    size_t avail = strbuf_rem(buf);
    char *ptr = strbuf_ptr(buf);

    va_list args;
    va_start(args, fmt);
    int nw = vsnprintf(ptr, avail, fmt, args);
    va_end(args);

    if (nw < 0) return log_errno_rf("dns_wnsg: wwriter failed");
    if ((size_t) nw >= avail) return log_error_rf("dns_wnsg: no space");

    buf->ptr += nw;

    // all done
    return 0;
}

// log ad dns error
static int dns_dec_err(struct dns_dec *dec, int group, int field, int ec)
{
    if (dec->nerr >= ARR_LEN(dec->errs)) {
        return log_error_rc(DEC_ERR, "No room for err  %d %d %d", group, field, ec);
    }

    dec->errs[dec->nerr].group = group;
    dec->errs[dec->nerr].field = field;
    dec->errs[dec->nerr].ec = ec;
    dec->nerr++;

    // all done
    return 0;
}

// convert dns err to string
char *dns_err_tostr(struct dns_dec *dec, struct dns_err *err)
{
    const char *group = dec_code_tostr(err->group);
    const char *field = dec_code_tostr(err->field);
    const char *error = dns_ec_tostr(err->ec);

    int avail = sizeof(dec->msg);
    int nw = snprintf(dec->msg, avail, "%s %s %s", group, field, error);
    if (nw < 0) return log_errno_rn("dns_err_tostr snprintf failed");
    if (nw >= avail) return log_error_rn("dns_err_tostr sprintf no room");

    // all done
    return dec->msg;
}

// convert decode error list to str
static int dns_dec_genmsg(struct dns_dec *dec)
{
    if (!dec->nerr) {
        if (!strbuf_rem(&dec->emsg)) {
            // decoders desc failed ?
            dns_wmsg(dec, "[ERROR] Missing PDU desc");
        }
        // done
        return 0;
    }

    // desribe error
    dns_wmsg(dec, "[ERROR] ");
    if (dec->pkt_len >= DNS_HDR_LEN) {
        // have a hdr
        dns_wmsg(dec, "ID 0x%04x ",  dec->hdr.id);
    }

    // convert the error chain to desc
    for (int i = dec->nerr - 1; i >=  0; i--) {
        char *estr = dns_err_tostr(dec, &dec->errs[i]);
        dns_wmsg(dec, "/ %s", estr);
    }

    dns_wmsg(dec, "\n");

    // report ERROR
    return DEC_ERR;
}

// decode 12-byte dns header
int dns_hdr_decode(struct dns_hdr *hdr, const uint8_t *buf, size_t len)
{
    // size check
    if (len < DNS_HDR_LEN) return DNS_EHDRLEN;

    // decode hdr
    hdr->id       = dec_u16(buf + 0);
    hdr->flags    = dec_u16(buf + 2);
    hdr->qd_count = dec_u16(buf + 4);
    hdr->an_count = dec_u16(buf + 6);
    hdr->ns_count = dec_u16(buf + 8);
    hdr->ar_count = dec_u16(buf + 10);

    // all done
    return 0;
}

// decode dns name from pkt buf - return bytes written or error
int decode_name(struct dns_dec *dec, char *name, size_t nlen)
{
    size_t pkt_idx = dec->offset;
    size_t out_len = nlen;
    int njmp = 0, len = -1;

    while (pkt_idx < dec->pkt_len) {
        // get label length
        len = dec->pkt_buf[pkt_idx++];
        // compression pointer - rfc1035 - 4.1.4. Message compression
        if ((len & DNS_COMP_PTR) == DNS_COMP_PTR) {
            if (pkt_idx == dec->pkt_len)  return -DNS_EBADJMP;
            if (njmp++ > DNS_MAX_JMP) return -DNS_EMAXJMP;
            if (njmp == 1) dec->offset += 2;
            // convert to jmp position
            len = ((len & 0x3F) << 8) | dec->pkt_buf[pkt_idx];
            if (len < 12)      return -DNS_EBADJMP;
            if ((size_t) len > dec->pkt_len) return -DNS_EBADJMP;
            // jmp to position
            pkt_idx = len;
            continue;
        }

        // label len (0-63)
        if ((size_t) len > dec->pkt_len - pkt_idx) return -DNS_ENAMELEN;
        if ((size_t) len > out_len) return -DNS_EOUTLEN;
        if (!njmp) dec->offset += 1 + len;

        // null check
        if (len == 0) break;

        // copy label
        if (name) {
            memcpy(name, dec->pkt_buf + pkt_idx, len);
            name += len;
        }
        out_len -= len;
        pkt_idx += len;

        // add a dot
        if (!out_len) return -DNS_EOUTLEN;
        if (pkt_idx < dec->pkt_len && dec->pkt_buf[pkt_idx]) {
            // store the dot
            if (name) *name++ = '.';
            out_len--;
        }
    }

    // did we stop at 0
    if (len != 0) return -DNS_ENONULL;

    // null-terminate
    if (!out_len) return -DNS_EOUTLEN;
    if (name) {
        *name++ = '\0';
        out_len--;
    }

    // num bytes writen
    return nlen - out_len;
}

struct dns_sect {
    struct dns_rr *base;
    uint16_t *count;
    uint16_t max;
};

static void sect_init(struct dns_sect *sect,
    struct dns_rr *base, uint16_t max,
    uint16_t *count)
{
    sect->base = base;
    sect->max  = max;
    sect->count = count;
}

// print dns_qd to str - returns bytes written or error
int dns_qd_tostr(struct dns_qd *quest, char *buf, size_t buf_len)
{
    // ensure no hidden fields
    const char *name = str_def(quest->qname, "<null>");
    const char *type_str = dns_type_tostr(quest->qtype);
    const char *class_str = dns_class_tostr(quest->qclass);

    char *wptr = buf;
    char *wend = wptr + buf_len;

    int rc = snprintf(wptr, wend - wptr, "%s %s %s ", name, class_str, type_str);
    if (rc < 0) return log_errno_rf("snprintf failed");
    wptr += rc;

    // bytes written
    return wptr - buf;
}

// print dns_rr to string - return bytes written or error
int dns_rr_tostr(struct dns_rr *rr, char *mem, size_t len)
{
    if (!rr) return 0;

    struct strbuf tmp;
    struct strbuf *buf = strbuf_init(&tmp, mem, len);

    // name,class,type
    strbuf_putm(buf, STR_LIT("  "));
    strbuf_puts(buf, str_def(rr->name, "<null>"));
    strbuf_putcs(buf, ' ', dns_class_tostr(rr->class));
    strbuf_putcs(buf, ' ', dns_type_tostr(rr->type));
    strbuf_putm(buf, STR_LIT(" "));

    // rdata
    switch(rr->type) {
    case DNS_TYPE_A: // IP4 address
        len = ip4_str_encode(rr->rdata.a, strbuf_ptr(buf), strbuf_rem(buf));
        strbuf_mksp(buf, len);
        break;
    case DNS_TYPE_NS:
        strbuf_puts(buf, rr->rdata.ns_name);
        break;
    case DNS_TYPE_CNAME:
        strbuf_puts(buf, rr->rdata.cname);
        break;
    case DNS_TYPE_SOA:
        // name + name + 5 integers
        strbuf_putm(buf, STR_LIT("MNAME="));
        strbuf_puts(buf, rr->rdata.soa.mname);
        strbuf_putcm(buf, ' ', STR_LIT("RNAME="));
        strbuf_puts(buf, rr->rdata.soa.rname);
        strbuf_putcn(buf, ' ', rr->rdata.soa.serial);
        strbuf_putcn(buf, ' ', rr->rdata.soa.refresh);
        strbuf_putcn(buf, ' ', rr->rdata.soa.retry);
        strbuf_putcn(buf, ' ', rr->rdata.soa.expire);
        strbuf_putcn(buf, ' ', rr->rdata.soa.min_ttl);
        break;
    case DNS_TYPE_PTR:
        strbuf_puts(buf, rr->rdata.ptr_name);
        break;
    case DNS_TYPE_HINFO:
        strbuf_putm(buf, STR_LIT("cpu="));
        strbuf_puts(buf, rr->rdata.hinfo.cpu_str);
        strbuf_putcm(buf,' ', STR_LIT("os="));
        strbuf_puts(buf, rr->rdata.hinfo.os_str);
        break;
    case DNS_TYPE_MX: // Mail Exchange
        strbuf_putm(buf, STR_LIT("Pref="));
        strbuf_putn(buf, rr->rdata.mx.pref);
        strbuf_putcs(buf, ' ', rr->rdata.mx.name);
        break;
    case DNS_TYPE_TXT:
        for (int i = 0; i < rr->rdata.txt.num_str; i++) {
            char *str = rr->rdata.txt.str[i];
            strbuf_putm(buf, STR_LIT(" txt="));
            strbuf_puts(buf, str);
        }
        break;
    case DNS_TYPE_AAAA:// IPv6 Address
        len = ip6_str_encode(rr->rdata.aaaa, 0, strbuf_ptr(buf), strbuf_rem(buf));
        strbuf_mksp(buf, len);
        break;
    case DNS_TYPE_SRV:
        strbuf_putm(buf, STR_LIT("Priority"));
        strbuf_putcn(buf, ' ', rr->rdata.srv.prior);
        strbuf_putcm(buf, ' ', STR_LIT("Weight"));
        strbuf_putcn(buf, ' ', rr->rdata.srv.weight);
        strbuf_putcm(buf, ' ', STR_LIT("Port"));
        strbuf_putcn(buf, ' ', rr->rdata.srv.port);
        strbuf_putcm(buf, ' ', STR_LIT("Srv"));
        strbuf_putcs(buf, ' ', rr->rdata.srv.name);
        break;
    case DNS_TYPE_OPT: // EDNS0
        strbuf_putm(buf, STR_LIT("udp-size:"));
        strbuf_putcn(buf, ' ', rr->rdata.opt.udp_size);
        strbuf_putcm(buf, ' ', STR_LIT("Ext-RCODE:"));
        strbuf_putcn(buf, ' ', rr->rdata.opt.ext_rcode);
        strbuf_putcm(buf, ' ', STR_LIT("EDNS0:"));
        strbuf_putcn(buf, ' ', rr->rdata.opt.edns_ver);
        strbuf_putcm(buf, ' ', STR_LIT("DNSEC-OK:"));
        strbuf_putcn(buf, ' ', rr->rdata.opt.do_bit);
        break;
    default:
        break;
    }

    strbuf_endz(buf);

    // bytes written
    return strbuf_pos(buf);
}

// add dns section as string to buffer
static int dns_sect_tostr(struct dns_sect *sect, char *buf, size_t buf_len)
{
    size_t nw = 0;

    for (int i = 0; i < *sect->count; i++) {
        if (i > 0) {
            if (nw == buf_len) return DNS_FAIL;
            buf[nw++] = '\n';
        }
        int rc = dns_rr_tostr(&sect->base[i], buf + nw, buf_len - nw);
        if (rc < 0) return rc;
        nw += rc;
    }

    return nw;
}

// add all dns sections as string to buffer
int dns_sects_tostr(struct dns_msg *msg, char *buf, size_t len)
{
    struct dns_sect sect;
    int rc, nw = 0;

    sect_init(&sect, msg->an, ARR_LEN(msg->an), &msg->an_len);
    rc = dns_sect_tostr(&sect, buf, len);
    if (rc < 0) return rc;
    nw += nw;

    sect_init(&sect, msg->ns, ARR_LEN(msg->ns), &msg->ns_len);
    rc = dns_sect_tostr(&sect, buf, len);
    if (rc < 0) return rc;
    nw += nw;

    sect_init(&sect, msg->ar, ARR_LEN(msg->ar), &msg->ar_len);
    rc = dns_sect_tostr(&sect, buf, len);
    if (rc < 0) return rc;
    nw += nw;

    return nw;
}

// add name to msg store
static char *msg_store_str(struct dns_msg *msg, const char *str, size_t len)
{
    if (len > DNS_NAME_MAXSTR) return NULL;

    // space for name + nul ?
    if (msg->names_len + len + 1 > sizeof(msg->names)) {
        errno = ENOBUFS;
        return NULL;
    }

    // add str to names buffer
    char *store = msg->names + msg->names_len;
    memcpy(store, str, len);
    store[len] = '\0';
    msg->names_len += len + 1;

    return store;
}

static char *msg_store_name(struct dns_msg *msg, const char *name)
{
    return msg_store_str(msg, name, safe_strlen(name));
}

// fix label - add root if needed
static int fix_label(int rc, char *wptr, char *wend)
{
    if (rc != 1) {
        // return no-null or error
        if (rc > 0) rc--;
        return rc;
    }

    return dns_add_str(wptr, wend - wptr, STR_LIT(DNS_NULL_STR));
}

// decode a DNS resource record (RR) into a section dns_rr
static int dns_rr_decode(struct dns_dec *dec,
    struct dns_msg *msg, int sect_code, struct dns_sect *sect)
{
    // get next rr
    struct dns_rr *rr = NULL;
    if (dec->load_msg) {
        if (*sect->count >= sect->max) {
            return log_error_rf("No space to store %d record", sect_code);
        }
        rr = &sect->base[*sect->count];
    }

    // decode name
    char *wptr = dec->msg;
    char *wend = wptr + sizeof(dec->msg);
    char *name = wptr;
    int rc = decode_name(dec, name, wend - wptr);
    if (rc < 0) return dns_dec_err(dec, DNS_RR, DNS_NAME, -rc);
    rc = fix_label(rc, wptr, wend);
    if (rc < 0) return rc;
    wptr += rc + 1;

    // store name
    if (rr) {
        rr->name = msg_store_name(msg, name);
        if (!rr->name) return log_errno_rf("No space to store record name");
    }

    // decode type, class, ttl, rdlen
    uint16_t len = 2 + 2 + 4 + 2;
    if (dec->offset + len > dec->pkt_len) return dns_dec_err(dec, DNS_RR, DNS_HDR, DNS_ETRUNC);
    uint16_t rr_type  = dec_u16(dec->pkt_buf + dec->offset + 0);
    uint16_t rr_class = dec_u16(dec->pkt_buf + dec->offset + 2);
    uint32_t rr_ttl   = dec_u32(dec->pkt_buf + dec->offset + 4);
    uint16_t rdlen    = dec_u16(dec->pkt_buf + dec->offset + 8);
    dec->offset += len;

    // store rr fields
    if (rr) {
        rr->type  = rr_type;
        rr->class = rr_class;
        rr->ttl   = rr_ttl;
        rr->rdlen = rdlen;
    }

    // rdata
    uint16_t rd_end = dec->offset + rdlen;
    if (rd_end > dec->pkt_len) return dns_dec_err(dec, DNS_RR, DNS_RDATA, DNS_ETRUNC);
    const uint8_t *rdata = dec->pkt_buf + dec->offset;
    const char *rdata_str = wptr;
    const char *rdata_desc = "";

    // decode rdata
    switch(rr_type) {
    case DNS_TYPE_A: // IP4 address
        if (rdlen != 4) return dns_dec_err(dec, DNS_RR, DNS_A, DNS_ERDATALEN);
        // store rdata
        if (rr) memcpy(rr->rdata.a, rdata, rdlen);
        dec->offset += 4;
        // describe
        if (dec->need_emsg) {
            if (!ip4_str_encode(rdata, wptr, wend - wptr)) {
                log_error("ip4_str_decode failed");
                rdata_str = "???";
            }
            wptr += strlen(rdata_str);
            rdata_desc = rdata_str;
        }
        break;
    case DNS_TYPE_NS: //  Authoritative Name Server
        // decode name
        rc = decode_name(dec, wptr, wend - wptr);
        if (rc < 0) return dns_dec_err(dec, DNS_RR, DNS_TNS, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        // store rdata
        if (rr) {
            rr->rdata.ns_name = msg_store_name(msg, wptr);
            if (!rr->rdata.ns_name) return log_errno_rf("No space to store ns_name");
        }
        wptr += rc;
        rdata_desc = rdata_str;
        break;
    case DNS_TYPE_CNAME: // Canonical Name (Alias)
        // decode name
        rc = decode_name(dec, wptr, wend - wptr);
        if (rc < 0) return dns_dec_err(dec, DNS_RR, DNS_CNAME, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        // store rdata
        if (rr) {
            rr->rdata.cname = msg_store_name(msg, wptr);
            if (!rr->rdata.cname) return log_errno_rf("No space to store cname");
        }
        wptr += rc;
        rdata_desc = rdata_str;
        break;

    case DNS_TYPE_SOA: { // Start of Authority
        // Primary Master Name Server - MNAME
        if (dec->need_emsg) {
            rc = snprintf(wptr,wend - wptr, "MNAME=");
            if (rc < 0) log_errno_rn("snprintf SOA:NNAME failed");
            wptr += rc;
        }
        // decode name
        rc = decode_name(dec, wptr, wend - wptr);
        if (rc < 0) return dns_dec_err(dec, DNS_RR, DNS_SOA, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        // store rdata
        if (rr) {
            rr->rdata.soa.mname = msg_store_name(msg, wptr);
            if (!rr->rdata.soa.mname) return log_errno_rf("No space to store mname");
        }
        wptr += rc;
        // Responsible Person's Email - RNAME
        if (dec->need_emsg) {
            rc = snprintf(wptr,wend - wptr, " RNAME=");
            if (rc < 0) log_errno_rf("snprintf SOA:RNAME failed");
            wptr += rc;
        }
        // decode name
        rc = decode_name(dec, wptr, wend - wptr);
        if (rc < 0) return dns_dec_err(dec, DNS_RR, DNS_SOA, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        // store rdata
        if (rr) {
            rr->rdata.soa.rname = msg_store_name(msg, wptr);
            if (!rr->rdata.soa.mname) return log_errno_rf("No space to store rname");
        }
        wptr += rc;

        // decode serial,refresh,retry,expire,mininum (5 x 32 bit ints)
        if (dec->offset + 20 > rd_end) return dns_dec_err(dec, DNS_RR, DNS_SOA, DNS_ERDATALEN);
        char *names[5] = { "serial","refresh", "retry", "expire", "min_ttl" };
        uint32_t vals[5];
        memcpy(vals, dec->pkt_buf + dec->offset, 20);
        dec->offset += 20;
        for (int i = 0; i < 5; i++) {
            vals[i] = ntohl(vals[i]);
            if (dec->need_emsg) {
                rc = snprintf(wptr, wend - wptr, " %s=%u", names[i], vals[i]);
                if (rc < 0) log_errno_rf("snprintf SOA:%s failed", names[i]);
                wptr += rc;
            }
        }
        // store rdata
        if (rr) {
            rr->rdata.soa.serial  = vals[0];
            rr->rdata.soa.refresh = vals[1];
            rr->rdata.soa.retry   = vals[2];
            rr->rdata.soa.expire  = vals[3];
            rr->rdata.soa.min_ttl = vals[4];
        }
        // decoded
        rdata_desc = rdata_str;
        break;
    }
    case DNS_TYPE_PTR: // Domain Name Pointer (Reverse DNS)
        // decode name
        rc = decode_name(dec, wptr, wend - wptr);
        if (rc < 0) return dns_dec_err(dec, DNS_RR, DNS_CNAME, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        // store rdata
        if (rr) {
            rr->rdata.ptr_name = msg_store_name(msg, wptr);
            if (!rr->rdata.ptr_name) return log_errno_rf("No space to store PTR");
        }
        wptr += rc;
        rdata_desc = rdata_str;
        break;
    case DNS_TYPE_HINFO: // Host Information
        // cpu-str + os-str
        if (rdlen < 2) return dns_dec_err(dec, DNS_RR, DNS_HINFO, DNS_ERDATALEN);

        // hinfo:cpu-str
        len = dec->pkt_buf[dec->offset++];
        if (dec->offset + len > rd_end) return dns_dec_err(dec, DNS_RR, DNS_HINFO, DNS_ERDATALEN);
        // describe
        if (dec->need_emsg) {
            const char *str = mkptr(dec->pkt_buf, dec->offset);
            rc = snprintf(wptr, wend - wptr, " cpu=%.*s", len, str);
            if (rc < 0) log_errno_rf("snprintf HINFO:CPU failed");
            wptr += rc;
        }
        // store rdata
        if (rr) {
            const char *str = mkptr(dec->pkt_buf, dec->offset);
            rr->rdata.hinfo.cpu_str = msg_store_str(msg, str, len);
            if (!rr->rdata.hinfo.cpu_str) return log_errno_rf("No space to store HINFO:CPU");
        }
        dec->offset += len;

        // hinfo:os-str
        if (dec->offset >= rd_end) return dns_dec_err(dec, DNS_RR, DNS_HINFO, DNS_ERDATALEN);
        len = dec->pkt_buf[dec->offset++];
        if (dec->offset + len > rd_end) return dns_dec_err(dec, DNS_RR, DNS_HINFO, DNS_ERDATALEN);
        // describe
        if (dec->need_emsg) {
            const char *str = mkptr(dec->pkt_buf, dec->offset);
            rc = snprintf(wptr,wend - wptr, " os-str=%.*s", len, str);
            if (rc < 0) log_errno_rf("snprintf HINFO:CPU failed");
            wptr += rc;
        }
        // store rdata
        if (rr)  {
            const char *str = mkptr(dec->pkt_buf, dec->offset);
            rr->rdata.hinfo.os_str = msg_store_str(msg, str, len);
            if (!rr->rdata.hinfo.os_str) return log_errno_rf("No space to store HINFO:OS");
        }
        dec->offset += len;
        // decoded
        rdata_desc = rdata_str;
        break;
    case DNS_TYPE_MX: { // Mail Exchange
        if (rdlen < 3) return dns_dec_err(dec, DNS_RR, DNS_MX, DNS_ERDATALEN);
        // Preference
        uint16_t pref = dec_u16(mkptr(dec->pkt_buf, dec->offset));
        dec->offset += 2;
        if (dec->need_emsg) {
            rc = snprintf(wptr, wend - wptr, "pref %d ", pref);
            if (rc < 0) log_errno_rf("snprintf MX pref failed");
            wptr += rc;
        }
        // Mail Server Name
        rc = decode_name(dec, wptr, wend - wptr);
        if (rc < 0) return dns_dec_err(dec, DNS_RR, DNS_MX, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        // store rdata
        if (rr) {
            rr->rdata.mx.pref = pref;
            rr->rdata.mx.name = msg_store_name(msg, wptr);
            if (!rr->rdata.mx.name) return log_errno_rf("No space to store mx_name");
        }
        wptr += rc;
        // decoded
        rdata_desc = rdata_str;
        break;
    }
    case DNS_TYPE_TXT: // Text Strings
        while (dec->offset < rd_end) {
            // Length
            len = dec->pkt_buf[dec->offset++];
            if (dec->offset + len > rd_end) return dns_dec_err(dec, DNS_RR, DNS_TXT, DNS_ERDATALEN);
            // describe
            if (dec->need_emsg)  {
                const char *str = mkptr(dec->pkt_buf, dec->offset);
                rc = snprintf(wptr,wend - wptr, " txt=%.*s", len, str);
                if (rc < 0) log_errno_rf("snprintf HINFO:CPU failed");
                wptr += rc;
            }
            // store rdata
            if (rr) {
                const char *str = mkptr(dec->pkt_buf, dec->offset);
                size_t max_str = ARR_LEN(rr->rdata.txt.str);
                size_t num_str = rr->rdata.txt.num_str;
                if (num_str >= max_str) return log_errno_rf("No space for TXT entry");
                rr->rdata.txt.str[num_str] = msg_store_str(msg, str, len);
                if (!rr->rdata.txt.str[num_str]) return log_errno_rf("No space to store TXT str");
                rr->rdata.txt.num_str++;
            }
            dec->offset += len;
        }
        // decoded
        rdata_desc = rdata_str;
        break;
    case DNS_TYPE_AAAA:  // IPv6 Address
        if (rdlen != 16) return dns_dec_err(dec, DNS_RR, DNS_AAAA, DNS_ERDATALEN);
        dec->offset += 16;
        // store rdata
        if (rr) memcpy(rr->rdata.aaaa, rdata, rdlen);
        // describe
        if (dec->need_emsg) {
            if (!ip4_str_encode(rdata, wptr, wend - wptr)) {
                log_error("ip6_str_decode failed");
                rdata_str = "???";
            }
            wptr += strlen(rdata_str);
            rdata_desc = rdata_str;
        }
        break;
    case DNS_TYPE_SRV: { // Service Locator
        // 2 + 2 + 2 + name
        if (rdlen < 7) return dns_dec_err(dec, DNS_RR, DNS_SRV, DNS_ERDATALEN);
        // decode 3 x uint16
        uint16_t prior  = dec_u16(dec->pkt_buf + dec->offset + 0);
        uint16_t weight = dec_u16(dec->pkt_buf + dec->offset + 2);
        uint16_t port   = dec_u16(dec->pkt_buf + dec->offset + 4);
        dec->offset += 6;
        // describe
        if (dec->need_emsg) {
            rc = snprintf(wptr,wend - wptr, "Priority %d Weight %d port %d ", prior, weight, port);
            if (rc < 0) log_errno_rf("snprintf SRV Priority");
            wptr += rc;
        }
        // name
        rc = decode_name(dec, wptr, wend - wptr);
        if (rc < 0) return dns_dec_err(dec, DNS_RR, DNS_SRV, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        // store rdata
        if (rr) {
            rr->rdata.srv.prior = prior;
            rr->rdata.srv.weight = weight;
            rr->rdata.srv.port = port;
            rr->rdata.srv.name = msg_store_name(msg, wptr);
            if (!rr->rdata.srv.name) return log_errno_rf("No space to store srv_name");
        }
        wptr += rc;
        // decoded
        rdata_desc = rdata_str;
        break;
    }
    case DNS_TYPE_OPT: { // EDNS0 Options (RFC 6891)
        if (sect_code != DNS_AR) return dns_dec_err(dec, DNS_RR, DNS_OPT, DNS_EMSGFMT);
        // decode class, ttl
        uint16_t udp_size = rr_class;
        uint8_t ext_rcode = (rr_ttl >> 24) & 0xff;
        uint8_t version   = (rr_ttl >> 16) & 0xff;
        uint8_t do_bit    = (rr_ttl & 0x8000) ? 1 : 0;
        uint16_t z_bits   = rr_ttl & 0x7fff;
        // store rdata
        if (rr) {
            rr->rdata.opt.udp_size = udp_size;
            rr->rdata.opt.ext_rcode = ext_rcode;
            rr->rdata.opt.edns_ver = version;
            rr->rdata.opt.do_bit = do_bit;
            rr->rdata.opt.z_bits = z_bits;
            rr->class = 0;
            rr->name = msg_store_name(msg, name);
            if (!rr->name) return log_errno_rf("No space to store root");
        }

        // override name
        if (*name == '.') name = DNS_ROOT_STR;

        // describe
        rc = snprintf(wptr, wend - wptr,
                "UDP-size:%d Ext-RCODE:%d EDNS0:%d DNSEC-OK:%d",
                udp_size, ext_rcode, version, !!do_bit);
        if (rc < 0) log_errno_rf("snprintf OPT failed");
        wptr += rc;

        // save for validation at msg end
        dec->got_edns = 1;
        dec->udp_size = udp_size;
        dec->ext_rcode = ext_rcode;
        dec->edns_ver = version;
        dec->dnssec_ok = !!do_bit;

        // skip remaing fields
        dec->offset = rd_end;

        // decoded
        rdata_desc = rdata_str;
        break;
    }
    case DNS_TYPE_ANY: // Wildcard match (Query only)
        dec->offset += rdlen;
        break;
    default:
        dec->offset += rdlen;
        break;
    }

    if (dec->offset != rd_end) return dns_dec_err(dec, DNS_RR, DNS_RDATA, DNS_ERDATALEN);

    // next record
    if (rr && sect) (*sect->count)++;

    if (dec->need_emsg) {
        // ensure no hidden fields
        const char *rec_name  = str_def(name, "<null>");
        const char *sect_str  = dec_code_tostr(sect_code);
        const char *class_str = dns_class_tostr(rr_class);
        const char *type_str  = dns_type_tostr(rr_type);
        // Record prefix
        rc = rr_type == DNS_TYPE_OPT
            ? dns_wmsg(dec, "  %s: %s %s %s\n", sect_str, rec_name, type_str, rdata_desc)
            : dns_wmsg(dec, "  %s: %s %d %s %s %s\n", sect_str, rec_name, rr_ttl,
                class_str, type_str, rdata_desc);
    }

    // all done
    return rc;
}

// decode a question data entry
static int dns_qd_decode(struct dns_dec *dec, struct dns_msg *msg)
{
    // grab next qd
    struct dns_qd *qd = NULL;
    if (dec->load_msg) {
        if (msg->qd_len >= ARR_LEN(msg->qd)) {
            return log_error_rf("No space to store question");
        }
        qd = &msg->qd[msg->qd_len];
    }

    char *wptr = dec->msg;
    char *wptr_end = wptr + sizeof(dec->msg);

    // decode name
    char *name = wptr;
    int ec = decode_name(dec, name, wptr_end - wptr);
    if (ec < 0) return dns_dec_err(dec, DNS_QD, DNS_NAME, ec);
    wptr += ec;

    // decode qtype, qclass
    if (dec->offset + 4 > dec->pkt_len) return dns_dec_err(dec, DNS_QD, DNS_HDR, DNS_ETRUNC);
    uint16_t qtype  = dec_u16(dec->pkt_buf + dec->offset);
    uint16_t qclass = dec_u16(dec->pkt_buf + dec->offset + 2);
    dec->offset += 4;

    // load question
    if (qd)  {
        qd->qname = msg_store_name(msg, name);
        if (!qd->qname) return log_errno_rf("No space to store question name");
        qd->qtype = qtype;
        qd->qclass = qclass;
        msg->qd_len++;
    }

    if (dec->need_emsg) {
        // ensure no hidden fields
        const char *quest_name = str_def(name, "<null>");
        const char *class_str = dns_class_tostr(qclass);
        const char *type_str = dns_type_tostr(qtype);
        // desc Question
        ec = dns_wmsg(dec, "  %s: %s %s %s\n",
            "Question", quest_name, class_str, type_str
        );
    }

    // all done
    return ec;
}

// decode all resource records (RR) for a DNS section
static int dns_sect_decode(struct dns_dec *dec, struct dns_msg *msg,
    int nrec, int sect_code, struct dns_sect *sect)
{
    for (int i = 0; i < nrec; i++) {
        if (dns_rr_decode(dec, msg, sect_code, sect) != 0) {
            return dns_dec_err(dec, DNS_PDU, sect_code, i + DNS_EC_ISROW);
        }
    }

    return 0;
}

// decode answer section
static int decode_an(struct dns_dec *dec, struct dns_msg *msg)
{
    struct dns_sect sect;
    sect_init(&sect, msg->an, ARR_LEN(msg->an), &msg->an_len);
    return dns_sect_decode(dec, msg, msg->hdr.an_count, DNS_AN, &sect);
}

// decode authority section
static int decode_ns(struct dns_dec *dec, struct dns_msg *msg)
{
    struct dns_sect sect;
    sect_init(&sect, msg->ns, ARR_LEN(msg->ns), &msg->ns_len);
    return dns_sect_decode(dec, msg, msg->hdr.ns_count, DNS_NS, &sect);
}

// decode additional section
static int decode_ar(struct dns_dec *dec, struct dns_msg *msg)
{
    struct dns_sect sect;
    sect_init(&sect, msg->ar, ARR_LEN(msg->ar), &msg->ar_len);
    return dns_sect_decode(dec, msg, msg->hdr.ar_count, DNS_AR, &sect);
}

// decode question section
static int decode_qd(struct dns_dec *dec, struct dns_msg *msg)
{
    for (int i = 0; i < msg->hdr.qd_count; i++) {
        if (dns_qd_decode(dec, msg) != 0) {
            return dns_dec_err(dec, DNS_PDU, DNS_QD, i + DNS_EC_ISROW);
        }
    }

    return 0;
}

// decode header into msg
static int decode_hdr(struct dns_dec *dec, struct dns_msg *msg)
{
    struct dns_hdr *hdr = &msg->hdr;

    // read header
    int rc = dns_hdr_decode(hdr, dec->pkt_buf, dec->pkt_len);
    if (rc) return dns_dec_err(dec, DNS_PDU, DNS_HDR, rc);
    dec->offset += DNS_HDR_LEN;

    // describe msg ?
    if (!dec->need_emsg) return 0;

    // copy hdr fields for dns_dec_genmsg
    dec->hdr = *hdr;

    // describe
    uint16_t flags = hdr->flags;
    int qr = flags & DNS_FLAGS_QR ? 1 : 0;
    int opcode  = (flags & DNS_FLAGS_OPCODE) >> 11;
    const char *opcode_str = opcode_tostr(opcode);
    const char *type_str = qr ? "RESPONSE" : "QUERY";

    char tmp[100];
    struct strbuf buf = STRBUF_INIT(tmp, sizeof(tmp));
    tmp[0] = '\0';

    if (qr) {
        // query response
        int as = flags & DNS_FLAGS_AA ? 1 : 0;
        int tc = flags & DNS_FLAGS_TC ? 1 : 0;
        int rd = flags & DNS_FLAGS_RD ? 1 : 0;
        int ra = flags & DNS_FLAGS_RA ? 1 : 0;
        int rcode = flags & DNS_FLAGS_RCODE;

        // add flags
        if (as) strbuf_puticm(&buf, ' ', STR_LIT("AS:1"));
        if (tc) strbuf_puticm(&buf, ' ', STR_LIT("TC:1"));
        if (rd) strbuf_puticm(&buf, ' ', STR_LIT("RD:1"));
        if (ra) strbuf_puticm(&buf, ' ', STR_LIT("RA:1"));

        // convert RCODE to str
        const char *rcode_str = rcode_tostr(rcode);
        strbuf_puticm(&buf, ' ', STR_LIT("RCODE:"));
        strbuf_puts(&buf, rcode_str);

        // validate OPCODE range
        if (opcode == 3 || opcode > 5) {
            strbuf_puticm(&buf, ' ', STR_LIT("bad-opcode"));
        }

        // validate RCODE range
        if (rcode > 10) {
            strbuf_puticm(&buf, ' ', STR_LIT("bad-rcode"));
            dns_dec_err(dec, DNS_PDU, DNS_HDR, DNS_ERCODE);
        }
    }
    else {
        // query
        int tc = flags & DNS_FLAGS_TC ? 1 : 0;
        int rd = flags & DNS_FLAGS_RD ? 1 : 0;
        int cd = flags & DNS_FLAGS_CD ? 1 : 0;
        int ad = flags & DNS_FLAGS_AD ? 1 : 0;

        // add flags
        if (tc) strbuf_puticm(&buf, ' ', STR_LIT("TC:1"));
        if (rd) strbuf_puticm(&buf, ' ', STR_LIT("RD:1"));
        if (cd) strbuf_puticm(&buf, ' ', STR_LIT("CD:1"));
        if (ad) strbuf_puticm(&buf, ' ', STR_LIT("AD:1"));

        // validate OPCODE range
        if (opcode == 3 || opcode > 5) {
            strbuf_puticm(&buf, ' ', STR_LIT("bad-opcode"));
            dns_dec_err(dec, DNS_PDU, DNS_HDR, DNS_EOPCODE);
        }
    }

    // desc PDU as we decode
    rc = dns_wmsg(dec,
        "[%s] ID 0x%04x QR:%d OPCODE:%s %.*s\n",
        type_str, hdr->id, qr, opcode_str,
        (int) strbuf_pos(&buf), strbuf_start(&buf));

    return rc;
}

// decode DNS message - See rfc1035 Message format 4.1. Format
static int decode_msg(struct dns_dec *dec, struct dns_msg *msg)
{
    int rc;

    dns_msg_reset(msg);

    if ((rc = decode_hdr(dec, msg))) return rc;
    if ((rc = decode_qd(dec, msg)))  return rc;
    if ((rc = decode_an(dec, msg)))  return rc;
    if ((rc = decode_ns(dec, msg)))  return rc;
    if ((rc = decode_ar(dec, msg)))  return rc;

    // validate message size
    if (dec->need_emsg && dec->pkt_len > dec->udp_size) {
        //  pkt len exceed 512 bytes (UDP) or declared length
        rc = dns_wmsg(dec,
            "UDP message: packet-length %zu > max size %zu\n",
            dec->pkt_len, dec->udp_size
        );
    }

    // all done
    return rc;
}

int dns_validate(const void *buf, size_t len, char *emsg, size_t emsg_len)
{
    struct dns_dec dec = {
        .pkt_buf = buf,
        .pkt_len = len,
        .udp_size = DNS_MAX_UDP,
        .need_emsg = 1,
        .emsg = STRBUF_INIT(emsg, emsg_len)
    };

    struct dns_msg msg = { 0 };

    if (emsg) *emsg = '\0';

    int rc = decode_msg(&dec, &msg);

    if (dec.need_emsg) {
        int ec = dns_dec_genmsg(&dec);
        if (ec && !rc) rc = ec;
    }

    // 0 mean succes
    return rc;
}

// decode a buffer to a dns message
int dns_msg_decode(struct dns_msg *msg, uint8_t *buf, size_t len)
{
    struct dns_dec dec = {
        .pkt_buf = buf,
        .pkt_len = len,
        .udp_size = DNS_MAX_UDP,
        .load_msg = 1
    };

    return decode_msg(&dec, msg);
}

// used for jmp ptrs
struct dns_suffix {
    const char *name;
    uint8_t len;
    uint16_t offset;
};

// DNS encoder
struct dns_enc {
    uint8_t *pkt_buf;
    size_t pkt_max;
    size_t pkt_len;
    size_t num_suffix;
    struct dns_suffix suffix[DNS_MAX_SUFFIX];
};

// encode dns name into pkt buffer
static uint8_t *encode_name(struct dns_enc *enc, uint8_t *wptr, const char *name)
{
    int offset = wptr - enc->pkt_buf;
    const char *name_end = name ? name + strlen(name) : NULL;

    while (name < name_end) {
        // scan for suffix match
        for (size_t i = 0; i < enc->num_suffix; i++) {
            if (enc->suffix[i].offset < offset && strcasecmp(enc->suffix[i].name, name) == 0) {
                // suffix match - drop a 2-byte comp ptr
                uint16_t comp_ptr = 0xc000 | enc->suffix[i].offset;
                *wptr++ = comp_ptr >> 8;
                *wptr++ = comp_ptr;
                // done
                return wptr;
            }
        }
        if (enc->num_suffix < ARR_LEN(enc->suffix)) {
            // store new suffix
            enc->suffix[enc->num_suffix].name = name;
            enc->suffix[enc->num_suffix].offset = wptr - enc->pkt_buf;
            enc->num_suffix++;
        }

        // look for next label
        const char *dot_ptr = strchr(name, '.');
        uint8_t len = dot_ptr ? dot_ptr - name : name_end - name;
        if (len > DNS_LABEL_MAXSTR) {
            return log_error_rn(
                "Cannot encode label for name - len %d > max %d", len, DNS_LABEL_MAXSTR
            );

        }
        *wptr++ = len;
        wptr = mempcpy(wptr, name, len);

        name = dot_ptr ? dot_ptr + 1 : name_end;
    }

    // final label - drop a nul
    *wptr++ = '\0';

    // return wpos
    return wptr;
}

// return space back to buffer
static inline int dns_enc_retspace(struct dns_enc *enc, size_t len)
{
    if (len > enc->pkt_len) {
        return log_error_rf("retspace %zu > pkt_len %zu", len, enc->pkt_len);
    }

    enc->pkt_len -= len;

    return 0;
}

// reserve space in buffer
static inline uint8_t *dns_enc_mkspace(struct dns_enc *enc, size_t len)
{
    if (enc->pkt_len + len > enc->pkt_max) {
        return NULL;
    }

    uint8_t *buf = enc->pkt_buf + enc->pkt_len;
    enc->pkt_len += len;

    return buf;
}

static uint8_t *enc_fld_mkspace(struct dns_enc *enc, size_t len, int sc, int type)
{
    uint8_t *wbuf = dns_enc_mkspace(enc, len);

    if (!wbuf) {
        return log_error_rn(
            "No room for %s field %s len %zu",
            dec_code_tostr(sc), dns_type_tostr(type), len
        );
    }

    return wbuf;
}

// encode a dns question
static int dns_enc_quest(struct dns_enc *enc, struct dns_qd *quest)
{
    size_t len = DNS_NAME_MAXLEN + 4;

    uint8_t *wbuf = dns_enc_mkspace(enc, len);
    if (!wbuf) return log_error_rf("No room for question len %zu", len);

    // encode
    uint8_t *wptr = wbuf;
    wptr = encode_name(enc, wptr, quest->qname);
    if (!wptr) return -1;
    wptr = enc_u16(wptr, quest->qtype);
    wptr = enc_u16(wptr, quest->qclass);
    size_t used = wptr - wbuf;
    int rc = dns_enc_retspace(enc, len - used);
    if (rc) return rc;

    return 0;
}

static int dns_enc_name(struct dns_enc *enc, const char *name, int sc, int type)
{
    int len = DNS_NAME_MAXLEN;
    uint8_t *wbuf = enc_fld_mkspace(enc, len, sc, type);
    if (!wbuf) return -1;

    uint8_t *wptr = wbuf;
    wptr = encode_name(enc, wptr, name);
    if (!wptr) return -1;
    size_t used = wptr - wbuf;
    int rc = dns_enc_retspace(enc, len - used);

    return rc;
}

static int dns_enc_mem(struct dns_enc *enc, void *mem, size_t len, int sc, int type)
{
    uint8_t *wbuf = enc_fld_mkspace(enc, len, sc, type);
    if (!wbuf) return -1;
    memcpy(wbuf, mem, len);

    return 0;
}

// encode a [len] + str
static int dns_enc_str(struct dns_enc *enc, char *str, size_t len, int sc, int type)
{
    uint8_t *wbuf = enc_fld_mkspace(enc, len + 1, sc, type);
    if (!wbuf) return -1;

    uint8_t *wptr = wbuf;
    *wptr++ = len;
    memcpy(wptr, str, len);

    return 0;
}

// encode RR into packet buffer
static int dns_rr_encode(struct dns_enc *enc, struct dns_rr *rr, int sc)
{
    // add hdr (name|type|class|ttl|rdlength
    int len = DNS_NAME_MAXLEN + 2 + 2 + 4 + 2;
    uint8_t *wbuf = enc_fld_mkspace(enc, len, sc, 0);
    if (!wbuf) return -1;

    uint8_t *wptr = wbuf;
    wptr = encode_name(enc, wptr, rr->name);
    if (!wptr) return -1;
    wptr = enc_u16(wptr, rr->type);
    wptr = enc_u16(wptr, rr->class);
    wptr = enc_u32(wptr, rr->ttl);
    uint8_t *rdlen_ptr = wptr;
    wptr = enc_u16(wptr,  0);
    size_t used = wptr - wbuf;
    int rc = dns_enc_retspace(enc, len - used);
    if (rc) return rc;
    uint16_t pkt_len = enc->pkt_len;

    // add rdata
    switch(rr->type) {
    case DNS_TYPE_A:
        rc = dns_enc_mem(enc, rr->rdata.a, sizeof(rr->rdata.a), sc, rr->type);
        if (rc) return rc;
        break;
    case DNS_TYPE_NS:
        rc = dns_enc_name(enc, rr->rdata.ns_name, sc, rr->type);
        if (rc) return rc;
        break;
    case DNS_TYPE_CNAME: // Canonical Name (Alias)
       rc = dns_enc_name(enc, rr->rdata.cname, sc, rr->type);
       if (rc) return rc;
       break;
    case DNS_TYPE_SOA:  // Start of Authority
        // Primary Master Name Server - MNAME
        rc = dns_enc_name(enc, rr->rdata.soa.mname, sc, rr->type);
        if (rc) return rc;
        rc = dns_enc_name(enc, rr->rdata.soa.rname, sc, rr->type);
        if (rc) return rc;
        //  encode 5 fields
        len = 5 * sizeof(uint32_t);
        wptr = enc_fld_mkspace(enc, len, sc, rr->type);
        if (!wptr) return -1;
        wptr = enc_u32(wptr, rr->rdata.soa.serial);
        wptr = enc_u32(wptr, rr->rdata.soa.refresh);
        wptr = enc_u32(wptr, rr->rdata.soa.retry);
        wptr = enc_u32(wptr, rr->rdata.soa.expire);
        wptr = enc_u32(wptr, rr->rdata.soa.min_ttl);
        break;
    case DNS_TYPE_PTR: // Domain Name Pointer (Reverse DNS)
        rc = dns_enc_name(enc, rr->rdata.ptr_name, sc, rr->type);
        if (rc) return rc;
        break;
    case DNS_TYPE_HINFO: { // Host Information
        size_t len_cpu = safe_strlen(rr->rdata.hinfo.cpu_str);
        size_t len_os  = safe_strlen(rr->rdata.hinfo.os_str);
        wptr = enc_fld_mkspace(enc, 2 + len_cpu + len_os, sc, rr->type);
        if (!wptr) return -1;
        *wptr++ = len_cpu;
        if (len_cpu)  memcpy(wptr, rr->rdata.hinfo.cpu_str, len_cpu);
        *wptr++ = len_os;
        if (len_os) memcpy(wptr, rr->rdata.hinfo.os_str, len_os);
        break;
    }
    case DNS_TYPE_MX: // Mail Exchange
        len = sizeof(uint32_t);
        wptr = enc_fld_mkspace(enc, len, sc, rr->type);
        if (!wptr) return -1;
        wptr = enc_u16(wptr, rr->rdata.mx.pref);
        rc = dns_enc_name(enc, rr->rdata.mx.name, sc, rr->type);
        if (rc) return rc;
        break;
    case DNS_TYPE_TXT:
        // encode txt array
        for (int i = 0; i < rr->rdata.txt.num_str; i++) {
            char *str = rr->rdata.txt.str[i];
            len = safe_strlen(str);
            rc = dns_enc_str(enc, str, len, sc, rr->type);
            if (rc) return rc;
        }
        break;
    case DNS_TYPE_AAAA: // IPv6 Address
        len = sizeof(rr->rdata.aaaa);
        rc = dns_enc_mem(enc, rr->rdata.aaaa, len, sc, rr->type);
        if (rc) return rc;
        break;
    case DNS_TYPE_SRV: // Service Locator
        len = sizeof(uint16_t) * 3;
        wptr = enc_fld_mkspace(enc, 2, sc, rr->type);
        if (!wptr) return -1;
        wptr = enc_u16(wptr, rr->rdata.srv.prior);
        wptr = enc_u16(wptr, rr->rdata.srv.weight);
        wptr = enc_u16(wptr, rr->rdata.srv.port);
        rc = dns_enc_name(enc, rr->rdata.srv.name, sc, rr->type);
        if (rc) return rc;
        break;
    case DNS_TYPE_OPT: {// EDNS0 Options (RFC 6891)
        uint32_t ttl = 0;
        ttl |= rr->rdata.opt.ext_rcode << 24;
        ttl |= rr->rdata.opt.edns_ver  << 16;
        if (rr->rdata.opt.do_bit) ttl |= 0x8000;
        ttl |= rr->rdata.opt.z_bits & 0x7fff;
        // rewind to class field
        wptr = rdlen_ptr - (4 + 2);
        wptr = enc_u16(wptr, rr->rdata.opt.udp_size);
        wptr = enc_u16(wptr, ttl);
        break;
    }
    case DNS_TYPE_ANY: // Wildcard match (Query only)
        break;
    default:
        break;
    }

    uint16_t rdlen = enc->pkt_len - pkt_len;
    enc_u16(rdlen_ptr,rdlen);

    // all done
    return 0;
}

// encode a section block
static int dns_sect_enc(struct dns_enc *enc, int sc, struct dns_sect *sect)
{
    int nrec = *sect->count;

    for (int i = 0; i < nrec; i++) {
        int rc = dns_rr_encode(enc, &sect->base[i], sc);
        if (rc) return rc;
    }

    return 0;
}

// encode additional section
static int encode_ar(struct dns_enc *enc, struct dns_msg *msg)
{
    struct dns_sect sect;
    sect_init(&sect, msg->ar, ARR_LEN(msg->ar), &msg->ar_len);
    return dns_sect_enc(enc, DNS_AR, &sect);
}

// encode authority section
static int encode_ns(struct dns_enc *enc, struct dns_msg *msg)
{
    struct dns_sect sect;
    sect_init(&sect, msg->ns, ARR_LEN(msg->ns), &msg->ns_len);
    return dns_sect_enc(enc, DNS_NS, &sect);
}

// encode answer section
static int encode_an(struct dns_enc *enc, struct dns_msg *msg)
{
    struct dns_sect sect;
    sect_init(&sect, msg->an, ARR_LEN(msg->an), &msg->an_len);
    return dns_sect_enc(enc, DNS_AN, &sect);
}

// encode question section
static int encode_qd(struct dns_enc *enc, struct dns_msg *msg)
{
    for (int i = 0; i < msg->qd_len; i++) {
        int rc = dns_enc_quest(enc, &msg->qd[i]);
        if (rc) return rc;
    }

    return 0;
}

// encode a DNS header
static int encode_hdr(struct dns_enc *enc, struct dns_msg *msg)
{
    // sync hdr
    struct dns_hdr *hdr = &msg->hdr;
    hdr->qd_count = msg->qd_len;
    hdr->an_count = msg->an_len;
    hdr->ns_count = msg->ns_len;
    hdr->ar_count = msg->ar_len;

    // make space
    uint8_t *wbuf = dns_enc_mkspace(enc, DNS_HDR_LEN);
    if (!wbuf) {
        return log_error_rf("No room for DNS hdr len %d", DNS_HDR_LEN);
    }

    // encode
    uint8_t *wptr = wbuf;
    wptr = enc_u16(wptr, hdr->id);
    wptr = enc_u16(wptr, hdr->flags);
    wptr = enc_u16(wptr, hdr->qd_count);
    wptr = enc_u16(wptr, hdr->an_count);
    wptr = enc_u16(wptr, hdr->ns_count);
    wptr = enc_u16(wptr, hdr->ar_count);

    return 0;
}

// encode DNS message
static int encode_msg(struct dns_enc *enc, struct dns_msg *msg)
{
    int rc;

    if ((rc = encode_hdr(enc, msg))) return rc;
    if ((rc = encode_qd(enc, msg)))  return rc;
    if ((rc = encode_an(enc, msg)))  return rc;
    if ((rc = encode_ns(enc, msg)))  return rc;
    if ((rc = encode_ar(enc, msg)))  return rc;

    return rc;
}

// encode DNS message into buffer
ssize_t dns_msg_encode(struct dns_msg *msg, uint8_t *buf, size_t len)
{
    struct dns_enc enc =  {
        .pkt_buf = buf,
        .pkt_max = len,
    };

    int rc = encode_msg(&enc, msg);
    if (rc) return -1;

    return enc.pkt_len;
}


// add dns_rr to a DNS section
static int dns_sect_add_rr(struct dns_msg *msg, struct dns_sect *sect, struct dns_rr *src_rr)
{
    if (!src_rr) return 0;

    // space for record ?
    struct dns_rr *rr;
    if (*sect->count >= sect->max) return -1;
    rr = &sect->base[*sect->count];

    rr->name = msg_store_name(msg, src_rr->name);
    if (!rr->name) return -1;

    rr->type  = src_rr->type;
    rr->class = src_rr->class;
    rr->ttl   = src_rr->ttl;

    // store rdata
    switch(rr->type) {
    case DNS_TYPE_A:
        memcpy(rr->rdata.a, src_rr->rdata.a, 4);
        break;
    case DNS_TYPE_NS:
        rr->rdata.ns_name = msg_store_name(msg, src_rr->rdata.ns_name);
        if (!rr->rdata.ns_name) return -1;
        break;
    case DNS_TYPE_CNAME: // Canonical Name (Alias)
       rr->rdata.cname = msg_store_name(msg, src_rr->rdata.cname);
       if (!rr->rdata.cname) return -1;
       break;
    case DNS_TYPE_SOA: { // Start of Authority
        // Primary Master Name Server - MNAME
        rr->rdata.soa.mname = msg_store_name(msg, src_rr->rdata.soa.mname);
        if (!rr->rdata.soa.mname) return -1;
        // Responsible Person's Email - RNAME
        rr->rdata.soa.rname = msg_store_name(msg, src_rr->rdata.soa.rname);
        if (!rr->rdata.soa.mname) return -1;
        rr->rdata.soa.serial   = src_rr->rdata.soa.serial;
        rr->rdata.soa.refresh  = src_rr->rdata.soa.refresh;
        rr->rdata.soa.retry    = src_rr->rdata.soa.retry;
        rr->rdata.soa.expire   = src_rr->rdata.soa.expire;
        rr->rdata.soa.min_ttl  = src_rr->rdata.soa.min_ttl;
        break;
    }
    case DNS_TYPE_PTR: // Domain Name Pointer (Reverse DNS)
        rr->rdata.ptr_name = msg_store_name(msg, src_rr->rdata.ptr_name);
        if (!rr->rdata.ptr_name) return -1;
        break;
    case DNS_TYPE_HINFO: // Host Information
        rr->rdata.hinfo.cpu_str = msg_store_name(msg, src_rr->rdata.hinfo.cpu_str);
        rr->rdata.hinfo.os_str  = msg_store_name(msg, src_rr->rdata.hinfo.os_str);
        break;
    case DNS_TYPE_MX: // Mail Exchange
        rr->rdata.mx.pref = src_rr->rdata.mx.pref;
        rr->rdata.mx.name = msg_store_name(msg, src_rr->rdata.mx.name);
        if (!rr->rdata.mx.name) return -1;
        break;
    case DNS_TYPE_TXT:
        for (int i = 0; i < src_rr->rdata.txt.num_str; i++) {
            char *src_str = src_rr->rdata.txt.str[i];
            if (rr->rdata.txt.num_str >= ARR_LEN(rr->rdata.txt.str)) return -1;
            rr->rdata.txt.str[rr->rdata.txt.num_str] = msg_store_name(msg, src_str);
            if (!rr->rdata.txt.str[rr->rdata.txt.num_str]) return -1;
            rr->rdata.txt.num_str++;
        }
        break;
    case DNS_TYPE_AAAA:  // IPv6 Address
        mempcpy(rr->rdata.aaaa, src_rr->rdata.aaaa, 16);
        break;
    case DNS_TYPE_SRV: // Service Locator
        rr->rdata.srv.prior  = src_rr->rdata.srv.prior;
        rr->rdata.srv.weight = src_rr->rdata.srv.weight;
        rr->rdata.srv.port   = src_rr->rdata.srv.port;
        rr->rdata.srv.name   = msg_store_name(msg, src_rr->rdata.srv.name);
        if (!rr->rdata.srv.name) return -1;
        break;
    case DNS_TYPE_ANY: // Wildcard match (Query only)
        break;
    case DNS_TYPE_OPT: // EDNS0 Options (RFC 6891)
        rr->rdata.opt.udp_size = src_rr->rdata.opt.udp_size;
        rr->rdata.opt.ext_rcode = src_rr->rdata.opt.ext_rcode;
        rr->rdata.opt.edns_ver = src_rr->rdata.opt.edns_ver;
        rr->rdata.opt.do_bit = src_rr->rdata.opt.do_bit;
        break;
    default:
        break;
    }

    // record loaded okay
    (*sect->count)++;

    return 0;
}

// add question to DNS msg
int dns_add_qdn(struct dns_msg *msg, const char *qname, size_t len, uint16_t qtype, uint16_t qclass)
{
    struct dns_qd *qd;
    if (msg->qd_len >= ARR_LEN(msg->qd)) return -1;
    qd = &msg->qd[msg->qd_len];

    qd->qname = msg_store_str(msg, qname, len);
    if (!qd->qname) return -1;
    qd->qtype = qtype;
    qd->qclass = qclass;

    // question added
    msg->qd_len++;

    return 0;
}

// add a resource record to an|ns|ar section
int dns_add_rr(struct dns_msg *msg, int sc, struct dns_rr *rr)
{
    struct dns_sect sect;

    switch(sc) {
    case DNS_MSG_AN: sect_init(&sect, msg->an, ARR_LEN(msg->an), &msg->an_len); break;
    case DNS_MSG_NS: sect_init(&sect, msg->ns, ARR_LEN(msg->ns), &msg->ns_len); break;
    case DNS_MSG_AR: sect_init(&sect, msg->ar, ARR_LEN(msg->ar), &msg->ar_len); break;
    default: return -1;
    }

    return dns_sect_add_rr(msg, &sect, rr);
}

// get first dns resource record
struct dns_rr *dns_get_rr(struct dns_msg *msg)
{
    if (msg->an_len) return &msg->an[0];
    if (msg->ns_len) return &msg->ns[0];
    if (msg->ar_len) return &msg->ar[0];

    return NULL;
}

int dns_get_type(const char *str)
{
    if (!strcasecmp(str, "A"))     return DNS_TYPE_A;
    if (!strcasecmp(str, "NS"))    return DNS_TYPE_NS;
    if (!strcasecmp(str, "CNAME")) return DNS_TYPE_CNAME;
    if (!strcasecmp(str, "SOA"))   return DNS_TYPE_SOA;
    if (!strcasecmp(str, "PTR"))   return DNS_TYPE_PTR;
    if (!strcasecmp(str, "HINFO")) return DNS_TYPE_HINFO;
    if (!strcasecmp(str, "MX"))    return DNS_TYPE_MX;
    if (!strcasecmp(str, "TXT"))   return DNS_TYPE_TXT;
    if (!strcasecmp(str, "AAAA"))  return DNS_TYPE_AAAA;
    if (!strcasecmp(str, "SRV"))   return DNS_TYPE_SRV;

    return 0;
}

int dns_get_class(const char *str)
{
    if (!strcasecmp(str, "IN"))  return DNS_CLASS_IN;
    if (!strcasecmp(str, "CS"))  return DNS_CLASS_CS;
    if (!strcasecmp(str, "CH"))  return DNS_CLASS_CH;
    if (!strcasecmp(str, "HS"))  return DNS_CLASS_HS;
    if (!strcasecmp(str, "ANY")) return DNS_CLASS_ANY;

    return 0;
}

int dns_get_flag(const char *str)
{
    if (!strcasecmp(str, "CD")) return DNS_FLAGS_CD;
    if (!strcasecmp(str, "RD")) return DNS_FLAGS_RD;
    if (!strcasecmp(str, "AD")) return DNS_FLAGS_AD;

    return 0;
}

const char *dns_sc_tostr(int sc)
{
    switch(sc) {
    case DNS_MSG_AN: return "Answer";
    case DNS_MSG_NS: return "Authority";
    case DNS_MSG_AR: return "Additional";
    default: return int_tostr(sc);
    }
}


