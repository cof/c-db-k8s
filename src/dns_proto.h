/*
 * A DNS message codec API
 * -----------------------
 * A DNS codec API for reading and writing DNS messages featuring
 * - Structure-composable: built for inline embedding, object compostion & memory locality
 * - full rfc1035 codec support for encoding/decoding wire-format DNS messages.
 * - provides a DNS message structure for easy message generation
 * - Human-readable formatting of decoded DNS messages
 * - Validation service of PDUS with dns section error reporting
 *
 * Example Usage:
 * --------------
 *
 *  // create a query msg
 *  char buf[BUFSIZ];
 *  struct dns_msg msg;
 *  dns_msg_init(&msg, 0x1234, DNS_FLAGS_RD);
 *  int rc = dns_add_qd(&msg, "example.com", DNS_TYPE_A, DNS_CLASS_IN);
 *  ssize_t pkt_len = dns_msg_encode(&msg, buf, sizeof(buf));
 *  // buffer now has wire-format dns query
 *
 * DNS message
 * -----------
 * API uses a dns message structure to allow users easily set or get fields.
 *
 *  struct dns_msg 
 *   - hdr (id, flags, qd_count, an_count, ns_count, ar_count)
 *   - qd_recs - question section 
 *   - an_recs - answer section 
 *   - ns_recs - authority section
 *   - ar_recs - additional section
 *
 * User can use helper functions to set fields or set them directly.
 *
 * Basic API
 * ----------
 * dns_hdr_decode(hdr, buf, len) : decode buffer into dns header
 * dns_msg_reset(msg) : reset fields to 0
 * dns_msg_decode(msg, buf, len) : decode buffer into a DNS message
 * dns_msg_encode(msg, buf, len) : encode DNS message into buffer
 * dns_validate(pkt_buf, pkt_len, emsg, emsg_len) : check pkt valid and print desc to esmg
 *
 * Helpers
 * --------
 * dns_msg_add_qdn(msg, qname, len, qtype, qclass): add to qd section 
 * dns_msg_add_qd(msg, qname, qtype, qclass) : add to qd section - no qname len 
 * dns_add_rr(msg, sect, rr) : add rr to a section an|ns|ar
 * dns_msg_sects_tostr(msg,buf,len) : print sections to str buffer
 * -
 * dns_msg_get_rec(msg) : get first rec if available
 * dns_msg_cnt_rec(mg) - count total records in msg
 * dns_rr_load(rr, sc, str) - load text form of rr into rr
 *
 * References
 * ----------
 * rfc1035 - DOMAIN NAMES - IMPLEMENTATION AND SPECIFICATION
 * rfc6891 - Extension Mechanisms for DNS (EDNS(0))
 */
#ifndef _DNS_PROTO_H_
#define _DNS_PROTO_H_

// Protocol Size limits
#define DNS_NAME_MAXLEN  255  // names  255 octets or less
#define DNS_NAME_MAXSTR  253  // 255 - len(1) - nul(1) = 253
#define DNS_LABEL_MAXSTR 63   // 63 octets or less
#define DNS_HDR_LEN      12   // header size
#define DNS_MAX_UDP     512   // can be overriden by EDNS

// zero-length label
#define DNS_NULL_STR "."
#define DNS_ROOT_STR "<Root>"

// Our limits
#define DNS_MAX_PDUSIZE  2048
#define DNS_MAX_QD          1 // max question data
#define DNS_MAX_RR         32 // max record
#define DNS_MAX_TXT        32 // max txt string
#define DNS_COMP_PTR     0xC0 // 1100000 upper 2 bits
#define DNS_MAX_JMP        16 // max number of compression pointer jmps
#define DNS_MAX_SUFFIX     32 // max number of compression names

#define DNS_FAIL -1

// DNS header flags
#define DNS_FLAGS_QR      0x8000 // query response
#define DNS_FLAGS_OPCODE  0x7800 // 4 bit Opcode (0 .. 5)
#define DNS_FLAGS_CD      0x0800 // Checking Disabled (DNSSEC)
#define DNS_FLAGS_AA      0x0400 // Authoritative Answer
#define DNS_FLAGS_TC      0x0200 // Truncated
#define DNS_FLAGS_AD      0x0020 // Authentic Data
#define DNS_FLAGS_RD      0x0100 // Recursion Desired
#define DNS_FLAGS_RA      0x0080 // Recursion Available
#define DNS_FLAGS_RCODE   0x000F // 4 bit Response Code

