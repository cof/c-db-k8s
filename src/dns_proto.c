/*
 * A DNS message codec API
 * -----------------------
 * See dns_proto.h for full API description.
 *
 * Basic API
 * ----------
 * validate_dns_packet(pkt_buf, pkt_len, emsg) : check pkt valid and print desc to esmg
 * dns_msg_decode(msg, buf, len) : decode buffer into a DNS message
 * dns_msg_encode(msg, buf, len) : encode DNS message into buffer
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
    struct dns_header hdr;
    char msg[DNS_MSG_SIZE]; // parse_dns_name + dns_err_tostr
};

// print a msg to decoder state
static int dns_wmsg(struct dns_dec *dec, const char *fmt, ...) \
    __attribute__((format(printf, 2, 3)));

// decoders

// XXX - use xmacros to ensure both code and string match
#define DNS_ERRORS(X) \
    X(DNS_ERR_OK, "Okay") \
    X(DNS_ERR_HDRLEN, "header len") \
    X(DNS_ERR_BADJMP, "Invalid compression pointer (outside range)") \
    X(DNS_ERR_MAXJMP, "Invalid compression pointer (loop detected)") \
    X(DNS_ERR_NAMELEN, "Name length bigger than pkt size") \
    X(DNS_ERR_OUTLEN, "Name bigger than buf size") \
    X(DNS_ERR_NONULL, "Name missing null char") \
    X(DNS_ERR_MINLEN, "Field smaller than min len") \
    X(DNS_ERR_FLDTRUNC,  "Field truncated") \
    X(DNS_ERR_FLDRDLEN, "Field len bigged than rdata len") \
    X(DNS_ERR_FLDPKTLEN, "Field len bigged than pkt len") \
    X(DNS_ERR_FLDMISS, "Field Missing") \
    X(DNS_ERR_OPTSECT, "OPT field not allowed") \
    X(DNS_ERR_NOSPACE, "No space in buffer") \
    X(DNS_ERR_BADOPCODE, "Invalid OPCODE") \
    X(DNS_ERR_BADRCODE,  "Invalid RCODE")


#define DNS_ERROR_ENUM(NAME, TEXT) NAME,
#define DNS_ERROR_TEXT(NAME, TEXT) [NAME] = TEXT,

enum dns_dec_error {
    DNS_ERRORS(DNS_ERROR_ENUM)
};

static const char *dns_ec_strs[] = {
    DNS_ERRORS(DNS_ERROR_TEXT)
};

#define DNS_EC_ISROW 10000

static const char *dns_ec_tostr(int code)
{
    const char *str = ec_tostr(ARRAY(dns_ec_strs), code, NULL);
    if (str) return str;

    if (code >= DNS_EC_ISROW) {
        // its section row number
        code -= DNS_EC_ISROW;
        code++;
    }

    return int_tostr(code);
}

// decode error locations - using xmacros
#define DNS_DECODES(X) \
    X(DNS_DEC_NONE, "NONE") \
    X(DNS_DEC_PDU,  "PDU") \
    X(DNS_DEC_HDR, "HDR") \
    X(DNS_DEC_QUESTION,   "Question") \
    X(DNS_DEC_ANSWER,     "Answer") \
    X(DNS_DEC_AUTHORITY,  "Authority") \
    X(DNS_DEC_ADDITIONAL, "Additional") \
    X(DNS_DEC_RECORD,     "Record") \
    X(DNS_DEC_RDATA,      "RDATA") \
    X(DNS_DEC_NAME,       "Name") \
    X(DNS_DEC_TYPE_A,     "A") \
    X(DNS_DEC_TYPE_NS,    "NS") \
    X(DNS_DEC_TYPE_CNAME, "CNAME") \
    X(DNS_DEC_TYPE_SOA,   "SOA") \
    X(DNS_DEC_TYPE_PTR,   "PTR") \
    X(DNS_DEC_TYPE_HINFO, "HINFO")\
    X(DNS_DEC_TYPE_MX,    "MX") \
    X(DNS_DEC_TYPE_TXT,   "TXT") \
    X(DNS_DEC_TYPE_AAAA,  "AAAA") \
    X(DNS_DEC_TYPE_SRV,   "SRV") \
    X(DNS_DEC_TYPE_OPT,   "OPT") \
    X(DNS_DEC_TYPE_ANY,   "ANY") 

#define DNS_DECODE_ENUM(NAME, TEXT) NAME,
#define DNS_DECODE_TEXT(NAME, TEXT) [NAME] = TEXT,

enum dns_dec_code {
    DNS_DECODES(DNS_DECODE_ENUM)
};

static const char *dec_code_strs[] = {
    DNS_DECODES(DNS_DECODE_TEXT)
};

static const char *dec_code_tostr(int code)
{
    const char *str = ec_tostr(ARRAY(dec_code_strs), code, NULL);
    if (str) return str;

    return int_tostr(code);
}

const char *dns_class_tostr(int ec)
{
    if (ec == 1) return "IN";
    if (ec == 3) return "CH";
    if (ec == 4) return "HS";
    if (ec == 254) return "NONE";
    if (ec == 255) return "*";

    return int_tostr(ec);
};

const char *dns_type_tostr(int ec)
{
    if (ec == 1)  return "A";
    if (ec == 2)  return "NS";
    if (ec == 5)  return "CNAME";
    if (ec == 6)  return "SOA";
    if (ec == 12) return "PTR";
    if (ec == 15) return "MX";
    if (ec == 16) return "TXT";
    if (ec == 28) return "AAAA";
    if (ec == 33) return "SRV";
    if (ec == 41) return "OPT";
    if (ec == 252) return "AXFR";
    if (ec == 255)  return "ANY";

    return int_tostr(ec);
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
    size_t avail = strbuf_avail(buf);
    char *ptr = strbuf_pos(buf);

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
        if (!strbuf_avail(&dec->emsg)) {
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

// Required functions
int parse_dns_header(const uint8_t *buf, size_t len, struct dns_header *hdr)
{
    if (len < DNS_HDR_LEN) {
        // too small
        return DNS_ERR_HDRLEN;
    }

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

// return bytes written to out buffer or error code (-ec)
int parse_dns_name(
    const uint8_t *pkt, size_t pkt_len, size_t offset, 
    char *out, size_t out_len, 
    size_t *bytes_consumed)
{
    size_t pkt_idx = offset;
    int njmp = 0;
    int len = 0;
    size_t out_space = out_len;
    *bytes_consumed = 0;

    while (pkt_idx < pkt_len) {
        // first byte is the label length
        size_t len = pkt[pkt_idx++];
        // compression pointer - rfc1035 - 4.1.4. Message compression  
        if ((len & DNS_COMP_PTR) == DNS_COMP_PTR) {
            if (pkt_idx == pkt_len)   return -DNS_ERR_BADJMP; 
            if (njmp++ > DNS_MAX_JMP) return -DNS_ERR_MAXJMP;
            if (njmp == 1) *bytes_consumed += 2;
            // convert to jmp position
            len = ((len & 0x3F) << 8) | pkt[pkt_idx];
            if (len < 12)      return -DNS_ERR_BADJMP;
            if (len > pkt_len) return -DNS_ERR_BADJMP;
            // jmp to position
            pkt_idx = len;
            continue;
        }

        // label - len (0-63)
        size_t pkt_rem = pkt_len - pkt_idx;
        if (len > pkt_rem) return -DNS_ERR_NAMELEN;
        if (len > out_len) return -DNS_ERR_OUTLEN; 
        if (!njmp) *bytes_consumed += 1 + len;

        // null check
        if (len == 0) break;
    
        // copy label
        if (out) {
            memcpy(out, pkt + pkt_idx, len);
            out += len;
        }
        out_len -= len;
        pkt_idx += len;

        // add a dot    
        if (!out_len) return -DNS_ERR_OUTLEN;
        if (pkt_idx < pkt_len && pkt[pkt_idx] != 0) {
            // store the dot
            if (out) *out++ = '.';
            out_len--;
        }
    }

    // did we stop at 0
    if (len != 0) return -DNS_ERR_NONULL;

    // null-terminate
    if (!out_len) return -DNS_ERR_OUTLEN;
    if (out) {
        *out = '\0';
        out++;
        out_len--;
    }

    // num bytes writen
    return out_space - out_len;
}

// print dns_quest to str - returns bytes written or error
int dns_quest_tostr(struct dns_quest *quest, char *buf, size_t buf_len)
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

// print dns_rec to string - return bytes written or error
int dns_rec_tostr(struct dns_rec *rec, int sc, char *buf, size_t buf_len)
{
    // ensure no hidden fields
    const char *name = str_def(rec->name, "<null>");
    const char *class_str = dns_class_tostr(rec->class);
    const char *type_str = dns_type_tostr(rec->type);

    char *wptr = buf;
    char *wend = wptr + buf_len;

    // hdr
    int rc = snprintf(wptr, wend - wptr, "  %s %s %s ", name, class_str, type_str);
    if (rc < 0) return log_errno_rf("snprintf failed for %s", dec_code_tostr(sc));
    wptr += rc;

    // rdata
    switch(rec->type) {
    case DNS_TYPE_A: // IP4 address
        if (!inet_ntop(AF_INET, rec->rdata.a, wptr,wend - wptr)) {
            log_errno("inet_ntop failed to decode IPv4 addr");
            break;
        }
        wptr += strlen(wptr);
        break;
    case DNS_TYPE_NS: // Authoritative Name Server
        rc = snprintf(wptr,wend - wptr, "%s", rec->rdata.ns_name);
        if (rc < 0) return log_errno_rf("snprintf failed");
        wptr += rc;
        break;
    case DNS_TYPE_CNAME: // Canonical Name (Alias)
        rc = snprintf(wptr,wend - wptr, "%s", rec->rdata.cname);
        if (rc < 0) return log_errno_rf("snprintf failed");
        wptr += rc;
        break;
    case DNS_TYPE_SOA: // Start of Authority
        // name + name + 5 integers
        rc = snprintf(wptr,wend - wptr, 
            "MNAME=%s RNAME=%s %u %u %u %u %u",
            rec->rdata.soa.mname,
            rec->rdata.soa.rname,
            rec->rdata.soa.serial,
            rec->rdata.soa.refresh,
            rec->rdata.soa.retry,
            rec->rdata.soa.expire,
            rec->rdata.soa.min_ttl);
        if (rc < 0) return log_errno_rf("snprintf failed");
        wptr += rc;
        break;
    case DNS_TYPE_PTR: // Domain Name Pointer (Reverse DNS)
        rc = snprintf(wptr,wend - wptr, "%s", rec->rdata.ptr_name);
        if (rc < 0) return log_errno_rf("snprintf failed");
        wptr += rc;
        break;
    case DNS_TYPE_HINFO: // Host Information
        rc = snprintf(wptr,wend - wptr, "cpu=%s os=%s", 
            rec->rdata.hinfo.cpu_str,
            rec->rdata.hinfo.os_str);
        if (rc < 0) return log_errno_rf("snprintf failed");
        wptr += rc;
        break;
    case DNS_TYPE_MX: // Mail Exchange 
        rc = snprintf(wptr,wend - wptr, 
            "Pref %d MX %s", rec->rdata.mx.pref, rec->rdata.mx.name);
        if (rc < 0) return log_errno_rf("snprintf failed");
        wptr += rc;
        break;
    case DNS_TYPE_TXT:
        for (int i = 0; i < rec->rdata.txt.num_str; i++) {
            char *str = rec->rdata.txt.str[i];
            rc = snprintf(wptr,wend - wptr, "%s", str);
            if (rc < 0) return log_errno_rf("snprintf failed");
            wptr += rc;
        }
        break;
    case DNS_TYPE_AAAA:// IPv6 Address
        if (!inet_ntop(AF_INET6, rec->rdata.aaaa, wptr,wend - wptr)) {
            log_errno("inet_ntop failed to decode IPv6 addr");
            break;
        }
        wptr += strlen(wptr);
        break;
    case DNS_TYPE_SRV:
        rc = snprintf(wptr,wend - wptr, 
            "Priority %d Weight %d Port %d SRV %s", 
            rec->rdata.srv.prior, 
            rec->rdata.srv.weight, 
            rec->rdata.srv.port,
            rec->rdata.srv.name);
        if (rc < 0) return log_errno_rf("snprintf failed");
        wptr += rc;
        break;
    default:
        break;
    }

    // bytes written
    return wptr - buf;
}

// add dns section as string to buffer
int dns_sect_tostr(struct dns_sect *sect, int sc, char *buf, size_t buf_len)
{
    size_t nw = 0;
        
    for (size_t i = 0; i < sect->num_rec; i++) {
        if (i > 0) {
            if (nw == buf_len) return DNS_FAIL;
            buf[nw++] = '\n';
        }
        int rc = dns_rec_tostr(&sect->rec[i], sc, buf + nw, buf_len - nw);
        if (rc < 0) return rc;
        nw += rc;
    }

    return nw;
}

// add all dns sections as string to buffer
int dns_msg_sects_tostr(struct dns_msg *msg,  char *buf, size_t len)
{
    int rc, nw = 0;

    rc = dns_sect_tostr(&msg->an_recs, DNS_DEC_ANSWER, buf, len);
    if (rc < 0) return rc;
    nw += nw;

    rc = dns_sect_tostr(&msg->ns_recs, DNS_DEC_AUTHORITY,buf, len);
    if (rc < 0) return rc;
    nw += nw;

    rc = dns_sect_tostr(&msg->ar_recs, DNS_DEC_ADDITIONAL,buf, len);
    if (rc < 0) return rc;
    nw += nw;

    return nw;
}

// add name to msg store
static char *msg_store_name_str(struct dns_msg *msg, const char *name, size_t len)
{
    // space for name + nul ?
    if (msg->names_len + len + 1 > sizeof(msg->names)) {
        errno = ENOBUFS;
        return NULL;
    }

    // copy name
    char *str = msg->names + msg->names_len;
    memcpy(str, name, len);
    str[len] = '\0';
    msg->names_len += len + 1;

    return str;
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

static char *msg_store_name(struct dns_msg *msg, const char *name)
{
    return msg_store_name_str(msg, name, safe_strlen(name));
}

// decode a DNS resource record (RR) into a section dns_rec
static int parse_record(struct dns_dec *dec, struct dns_msg *msg, 
    int sect_code, struct dns_sect *sect)
{
    // grab next section rec
    struct dns_rec *rec = NULL;
    if (dec->load_msg) {
        if (sect->num_rec >= ARR_LEN(sect->rec)) {
            return log_error_rf("No space to store %d record", sect_code);
        }
        rec = &sect->rec[sect->num_rec];
    }

    // decode name
    char *wptr = dec->msg;
    char *wend = wptr + sizeof(dec->msg);
    char *name = wptr;
    size_t len;
    int rc = parse_dns_name(dec->pkt_buf, dec->pkt_len, dec->offset, name, wend - wptr,  &len);
    if (rc < 0) return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_NAME, -rc);
    rc = fix_label(rc, wptr, wend);
    if (rc < 0) return rc;
    dec->offset += len;
    wptr += rc + 1;

    // store rdata
    if (rec) {
        rec->name = msg_store_name(msg, name);
        if (!rec->name) {
            return log_errno_rf("No space to store record name");
        }
    }

    // decode type, class, ttl, rdlen
    len = 2 + 2 + 4 + 2;
    if (dec->offset + len > dec->pkt_len) {
        return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_HDR, DNS_ERR_FLDPKTLEN);
    }
    uint16_t rr_type  = dec_u16(dec->pkt_buf + dec->offset + 0);
    uint16_t rr_class = dec_u16(dec->pkt_buf + dec->offset + 2);
    uint32_t rr_ttl   = dec_u32(dec->pkt_buf + dec->offset + 4);
    uint16_t rdlen    = dec_u16(dec->pkt_buf + dec->offset + 8);
    dec->offset += len;

    if (rec) {
        rec->type  = rr_type;
        rec->class = rr_class;
        rec->ttl   = rr_ttl;
        rec->rdlen = rdlen;
    }

    // RDATA
    if (dec->offset + rdlen > dec->pkt_len) {
        return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_RDATA, DNS_ERR_FLDTRUNC);
    }
    const uint8_t *rdata = dec->pkt_buf + dec->offset;
    const char *rdata_str = wptr;
    const char *rdata_desc = "";
    size_t ridx = 0;

    // decode rdata
    switch(rr_type) {
    case DNS_TYPE_A: // IP4 address
        if (rdlen != 4) {
            return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_A, DNS_ERR_MINLEN);
        }
        ridx += 4;
        // store rdata
        if (rec) memcpy(rec->rdata.a, rdata, rdlen);
        // describe
        if (dec->need_emsg) {
            if (!inet_ntop(AF_INET, rdata, wptr,wend - wptr)) {
                return log_errno_rf("inet_ntop failed to decode IPv4 addr");
            }
            rdata_desc = rdata_str;
            wptr += 4;
        }
        break;
    case DNS_TYPE_NS: //  Authoritative Name Server
        if (rdlen < 1) {
            return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_NS, DNS_ERR_MINLEN);
        }
        // decode name
        rc = parse_dns_name(dec->pkt_buf, dec->pkt_len, dec->offset, wptr,wend - wptr,  &len);
        if (rc < 0) return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_NS, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        ridx += len;
        // store rdata
        if (rec) {
            rec->rdata.ns_name = msg_store_name(msg, wptr);
            if (!rec->rdata.ns_name) {
                return log_errno_rf("No space to store ns_name");
            }
        }
        wptr += rc;
        rdata_desc = rdata_str;
        break;
    case DNS_TYPE_CNAME: // Canonical Name (Alias)
        // decode name
        rc = parse_dns_name(dec->pkt_buf, dec->pkt_len, dec->offset, wptr, wend - wptr, &len);
        if (rc < 0) return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_CNAME, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        ridx += len;
        // store rdata
        if (rec) {
            rec->rdata.cname = msg_store_name(msg, wptr);
            if (!rec->rdata.cname) {
                return log_errno_rf("No space to store cname");
            }
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
        rc = parse_dns_name(dec->pkt_buf, dec->pkt_len, dec->offset, wptr, wend - wptr, &len);
        if (rc < 0) return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_SOA, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        ridx += len;
        // store rdata
        if (rec) {
            rec->rdata.soa.mname = msg_store_name(msg, wptr);
            if (!rec->rdata.soa.mname) {
                return log_errno_rf("No space to store mname");
            }
        }
        wptr += rc;

        // Responsible Person's Email - RNAME
        if (dec->need_emsg) {
            rc = snprintf(wptr,wend - wptr, " RNAME=");
            if (rc < 0) log_errno_rf("snprintf SOA:RNAME failed");
            wptr += rc;
        }
        // decode name
        rc = parse_dns_name(dec->pkt_buf, dec->pkt_len, dec->offset + ridx, wptr,wend - wptr, &len);
        if (rc < 0) return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_SOA, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        ridx += len;
        // store rdata
        if (rec) {
            rec->rdata.soa.rname = msg_store_name(msg, wptr);
            if (!rec->rdata.soa.mname) {
                return log_errno_rf("No space to store rname");
            }
        }
        wptr += rc;

        // decode serial,refresh,retry,expire,mininum (5 x 32 bit ints)
        if (ridx + 20 > rdlen) {
            return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_SOA, DNS_ERR_FLDTRUNC);
        }
        char *names[5] = { "serial","refresh", "retry", "expire", "min_ttl" };
        uint32_t vals[5];
        memcpy(vals, rdata + ridx, 20);
        ridx += 20;
        for (int i = 0; i < 5; i++) {
            vals[i] = ntohl(vals[i]);
            if (dec->need_emsg) {
                rc = snprintf(wptr,wend - wptr, " %s=%u", names[i], vals[i]);
                if (rc < 0) log_errno_rf("snprintf SOA:%s failed", names[i]);
                wptr += rc;
            }
        }
        // store rdata
        if (rec) {
            rec->rdata.soa.serial  = vals[0];
            rec->rdata.soa.refresh = vals[1];
            rec->rdata.soa.retry   = vals[2];
            rec->rdata.soa.expire  = vals[3];
            rec->rdata.soa.min_ttl = vals[4];
        }
        // decoded
        rdata_desc = rdata_str;
        break;
    }
    case DNS_TYPE_PTR: // Domain Name Pointer (Reverse DNS)
        // decode name
        rc = parse_dns_name(dec->pkt_buf, dec->pkt_len, dec->offset, wptr, wend - wptr, &len);
        if (rc < 0) return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_CNAME, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        ridx += len;
        // store rdata
        if (rec) {
            rec->rdata.ptr_name = msg_store_name(msg, wptr);
            if (!rec->rdata.ptr_name) {
                return log_errno_rf("No space to store PTR");
            }
        }
        wptr += rc;
        rdata_desc = rdata_str;
        break;
    case DNS_TYPE_HINFO: // Host Information
        // cpu-str + os-str
        if (rdlen < 2) {
            return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_HINFO, DNS_ERR_MINLEN);
        }
        // cpu-str
        len = rdata[ridx++];
        if (len + 1 > rdlen) {
            return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_HINFO, DNS_ERR_FLDRDLEN);
        }
        // describe
        if (dec->need_emsg) {
            rc = snprintf(wptr,wend - wptr, " cpu=%.*s", (int) len, rdata + ridx);
            if (rc < 0) log_errno_rf("snprintf HINFO:CPU failed");
            wptr += rc;
        }
        // store rdata
        if (rec) {
            rec->rdata.hinfo.cpu_str = msg_store_name_str(msg, (char *) rdata + ridx, len);
            if (!rec->rdata.hinfo.cpu_str) {
                return log_errno_rf("No space to store HINFO:CPU");
            }
        }
        ridx += len;

        // os-str
        if (ridx >= rdlen)  {
            return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_HINFO, DNS_ERR_FLDTRUNC);
        }
        len = rdata[ridx++];
        if (ridx + len > rdlen) {
            return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_HINFO, DNS_ERR_FLDRDLEN);
        }
        // describe
        if (dec->need_emsg) {
            rc = snprintf(wptr,wend - wptr, " os-str=%.*s", (int) len, rdata + ridx);
            if (rc < 0) log_errno_rf("snprintf HINFO:CPU failed");
            wptr += rc;
        }
        // store rdata
        if (rec)  {
            rec->rdata.hinfo.os_str = msg_store_name_str(msg, (char *) rdata + ridx, len);
            if (!rec->rdata.hinfo.os_str) {
                return log_errno_rf("No space to store HINFO:OS");
            }
        }
        ridx += len;
        // decoded
        rdata_desc = rdata_str;
        break;
    case DNS_TYPE_MX: { // Mail Exchange 
        if (rdlen < 3) {
            return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_MX, DNS_ERR_MINLEN);
        }
        // Preference
        uint16_t pref = dec_u16(rdata);
        ridx += 2;
        if (dec->need_emsg) {
            rc = snprintf(wptr,wend - wptr, "pref %d ", pref);
            if (rc < 0) log_errno_rf("snprintf MX pref failed");
            wptr += rc;
        }
        // Mail Server Name
        rc = parse_dns_name(dec->pkt_buf, dec->pkt_len, dec->offset + ridx, wptr,wend - wptr, &len);
        if (rc < 0) return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_MX, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        ridx += len;
        // store rdata
        if (rec) {
            rec->rdata.mx.pref = pref;
            rec->rdata.mx.name = msg_store_name(msg, wptr);
            if (!rec->rdata.mx.name) {
                return log_errno_rf("No space to store mx_name");
            }
        }
        wptr += rc;
        // decoded
        rdata_desc = rdata_str;
        break;
    }
    case DNS_TYPE_TXT: // Text Strings
        while (ridx < rdlen) {
            // Length
            len = rdata[ridx++];
            if (ridx + len > rdlen) {
                return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_TXT, DNS_ERR_FLDRDLEN);
            }
            // describe
            if (dec->need_emsg)  {
                rc = snprintf(wptr,wend - wptr, " txt=%.*s", (int) len, rdata + ridx);
                if (rc < 0) log_errno_rf("snprintf HINFO:CPU failed");
                wptr += rc;
            }
            // store rdata
            if (rec) {
                if (rec->rdata.txt.num_str >= ARR_LEN(rec->rdata.txt.str)) {
                    return log_errno_rf("No space for TXT entry");
                }
                char *txt = make_ptr(rdata, ridx);
                rec->rdata.txt.str[rec->rdata.txt.num_str] = msg_store_name_str(msg, txt, len);
                if (!rec->rdata.txt.str[rec->rdata.txt.num_str]) {
                    return log_errno_rf("No space to store TXT str");
                }
                rec->rdata.txt.num_str++;
            }
            ridx += len;
        }
        // decoded
        rdata_desc = rdata_str;
        break;
    case DNS_TYPE_AAAA:  // IPv6 Address
        if (rdlen != 16) {
            return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_AAAA, DNS_ERR_MINLEN);
        }
        ridx += 16;
        // store rdata
        if (rec) memcpy(rec->rdata.aaaa, rdata, rdlen);
        // describe
        if (dec->need_emsg) {
            if (!inet_ntop(AF_INET6, rdata, wptr,wend - wptr)) {
                return log_errno_rf("inet_ntop failed to decode IPv6 addr");
            }
            wptr += rdlen;
            rdata_desc = rdata_str;
        }
        break;
    case DNS_TYPE_SRV: { // Service Locator
        // 2 + 2 + 2 + name
        if (rdlen < 7) {
            return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_SRV, DNS_ERR_MINLEN);
        }
        // decode 3 x uint16
        uint16_t prior  = dec_u16(dec->pkt_buf + dec->offset + 0);
        uint16_t weight = dec_u16(dec->pkt_buf + dec->offset + 2);
        uint16_t port   = dec_u16(dec->pkt_buf + dec->offset + 4);
        ridx += 6;
        // describe
        if (dec->need_emsg) {
            rc = snprintf(wptr,wend - wptr, "Priority %d Weight %d port %d ", prior, weight, port);
            if (rc < 0) log_errno_rf("snprintf SRV Priority");
            wptr += rc;
        }
        // name
        rc = parse_dns_name(dec->pkt_buf, dec->pkt_len, dec->offset + ridx, wptr,wend - wptr, &len);
        if (rc < 0) return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_SRV, -rc);
        rc = fix_label(rc, wptr, wend);
        if (rc < 0) return rc;
        ridx += len;
        // store rdata
        if (rec) {
            rec->rdata.srv.prior = prior;
            rec->rdata.srv.weight = weight;
            rec->rdata.srv.port = port;
            rec->rdata.srv.name = msg_store_name(msg, wptr);
            if (!rec->rdata.srv.name) {
                return log_errno_rf("No space to store srv_name");
            }
        }
        wptr += rc;
        // decoded
        rdata_desc = rdata_str;
        break;
    }
    case DNS_TYPE_OPT: { // EDNS0 Options (RFC 6891)
        if (sect_code != DNS_DEC_ADDITIONAL) {
            return dns_dec_err(dec, DNS_DEC_RECORD, DNS_DEC_TYPE_OPT, DNS_ERR_OPTSECT);
        }
        ridx = dec->offset - 8;

        // decode
        rr_class = 0;
        uint16_t udp_size = dec_u16(dec->pkt_buf + ridx);
        uint32_t ttl_val  = dec_u32(dec->pkt_buf + ridx + 2);
        uint8_t ext_rcode = (ttl_val >> 24) & 0xFF;
        uint8_t version   = (ttl_val >> 16) & 0xFF;
        uint8_t do_bit   = (ttl_val & 0x8000);

        // store rdata
        if (rec) {
            rec->rdata.opt.udp_size = udp_size;
            rec->rdata.opt.ttl_val = ttl_val;
            rec->rdata.opt.ext_rcode = ext_rcode;
            rec->rdata.opt.edns_ver = version;
            rec->rdata.opt.do_bit = do_bit;
            rec->class = 0;
            rec->name = msg_store_name(msg, name);
            if (!rec->name) {
                return log_errno_rf("No space to store root");
            }
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

        // decoded
        rdata_desc = rdata_str;
        break;
    }
    case DNS_TYPE_ANY: // Wildcard match (Query only) 
        break;
    default:
        break;
    }

    // rdata decoded - TODO check if ridx != rdlen
    dec->offset += rdlen;
    // next record
    if (rec && sect) {
        sect->num_rec++;
    }

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

// decode a question record
static int parse_question(struct dns_dec *dec, struct dns_msg *msg)
{
    // grab next section quest
    struct dns_quest *quest = NULL;
    if (dec->load_msg) {
        if (msg->num_qd >= ARR_LEN(msg->qd_recs)) {
            return log_error_rf("No space to store question");
        }
        quest = &msg->qd_recs[msg->num_qd];
    }

    char *wptr = dec->msg;
    char *wptr_end = wptr + sizeof(dec->msg);

    // decode name
    char *name = wptr;
    size_t consumed;
    int ec = parse_dns_name(dec->pkt_buf, dec->pkt_len, dec->offset, name, wptr_end - wptr, &consumed);
    if (ec < 0) return dns_dec_err(dec, DNS_DEC_QUESTION, DNS_DEC_NAME, -ec);
    wptr += ec;
    dec->offset += consumed;

    // decode qtype, qclass
    if (dec->offset + 4 > dec->pkt_len) {
        return dns_dec_err(dec, DNS_DEC_QUESTION, DNS_DEC_HDR, DNS_ERR_FLDPKTLEN);
    }
    uint16_t qtype  = dec_u16(dec->pkt_buf + dec->offset);
    uint16_t qclass = dec_u16(dec->pkt_buf + dec->offset + 2);
    dec->offset += 4;

    // load question
    if (quest)  {
        quest->qtype = qtype;
        quest->qclass = qclass;
        quest->qname = msg_store_name(msg, name);
        if (!quest->qname) {
            return log_errno_rf("No space to store question name");
        }
        msg->num_qd++;
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
static int dns_dec_sect(struct dns_dec *dec, struct dns_msg *msg,
    int nrec, int sect_code, struct dns_sect *sect)
{
    for (int i = 0; i < nrec; i++) {
        if (parse_record(dec, msg, sect_code, sect) != 0) {
            return dns_dec_err(dec, DNS_DEC_PDU, sect_code, i + DNS_EC_ISROW);
        }
    }

    return 0;
}

// decode additional section
static int decode_ar(struct dns_dec *dec, struct dns_msg *msg)
{
    return dns_dec_sect(dec, msg, msg->hdr.ar_count, DNS_DEC_ADDITIONAL, &msg->ar_recs);
}

// decode authority section
static int decode_ns(struct dns_dec *dec, struct dns_msg *msg)
{
    return dns_dec_sect(dec, msg, msg->hdr.ns_count, DNS_DEC_AUTHORITY, &msg->ns_recs);
}

// decode answer section
static int decode_an(struct dns_dec *dec, struct dns_msg *msg)
{
    return dns_dec_sect(dec, msg, msg->hdr.an_count, DNS_DEC_ANSWER, &msg->an_recs);
}

// decode question section
static int decode_qd(struct dns_dec *dec, struct dns_msg *msg)
{
    for (int i = 0; i < msg->hdr.qd_count; i++) {
        if (parse_question(dec, msg) != 0) {
            return dns_dec_err(dec, DNS_DEC_PDU, DNS_DEC_QUESTION, i + DNS_EC_ISROW);
        }
    }

    return 0;
}

// decode DNS header
static int decode_hdr(struct dns_dec *dec, struct dns_msg *msg)
{
    struct dns_header *hdr = &msg->hdr;

    // read header
    int rc = parse_dns_header(dec->pkt_buf, dec->pkt_len, hdr);
    if (rc != 0) {
        return dns_dec_err(dec, DNS_DEC_PDU, DNS_DEC_HDR, rc);
    }
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
            dns_dec_err(dec, DNS_DEC_PDU, DNS_DEC_HDR, DNS_ERR_BADRCODE);
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
            dns_dec_err(dec, DNS_DEC_PDU, DNS_DEC_HDR, DNS_ERR_BADOPCODE);
        }
    }

    // desc PDU as we decode
    rc = dns_wmsg(dec,
        "[%s] ID 0x%04x QR:%d OPCODE:%s %.*s\n",
        type_str, hdr->id, qr, opcode_str, 
        (int) strbuf_used(&buf), strbuf_start(&buf));

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

// Required functions
int validate_dns_packet(const uint8_t *pkt_buf, size_t pkt_len, char *emsg)
{
    struct dns_dec dec = {
        .pkt_buf = pkt_buf,
        .pkt_len = pkt_len,
        .udp_size = DNS_MAX_UDP,
        .need_emsg = 1,
        .emsg = STRBUF_INIT(emsg, DNS_EMSG_MAXLEN)
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

// encode a dns name
static uint8_t *enc_name(struct dns_enc *enc, uint8_t *wptr, const char *name)
{
    int offset = wptr - enc->pkt_buf;
    const char *name_end = name + strlen(name);
    
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
static int dns_enc_quest(struct dns_enc *enc, struct dns_quest *quest)
{
    size_t len = DNS_NAME_MAXLEN + 4;

    uint8_t *wbuf = dns_enc_mkspace(enc, len);
    if (!wbuf) {
        return log_error_rf("No room for question len %zu", len);
    }

    // encode
    uint8_t *wptr = wbuf;
    wptr = enc_name(enc, wptr, quest->qname);
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
    wptr = enc_name(enc, wptr, name);
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

// encode an RR record into packet buffer
static int encode_rec(struct dns_enc *enc, struct dns_rec *rec, int sc)
{
    // add hdr (name|type|class|ttl|rdlength
    int len = DNS_NAME_MAXLEN + 2 + 2 + 4 + 2;
    uint8_t *wbuf = enc_fld_mkspace(enc, len, sc, 0);
    if (!wbuf) return -1;

    uint8_t *wptr = wbuf;
    wptr = enc_name(enc, wptr, rec->name);
    if (!wptr) return -1;
    wptr = enc_u16(wptr, rec->type);
    wptr = enc_u16(wptr, rec->class);
    wptr = enc_u32(wptr, rec->ttl);
    uint8_t *rdlen_ptr = wptr;
    wptr = enc_u16(wptr,  0);
    size_t used = wptr - wbuf;
    int rc = dns_enc_retspace(enc, len - used);
    if (rc) return rc;
    uint16_t pkt_len = enc->pkt_len;

    // add data
    switch(rec->type) {
    case DNS_TYPE_A: 
        rc = dns_enc_mem(enc, rec->rdata.a, sizeof(rec->rdata.a), sc, rec->type);
        if (rc) return rc;
        break;
    case DNS_TYPE_NS:
        rc = dns_enc_name(enc, rec->rdata.ns_name, sc, rec->type);
        if (rc) return rc;
        break;
    case DNS_TYPE_CNAME: // Canonical Name (Alias)
       rc = dns_enc_name(enc, rec->rdata.cname, sc, rec->type);
       if (rc) return rc;
       break;
    case DNS_TYPE_SOA:  // Start of Authority
        // Primary Master Name Server - MNAME
        rc = dns_enc_name(enc, rec->rdata.soa.mname, sc, rec->type);
        if (rc) return rc;
        rc = dns_enc_name(enc,rec->rdata.soa.rname, sc, rec->type);
        if (rc) return rc;
        //  encode 5 fields
        len = 5 * sizeof(uint32_t);
        wptr = enc_fld_mkspace(enc, len, sc, rec->type);
        if (!wptr) return -1;
        wptr = enc_u32(wptr, rec->rdata.soa.serial);
        wptr = enc_u32(wptr, rec->rdata.soa.refresh);
        wptr = enc_u32(wptr, rec->rdata.soa.retry);
        wptr = enc_u32(wptr, rec->rdata.soa.expire);
        wptr = enc_u32(wptr, rec->rdata.soa.min_ttl);
        break;
    case DNS_TYPE_PTR: // Domain Name Pointer (Reverse DNS)
        rc = dns_enc_name(enc, rec->rdata.ptr_name, sc, rec->type);
        if (rc) return rc;
        break;
    case DNS_TYPE_HINFO: { // Host Information
        size_t len_cpu = safe_strlen(rec->rdata.hinfo.cpu_str);
        size_t len_os  = safe_strlen(rec->rdata.hinfo.os_str);
        wptr = enc_fld_mkspace(enc, 2 + len_cpu + len_os, sc, rec->type);
        if (!wptr) return -1;
        *wptr++ = len_cpu;
        if (len_cpu)  memcpy(wptr, rec->rdata.hinfo.cpu_str, len_cpu);
        *wptr++ = len_os;
        if (len_os) memcpy(wptr, rec->rdata.hinfo.os_str, len_os);
        break;
    }
    case DNS_TYPE_MX: // Mail Exchange 
        len = sizeof(uint32_t);
        wptr = enc_fld_mkspace(enc, len, sc, rec->type);
        if (!wptr) return -1;
        wptr = enc_u16(wptr, rec->rdata.mx.pref);
        rc = dns_enc_name(enc, rec->rdata.mx.name, sc, rec->type);
        if (rc) return rc;
        break;
    case DNS_TYPE_TXT: {
        // encode txt array
        for (int i = 0; i < rec->rdata.txt.num_str; i++) {
            char *str = rec->rdata.txt.str[i];
            len = safe_strlen(str);
            rc = dns_enc_str(enc, str, len, sc, rec->type);
            if (rc) return rc;
        }
        break;
    }
    case DNS_TYPE_AAAA: // IPv6 Address
        len = sizeof(rec->rdata.aaaa);
        rc = dns_enc_mem(enc, rec->rdata.aaaa, len, sc, rec->type);
        if (rc) return rc;
        break;
    case DNS_TYPE_SRV: // Service Locator
        len = sizeof(uint16_t) * 3;
        wptr = enc_fld_mkspace(enc, 2, sc, rec->type);
        if (!wptr) return -1;
        wptr = enc_u16(wptr, rec->rdata.srv.prior);
        wptr = enc_u16(wptr, rec->rdata.srv.weight);
        wptr = enc_u16(wptr, rec->rdata.srv.port);
        rc = dns_enc_name(enc, rec->rdata.srv.name, sc, rec->type);
        if (rc) return rc;
        break;
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
static int dns_enc_sect(struct dns_enc *enc, int nrec, int sc, struct dns_sect *sect)
{
    for (int i = 0; i < nrec; i++) {
        int rc = encode_rec(enc, &sect->rec[i], sc);
        if (rc) return rc;
    }

    return 0;
}

// encode additional section
static int encode_ar(struct dns_enc *enc, struct dns_msg *msg)
{
    return dns_enc_sect(enc, msg->hdr.ar_count, DNS_DEC_ADDITIONAL, &msg->ar_recs);
}

// encode authority section
static int encode_ns(struct dns_enc *enc, struct dns_msg *msg)
{
    return dns_enc_sect(enc, msg->hdr.ns_count, DNS_DEC_AUTHORITY, &msg->ns_recs);
}

// encode answer section
static int encode_an(struct dns_enc *enc, struct dns_msg *msg)
{
    return dns_enc_sect(enc, msg->hdr.an_count, DNS_DEC_ANSWER, &msg->an_recs);
}

// encode question section
static int encode_qd(struct dns_enc *enc, struct dns_msg *msg)
{
    for (int i = 0; i < msg->hdr.qd_count; i++) {
        int rc = dns_enc_quest(enc, &msg->qd_recs[i]);
        if (rc) return rc;
    }

    return 0;
}

// encode a DNS header
static int encode_hdr(struct dns_enc *enc, struct dns_msg *msg)
{
    // sync hdr
    struct dns_header *hdr = &msg->hdr;
    hdr->qd_count = msg->num_qd;
    hdr->an_count = msg->an_recs.num_rec;
    hdr->ns_count = msg->ns_recs.num_rec;
    hdr->ar_count = msg->ar_recs.num_rec;

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

// add a dns_rec to a DNS msg section record
static int dns_msg_add_sect(struct dns_msg *msg,
    int sc, struct dns_sect *sect, struct dns_rec *src_rec)
{
    if (!src_rec) {
        return log_error_rf("Store %s record is <null>", dec_code_tostr(sc));
    }

    // space for record ?
    struct dns_rec *rec;
    if (sect->num_rec >= ARR_LEN(sect->rec)) {
        return log_error_rf("No space to store %s record", dec_code_tostr(sc));
    }
    rec = &sect->rec[sect->num_rec];

    if (!src_rec->name) {
        return log_error_rf("Store %s record name is <null>", dec_code_tostr(sc));
    }

    // store name
    int len = strlen(src_rec->name);
    if (len > DNS_NAME_MAXSTR) {
        return log_error_rf("%s name length %d bigger than max %u", dec_code_tostr(sc), len, DNS_NAME_MAXSTR);
    }

    rec->name = msg_store_name(msg, src_rec->name);
    if (!rec->name) {
        return log_errno_rf("No space to store record name for %s", dec_code_tostr(sc));
    }
    rec->type  = src_rec->type;
    rec->class = src_rec->class;
    rec->ttl   = src_rec->ttl;

    // store rdata
    switch(rec->type) {
    case DNS_TYPE_A: 
        memcpy(rec->rdata.a, src_rec->rdata.a, 4);
        break;
    case DNS_TYPE_NS:
        rec->rdata.ns_name = msg_store_name(msg, src_rec->rdata.ns_name);
        if (!rec->rdata.ns_name) {
            return log_errno_rf("No space to store ns_name for %s", dec_code_tostr(sc));
        }
        break;
    case DNS_TYPE_CNAME: // Canonical Name (Alias)
       rec->rdata.cname = msg_store_name(msg, src_rec->rdata.cname);
       if (!rec->rdata.cname) {
          return log_errno_rf("No space to store cname for %s", dec_code_tostr(sc));
       }
       break;
    case DNS_TYPE_SOA: { // Start of Authority
        // Primary Master Name Server - MNAME
        rec->rdata.soa.mname = msg_store_name(msg, src_rec->rdata.soa.mname);
        if (!rec->rdata.soa.mname) {
            return log_errno_rf("No space to store mname");
        }
        // Responsible Person's Email - RNAME
        rec->rdata.soa.rname = msg_store_name(msg, src_rec->rdata.soa.rname);
        if (!rec->rdata.soa.mname) {
            return log_errno_rf("No space to store rname");
        }
        rec->rdata.soa.serial   = src_rec->rdata.soa.serial;
        rec->rdata.soa.refresh  = src_rec->rdata.soa.refresh;
        rec->rdata.soa.retry    = src_rec->rdata.soa.retry;
        rec->rdata.soa.expire   = src_rec->rdata.soa.expire;
        rec->rdata.soa.min_ttl  = src_rec->rdata.soa.min_ttl;
        break;
    }
    case DNS_TYPE_PTR: // Domain Name Pointer (Reverse DNS)
        rec->rdata.ptr_name = msg_store_name(msg, src_rec->rdata.ptr_name);
        if (!rec->rdata.ptr_name) {
            return log_errno_rf("No space to store PTR");
        }
        break;
    case DNS_TYPE_HINFO: // Host Information
        rec->rdata.hinfo.cpu_str = msg_store_name(msg, src_rec->rdata.hinfo.cpu_str);
        rec->rdata.hinfo.os_str  = msg_store_name(msg, src_rec->rdata.hinfo.os_str);
        break;
    case DNS_TYPE_MX: // Mail Exchange 
        rec->rdata.mx.pref = src_rec->rdata.mx.pref;
        rec->rdata.mx.name = msg_store_name(msg, src_rec->rdata.mx.name);
        if (!rec->rdata.mx.name) {
            return log_errno_rf("No space to store mx_name");
        }
        break;
    case DNS_TYPE_TXT: {
        for (int i = 0; i < src_rec->rdata.txt.num_str; i++) {
            char *src_str = src_rec->rdata.txt.str[i];
            if (rec->rdata.txt.num_str >= ARR_LEN(rec->rdata.txt.str)) {
                return log_errno_rf("No space for TXT entry");
            }
            rec->rdata.txt.str[rec->rdata.txt.num_str] = msg_store_name(msg, src_str);
            if (!rec->rdata.txt.str[rec->rdata.txt.num_str]) {
                return log_errno_rf("No space to store TXT str");
            }
            rec->rdata.txt.num_str++;
        }
        break;
    }
    case DNS_TYPE_AAAA:  // IPv6 Address
        mempcpy(rec->rdata.aaaa, src_rec->rdata.aaaa, 16);
        break;
    case DNS_TYPE_SRV: // Service Locator
        rec->rdata.srv.prior  = src_rec->rdata.srv.prior;
        rec->rdata.srv.weight = src_rec->rdata.srv.weight;
        rec->rdata.srv.port   = src_rec->rdata.srv.port;
        rec->rdata.srv.name   = msg_store_name(msg, src_rec->rdata.srv.name);
        if (!rec->rdata.srv.name) {
            return log_errno_rf("No space to store srv_name");
        }
        break;
    case DNS_TYPE_ANY: // Wildcard match (Query only) 
        break;

    default:
        break;
    }

    // record loaded okay
    sect->num_rec++;

    return 0;
}

// add a question to DNS msg
int dns_msg_add_qd(struct dns_msg *msg, 
    const char *name, size_t len, 
    uint16_t qtype,  uint16_t qclass)
{
    struct dns_quest *quest;
    if (msg->num_qd >= ARR_LEN(msg->qd_recs)) {
        return log_error_rf("No space to store %s record", dec_code_tostr(DNS_DEC_QUESTION));
    }
    quest = &msg->qd_recs[msg->num_qd];
    if (len > DNS_NAME_MAXSTR) {
        return log_error_rf("Name length %zu bigger than max %u", len, DNS_NAME_MAXSTR);
    }

    quest->qtype = qtype;
    quest->qclass = qclass;
    quest->qname = msg_store_name(msg, name);
    if (!quest->qname)  {
        return log_error_rf("No room to store %s name", dec_code_tostr(DNS_DEC_QUESTION));
    }

    // question added
    msg->num_qd++;

    return 0;
}

// add a qd|ns|ar section record to DNS msg
int dns_msg_add_rec(struct dns_msg *msg, int sc, struct dns_rec *rec)
{
    struct dns_sect *sect;

    switch(sc) {
    case DNS_MSG_AN: sect = &msg->an_recs; break;
    case DNS_MSG_NS: sect = &msg->ns_recs; break;
    case DNS_MSG_AR: sect = &msg->ar_recs; break;
    default: return log_error_rf("Unknown section %d", sc);
    }

    return dns_msg_add_sect(msg, sc, sect, rec);
}

// get first dns resource record
struct dns_rec *dns_msg_get_rec(struct dns_msg *msg)
{
    if (dns_msg_cnt_rec(msg) != 1) return NULL;
    if (dns_msg_num_an(msg)) return &msg->an_recs.rec[0];
    if (dns_msg_num_ns(msg)) return &msg->ns_recs.rec[0];
    if (dns_msg_num_ar(msg)) return &msg->ar_recs.rec[0];

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

/*
 * Figure out the record
 * =====================
 *  addr =  ip4addr|ip6addr|regname [<ttl>] [class=IN|CS|CH|HS]
 */