// DNS opcodes
#define DNS_OPCODE_QUERY  0
#define DNS_OPCODE_IQUERY 1
#define DNS_OPCODE_STATUS 2
#define DNS_OPCODE_NOTIFY 4
#define DNS_OPCODE_UPDATE 5

// DNS rcodes
#define DNS_RCODE_NOERROR  0
#define DNS_RCODE_FORMERR  1
#define DNS_RCODE_SERVFAIL 2
#define DNS_RCODE_NXDOMAIN 3
#define DNS_RCODE_NOTIMP   4
#define DNS_RCODE_REFUSED  5

// DNS Record Types (QTYPE / TYPE)
#define DNS_TYPE_A      1    // IPv4 Address
#define DNS_TYPE_NS     2    // Authoritative Name Server
#define DNS_TYPE_CNAME  5    // Canonical Name (Alias)
#define DNS_TYPE_SOA    6    // Start of Authority
#define DNS_TYPE_PTR    12   // Domain Name Pointer (Reverse DNS)
#define DNS_TYPE_HINFO  13   // Host Information
#define DNS_TYPE_MX     15   // Mail Exchange
#define DNS_TYPE_TXT    16   // Text Strings
#define DNS_TYPE_AAAA   28   // IPv6 Address
#define DNS_TYPE_SRV    33   // Service Locator
#define DNS_TYPE_OPT    41   // EDNS0 Options (RFC 6891) 
#define DNS_TYPE_ANY    255  // Wildcard match (Query only)

// DNS Classes (QCLASS / CLASS)
#define DNS_CLASS_IN    1    // Internet
#define DNS_CLASS_CS    2    // CSNET (Obsolete)
#define DNS_CLASS_CH    3    // CHAOS
#define DNS_CLASS_HS    4    // Hesiod
#define DNS_CLASS_ANY   255  // Wildcard match (Query only)

struct dns_hdr {
    uint16_t id;        // Transaction ID
    uint16_t flags;     // Flags (QR, Opcode, AA, TC, RD, RA, Z, RCODE)
    uint16_t qd_count;  // Number of Question Data
    uint16_t an_count;  // Number of Answer RR
    uint16_t ns_count;  // Number of Authority RR
    uint16_t ar_count;  // Number of Additional RR
};

int dns_hdr_decode(struct dns_hdr *hdr, const uint8_t *buf, size_t len);


/*  
  A DNS message api
 */
const char *rcode_tostr(int rcode);
const char *opcode_tostr(int opcode);
const char *dns_class_tostr(int ec);
const char *dns_type_tostr(int ec);

// DNS question data (QD)
struct dns_qd {
    const char *qname;
    uint16_t qtype;
    uint16_t qclass;
};

// convert dns msg section to readable string
int dns_qd_tostr(struct dns_qd *qd, char *buf, size_t buf_len);

// DNS resource record (RR)
// ------------------------
struct dns_rr {
    const char *name;
    uint16_t type;
    uint16_t class;
    uint32_t ttl; 
    uint16_t rdlen;
    // RDATA - union type to set|get values
    union {
        uint8_t a[4];   // 1
        char *ns_name;  // 2
        char *cname;    // 5
        struct {
            char *mname;
            char *rname;
            uint32_t serial;
            uint32_t refresh;
            uint32_t retry;
            uint32_t expire;
            uint32_t min_ttl;
        } soa; // 6
        char *ptr_name; 
        struct {
            char *cpu_str;
            char *os_str;
        } hinfo; // 13
        struct {
            uint16_t pref;    
            char *name;
        } mx; // 15
        struct {
            uint16_t num_str;
            char *str[DNS_MAX_TXT];
        } txt; // 16
        uint8_t aaaa[16];  //  28
        struct {
            uint16_t prior;
            uint16_t weight;
            uint16_t port;
            char *name;
        } srv; // 33
        struct {
            uint16_t udp_size;
            uint8_t  ext_rcode;
            uint8_t  edns_ver;
            uint16_t do_bit : 1;
            uint16_t z_bits : 15;
        } opt; // 41
        const uint8_t *raw;
    } rdata;
};

int dns_rr_tostr(struct dns_rr *rr, int sc, char *buf, size_t buf_len);

/*
 * DNS msg
 * -------
 * msg is a stucture defined as follows:
 *
 *  DNS msg - struct dns_msg 
 *   - hdr     - dns header
 *   - qd_recs - Question section 
 *   - an_recs - Answer section 
 *   - ns_recs - Authority section
 *   - ar_recs - Additional section
 *
 * Header 
 * ------
 *
 * Question Section:
 * -----------------
 *    qd_count  - number of question entires
 *    qd_recs   - array of struct dns_qd
 *    struct dns_qd 
 *    - qname 
 *    - qtype 
 *    - qclas
 *
 * Answser|Authority|Additional Sections:
 * --------------------------------------
 *  struct dns_rr
 *  - num_rec  - number of dns_rr
 *  - rec[32]  - fixed array of struct dns_rr
 *
 * dns_rr - used to set|get resource record
 * ----------------------------------------
 * struct dns_rr
 *  - name
 *  - type
 *  - class
 *  - ttl, 
 *  - rdlen
 *  - a union type to store RDATA
 */
struct dns_msg {
    struct dns_hdr hdr; 
    // names store
    char names[DNS_MAX_PDUSIZE];
    int names_len;
    uint16_t qd_len;
    uint16_t an_len;
    uint16_t ns_len;
    uint16_t ar_len;
    struct dns_qd qd[DNS_MAX_QD];
    struct dns_rr an[DNS_MAX_RR];
    struct dns_rr ns[DNS_MAX_RR];
    struct dns_rr ar[DNS_MAX_RR];
};

static inline struct dns_msg *dns_msg_reset(struct dns_msg *msg)
{
    // reset hdr
    msg->hdr.id = 0;
    msg->hdr.flags = 0;
    msg->hdr.qd_count = 0;
    msg->hdr.an_count = 0;
    msg->hdr.ns_count = 0;
    msg->hdr.ar_count = 0;

    // reset len
    msg->names_len = 0;
    msg->qd_len = 0;
    msg->an_len = 0;
    msg->ns_len = 0;
    msg->ar_len = 0;

    return msg;
}

// decode/encode a DNS message
int dns_msg_decode(struct dns_msg *msg, uint8_t *buf, size_t len);
ssize_t dns_msg_encode(struct dns_msg *msg, uint8_t *buf, size_t len);
int dns_validate(const void *buf, size_t len, char *emsg, size_t emsg_len);

// helper functions
int dns_msg_sects_tostr(struct dns_msg *msg,  char *buf, size_t len);

static inline void dns_hdr_init(struct dns_hdr *hdr, uint16_t id, uint16_t flags)
{
    hdr->id = id;
    hdr->flags = flags;
}

static inline void dns_msg_init(struct dns_msg *msg, uint16_t id, uint16_t flags)
{
    dns_msg_reset(msg);
    msg->hdr.id = id;
    msg->hdr.flags = flags;
}

int dns_add_qdn(struct dns_msg *msg, const char *qname, size_t len, uint16_t qtype, uint16_t qclass);

static inline int dns_add_qd(struct dns_msg *msg, const char *qname, uint16_t qtype, uint16_t qclass)
{
    return dns_add_qdn(msg, qname, qname ? strlen(qname) : 0, qtype, qclass);
}

// DNS message sections
#define DNS_MSG_AN 1
#define DNS_MSG_NS 2
#define DNS_MSG_AR 3

int dns_add_rr(struct dns_msg *msg, int sc, struct dns_rr *rr);


static inline int dns_msg_cnt_rec(struct dns_msg *msg)
{
    int nrec = 0;

    nrec += msg->hdr.an_count;
    nrec += msg->hdr.ns_count;
    nrec += msg->hdr.ar_count;

    return nrec;
}

struct dns_rr *dns_msg_get_rec(struct dns_msg *msg);
int dns_get_type(const char *str);
int dns_get_class(const char *str);
int dns_get_flag(const char *str);
int dns_rr_load(struct dns_rr *rr, int sc, const char *str);

#endif