int dns_rec_load(struct dns_rec *rec, int sc, const char *src)
{
    // get addr
    struct str_slice addr  = slice_make_cstr(src);
    struct str_slice attrs = slice_split(&addr, ' ');
    slice_trim(&addr);

    if (addr.len > DNS_NAME_MAXSTR) {
        return log_error_rf("%s <addr> len %zu bigger than max %d", 
            dec_code_tostr(sc), addr.len, DNS_NAME_MAXSTR);
    }

    // need a copy for inet_pton call
    char addr_str[DNS_NAME_MAXLEN];
    memcpy(addr_str, addr.ptr, addr.len);
    addr_str[addr.len] = '\0';

    // parse addr_str (ip4|ip6|name)
    uint8_t addr_raw[DNS_NAME_MAXLEN];
    if (inet_pton(AF_INET, addr_str, addr_raw) == 1) {
        // IPv4
        rec->type = DNS_TYPE_A;
        memcpy(rec->rdata.a, addr_raw, 4);
    }
    else if (inet_pton(AF_INET6, addr_str, addr_raw) == 1) {
        // IPv6
        rec->type = DNS_TYPE_AAAA;
        memcpy(rec->rdata.aaaa, addr_raw, 16);
    }
    else {
        // reg-name
        rec->type = DNS_TYPE_CNAME;
        rec->rdata.cname = addr_str;
    }

    // default class to Internet
    rec->class = DNS_CLASS_IN;
    
    // look for remaining attrs (e.g 3600 CH)
    while (attrs.len) {
        struct str_slice attr = slice_split(&attrs, ' ');
        char name[20];
        slice_trim(&attr);
        // covert slice to cptr
        size_t len = min(attr.len, sizeof(name) - 1);
        memcpy(name, attr.ptr, len);
        name[len] = '\0';
        // lookup code
        int dns_class = dns_get_class(name);
        if (dns_class != 0) {
            rec->class = dns_class;
        }
        else if (slice_isnumeric(attr)) {
            rec->ttl = atol(attr.ptr);
        }
    }

    // all done
    return 0;
}
