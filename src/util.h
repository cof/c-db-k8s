/*
 * UTIL api
 * --------
 * A misc utility API for apps featuring:
 * - signal handling
 * - string buffer
 * - string parsing
 * - hash tables
 * - codecs
 * - cmd-line parsing
 *
 * API sections
 * ------------
 * sys errors : general error codes
 * gen macros : array len, string literal, alignment, rmconst
 * ptr macros : ptr manipulation
 * str macros : Stringification
 * min-max    : safe min/max funcs
 * signal     : simple signal handler api
 * string     : misc string api
 * codec      : simple encoders and decoders
 * strbuf     : for simple string write buffer
 * str_slice  : for a memory view (buf+len)
 * setter     : for setting string and int values
 * cmd-line   : cmd-line api
 */
#ifndef _UTIL_H_
#define _UTIL_H_

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

// system errors
#define UTIL_OK    0
#define UTIL_FAIL -1
#define UTIL_EOF  -2

// general purpose macros
#define ARR_LEN(a) (sizeof(a) / sizeof(a[0]))
#define ARRAY(a)  ARR_LEN(a), a
#define STR_LIT(s) (s), (sizeof(s) - 1)
#define ALIGN_UP(n, a) (((n) + (a) - 1) & ~((a) - 1))

// ptr macros - mkmem/umkmem aka TOPTR/FROMPTR
#define RMCONST(_t, _v) ((_t)(uintptr_t)(_v))
#define containerof(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define mkptr(ptr, offset)  ((void *)  ( ((char *) ptr) + offset))
#define mkoffset(base, ptr) ((uint64_t) ((char *) (ptr) - (char *) (base)))
#define mkmem(val) ((void *) ((uintptr_t) val))
#define umkmem(val) ((uint64_t) ((uintptr_t) (val)))

// Stringification macros
#define XSTR(a) #a
#define STR(a) XSTR(a)

/* min-max API
 * -----------
 * max(x,y) : return max of x and y
 * min(x,y) : return min of x and y
 */
static inline size_t max(size_t x, size_t y)
{
    return x > y ? x : y;
}

static inline size_t min(size_t x, size_t y)
{
    return x < y ? x : y;
}


/* signal handler API
 * -----------------
 * Simple single handler API for apps featuring
 * - Structure-composable: built for inline embedding, object compostion & memory locality
 * - uses sigaction
 * - catches  SIGINT|SIGTERM
 * - ignores SIGPIPE
 * - logs signal, sender uid and pid for app
 * - simple set run to 1 to 0 design
 */

// signal handler state
struct simple_sig {
    volatile sig_atomic_t run;
    int signo;
    uid_t uid;
    pid_t pid;
};

/*
 * simple_sig API
 * --------------
 * setup_signals(sig) - setup signal handler
 */
int setup_signals(struct simple_sig *sig);

/*
 * String API
 * ----------
 * ec_tostr(len, estrs, ec, def) : lookup a string for ec or return default
 * dbj2a_hash(key, len)       : return dbj2a hash of key buffer
 * dbj2a_hash_str(str)        : return dbj2a hash of string
 * gen_str(buf,len,fmt,..)    : generate a string to buffer
 * get_basename(path)         : return basename of path if found
 * safe_strlen(str)           : return strlen if not null else 0
 * str_def(str, def_str)      : return str if set else default
 * str_memcpy(dst, src, len)  : memcpy a short string
 * is_white(ch)               : char is whitespace (SP|TAB|VTAB|CR|LF)
 * is_numeric(ch)             : char is a number (0-9)
 * str_cat(dst, src)          : copy src to dst return position of nul
 * str_tolower(str, len)      : lower case a string
 * str_toupper(str, len)      : upper case a string
 * str_countch(str, len, ch)  : count number of ch in str
 * str_cmpmem(s1,len,s2,len2)  : cmp mem return < 0, 0, > 0 if lt, eq or gt
 * str_cmpmemi(s1,len,s2,len2) : cmp mem ignore case return < 0, 0, > 0 if lt, eq or gt
 * str_startswith(str,len,ch) : true if str begins with ch
 * str_endswith(str,len,ch)   : true if str ends with ch
 * str_isnumeric(str, len)    : true if str is numeric
 * str_tou32(str, len)        : convert str to uint32_t
 * itoa(val, buf, len)        : print ascii repr of int to string buffer
 * u32toa(val, buf, len)      : print ascii repr of uint32_t to string buffer
 * int_tostr(val)             : convert int-val to string
 * u32_tostr(val)             : convert u32 to string
 * uint8_toa(buf, val)        : a fast 8-bit value to ascii encoder
 * uint16_toa(buf, val)       : a fast 16-bit value to ascii encoder
 * uint16_toax(buf, val)      : a fast 16-bit value to hex encoder
 * uint8_tostr(val, str, len) : print 8-bit value to string buffer
 */

static inline const char *ec_tostr(int len, const char *estr[len], int ec, const char *def)
{
    const char *str;

    str = ec >= 0 && ec < len
        ? estr[ec]
        : NULL;

    return str ?: def;
}

static inline uint64_t dbj2a_hash(const void *key, const int klen)
{
    const unsigned char *data,*end;
    uint64_t hash = 5381;

    end = key + klen;
    for (data = key; data < end; data++) {
        hash = ((hash << 5) + hash) ^ *data;
    }

    return hash;
}

static inline uint64_t dbj2a_hash_str(const char *name)
{
    return dbj2a_hash(name, strlen(name));
}

int gen_str(char *buf, size_t len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static inline const char *get_basename(const char *path)
{
    if (!path) return NULL;
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

static inline size_t safe_strlen(const char *str)
{
    return str ? strlen(str) : 0;
}

static inline const char *str_def(const char *str, const char *def_str)
{
    return str && *str ? str : def_str;
}

static inline void *str_memcpy(void *dst, const void *src, int len)
{
    uint8_t *dptr = dst;
    const uint8_t *sptr = src;
    const uint8_t *send = sptr + len;

    while (sptr < send) {
        *dptr++ = *sptr++;
    }

    return dptr;
}

static inline int is_white(int ch)
{
    return ch == ' ' || ch == '\t' || ch == '\v' || ch == '\r' || ch == '\n' ? 1 : 0;
}

static inline int is_numeral(int ch)
{
    return ch >= '0' && ch <= '9' ? 1 : 0;
}

static inline char *str_cat(char *dst, const char *src)
{
    while (*src) {
        *dst++ = *src++;
    }

    return dst;
}

static inline void str_tolower(char *str, size_t len)
{
    while (len) {
        int ch = *str;
        if (ch >= 'A' && ch <= 'Z') ch += 0x20;
        *str++ = ch;
        len--;
    }
}

static inline void str_toupper(char *str, size_t len)
{
    while (len) {
        int ch = *str;
        if (ch >= 'a' && ch <= 'z') ch -= 0x20;
        *str++ = ch;
        len--;
    }
}

static inline size_t str_countch(const char *str, size_t len, int ch)
{
    size_t count = 0;
    const char *str_end = str + len;

    while (str < str_end) {
        if (*str++ == ch) count++;
    }

    return count;
}

static inline int str_cmp(const void *s1, size_t len1, const void *s2, size_t len2)
{
    if (len1 != len2) {
        size_t len = len1 < len2 ? len1 : len2;
        int rc = memcmp(s1, s2, len);
        if (rc) return rc;
        return len1 - len2;
    }

    return memcmp(s1, s2, len1);
}

static inline int str_casecmp(const char *s1, size_t len1, const char *s2, size_t len2)
{
    size_t len = len1 < len2 ? len1 : len2;

    while (len) {
        int c1 = *s1++;
        int c2 = *s2++;
        if (c1 >= 'A' && c1 <= 'Z') c1 |= 0x20;
        if (c2 >= 'A' && c2 <= 'Z') c2 |= 0x20;
        if (c1 != c2) return c1 - c2;
        len--;
    }

    return len1 == len2 ? 0 : len1 - len2;
}

static inline int str_startswith(const char *str, size_t len, int ch)
{
    return len && str[0] == ch ? 1 : 0;
}

static inline int str_endswith(const char *str, size_t len, int ch)
{
    return len && str[len - 1] == ch ? 1 : 0;
}

static inline int str_isnumeric(const char *str, size_t len)
{
    if (!len) return 0;

    const char *end = str + len;

    while (str < end) {
        if (!is_numeral(*str)) return 0;
        str++;
    }

    return 1;
}

static inline uint32_t str_tou32(const char *str, size_t len)
{
    const char *ptr = str;
    const char *end = str + len;
    uint32_t val = 0;

    while (ptr < end && is_numeral(*ptr)) {
        // val = val * 10 using shifts + add
        val = (val << 3) + (val << 1) + (*ptr++ - '0');
    }

    return val;
}

char *itoa(int val, char *buf, size_t len);
char *u32toa(uint32_t val, char *buf, size_t len);
char *int_tostr(int val);
char *u32_tostr(uint32_t val);

// a fast 8-bit value to ascii encoder
static inline char *uint8_toa(char *wptr, uint8_t val)
{
    if (val < 10) {
        // 1-digit : 0 - 9
        *wptr++ = val + '0';
    }
    else if (val < 100) {
        // 2-digit : 10 - 99
        // n / 10 is : (n * 205) >> 11
        uint8_t d1 = (val * 205) >> 11;
        uint8_t d2 = val - (d1 * 10);
        *wptr++ = d1 + '0';
        *wptr++ = d2 + '0';
    }
    else {
        // 3-digit : 100 - 255
        // n / 100 is : (n * 164) >> 14
        uint8_t d1 = (val * 164) >> 14;
        uint8_t rem = val - (d1 * 100);
        // rem / 10 is : (rem * 205) >> 11
        uint8_t d2 = (rem * 205) >> 11;
        uint8_t d3 = rem - (d2 * 10);
        *wptr++ = d1 + '0';
        *wptr++ = d2 + '0';
        *wptr++ = d3 + '0';
    }

    return wptr;
}

// a fast 16-bit value to ascii encoder
static inline char *uint16_toa(char *wptr, uint16_t val)
{
    // 16-bits - max 5 digits - 65535
    if (val >= 10000) *wptr++ = (val / 10000) + '0';
    if (val >= 1000)  *wptr++ = (val / 1000 % 10) + '0';
    if (val >= 100)   *wptr++ = (val / 100 % 10) + '0';
    if (val >= 10)    *wptr++ = (val / 10 % 10) + '0';
    *wptr++ = (val % 10) + '0';

    return wptr;
}

// a fast 16-bit value to ascii hex encoder
static inline char *uint16_toax(char *wptr, uint16_t val)
{
    static const char hex[] = "0123456789abcdef";

    // 16-bits = 4 x nibbles = 4 x hex-chars
    if (val >= 0x1000) *wptr++ = hex[(val >> 12) & 0xf];
    if (val >= 0x100)  *wptr++ = hex[(val >> 8)  & 0xf];
    if (val >= 0x10)   *wptr++ = hex[(val >> 4)  & 0xf];
    *wptr++ = hex[val & 0xf];

    return wptr;
}

static inline char *uint8_tostr(uint8_t val, char *str, size_t len)
{
    if (len < 3) return str;

    char *dst = uint8_toa(str, val);
    *dst = '\0';

    return str;
}

/*
 * INET api
 * --------
 * len = ip4_str_decode(str, len, dst)  : decode IPv4 addr-str
 * len = ip6_str_decode(str, len, dst)  : decode IPv6 addr-str
 * len = ip4_str_encode(addr, str, len) : encode ip4-addr to str
 * len = ip6_str_encode(addr, flags, str, len) : encode ip6-addr to str
 */
#define IP4_ADDR_STRLEN sizeof("255.255.255.255")
#define IP6_ADDR_STRLEN sizeof("ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255")
#define PORT_STRLEN sizeof("65535")
#define IP_ADDRPORT_STRLEN (IP6_ADDR_STRLEN + 3 + PORT_STRLEN - 1)

// ip6_str_encode flags
#define IP6_STR_ADDBRACK  (1 << 0) // add []
#define IP6_STR_NOIPV4    (1 << 1) // dont use IPv4 mapped prefix
#define IP6_STR_STRIPV4   (1 << 2) // strip IPv4 mapped prefix

size_t ip4_str_decode(const char *str, size_t len, uint8_t dst[static 4]);
size_t ip6_str_decode(const char *str, size_t len, uint8_t dst[static 16]);
size_t ip4_str_encode(const uint8_t addr[static 4], char *str, size_t len);
size_t ip6_str_encode(const uint8_t addr[static 16], int flags, char *str, size_t len);

/*
 * Codec - Simple encoders/decoders
 * --------------------------------
 * hex_to_nibble : convert hex-char to 4-bit nibble
 * enc_u32(wptr, value) : encode 32-bit at wptr return wptr+4
 * enc_u16(wptr, value) : encode 16-bit at wptr return wptr+2
 * enc_mem(wptr, mem, len) : encode mem at wptr return wptr + len
 * dec_u32(buf) : decode a 32-bit value at buf
 * dec_u16(buf) : decode a 16-bit value at buf
 */

static inline uint8_t __attribute__((always_inline)) hex_to_nibble(char ch)
{
    // no multiply, cache hit or branches
    return (ch & 0xf) + (ch >> 6) + ((ch >> 6) << 3);
}

static inline uint8_t *enc_u32(uint8_t *wptr, uint32_t value)
{
    *wptr++ = value >> 24;
    *wptr++ = value >> 16;
    *wptr++ = value >> 8;
    *wptr++ = value;

    return wptr;
}

static inline uint8_t *enc_u16(uint8_t *wptr, uint16_t value)
{
    *wptr++ = value >> 8;
    *wptr++ = value;

    return wptr;
}

static inline uint8_t *enc_mem(uint8_t *wptr, void *mem, size_t len)
{
    memcpy(wptr, mem, len);
    wptr += len;

    return wptr;
}

static inline uint32_t dec_u32(const unsigned char *buf)
{
    uint32_t value;

    value = buf[0] << 24;
    value |= buf[1] << 16;
    value |= buf[2] << 8;
    value |= buf[3];

    return value;
}

static inline uint16_t dec_u16(const unsigned char *buf)
{
    uint16_t value;

    value = buf[0] << 8;
    value |= buf[1];

    return value;
}

/*
 * a simple string buffer API
 */

// strbuf state
struct strbuf {
    uint8_t *mem;
    uint8_t *ptr;
    uint8_t *end;
};

/* STRBUF api
 * ----------
 * STRBUF_INIT(mem, len)      : macro for compile-time init
 * strbuf_init(buf, mem, len) : load buffer with mem and size
 * strbuf_reset(buf)          : rewind buffer ptr to start
 * -
 * strbuf_start(buf)    : return buffer start pointer
 * strbuf_ptr(buf)      : return buffer position pointer
 * strbuf_end(buf)      : return 1 if ptr at end else 0
 * strbuf_len(buf)      : return buffer size
 * strbuf_rem(buf)      : return space remaining
 * strbuf_pos(buf)      : return space used
 * strbuf_mksp(buf,len) : return ptr if space else null
 * strbuf_endz(buf)     : set ptr pos to nul char
 * -
 * strbuf_putm(buf,  mem, len)      : append mem
 * strbuf_putmc(buf, mem, len, ch)  : append mem + ch
 * strbuf_putmz(buf, mem, len)      : append mem + 0
 * strbuf_putcm(buf, ch, mem,len)   : append ch + mem
 * strbuf_puticm(buf, ch, mem, len) : append ch + mem if used else mem
 * strbuf_puts(buf, str)            : append str
 * strbuf_putn(buf, num)            : append number
 * strbuf_putcn(buf, num)           : append ch + number
 * run_cmd(buf, flags, fmt, ...)    : run a system cmd
 */
#define STRBUF_INIT(_mem, _len) { \
    (uint8_t *) _mem, \
    (uint8_t *) _mem, \
    (uint8_t *) _mem + _len \
}

static inline struct strbuf *strbuf_init(struct strbuf *buf, void *mem, size_t len)
{
    buf->mem = mem;
    buf->ptr = buf->mem;
    buf->end = buf->mem + len;

    return buf;
}

static inline void strbuf_reset(struct strbuf *buf)
{
    buf->ptr = buf->mem;
}

static inline char *strbuf_start(struct strbuf *buf)
{
    return (char *) buf->mem;
}

static inline char *strbuf_ptr(struct strbuf *buf)
{
    return (char *) buf->ptr;
}

static inline size_t strbuf_len(struct strbuf *buf)
{
    return buf->end - buf->mem;
}

static inline size_t strbuf_rem(struct strbuf *buf)
{
    return buf->end - buf->ptr;
}

static inline size_t strbuf_pos(struct strbuf *buf)
{
    return buf->ptr - buf->mem;
}

static inline int strbuf_end(struct strbuf *buf)
{
    return buf->ptr >= buf->end;
}

static inline uint8_t *strbuf_mksp(struct strbuf *buf, size_t len)
{
    if (len > strbuf_rem(buf)) return NULL;
    uint8_t *ptr = buf->ptr;
    buf->ptr += len;
    return ptr;
}

static inline void strbuf_endz(struct strbuf *buf)
{
    if (buf->ptr < buf->end) *buf->ptr = '\0';
}

static inline size_t strbuf_putm(struct strbuf *buf, const char *mem, size_t len)
{
    uint8_t *wptr = strbuf_mksp(buf, len);
    if (!wptr) return 0;
    memcpy(wptr, mem, len);
    return len;
}

static inline size_t strbuf_putmc(struct strbuf *buf, const char *mem, size_t len, int c)
{
    uint8_t *wptr = strbuf_mksp(buf, len + 1);
    if (!wptr) return 0;
    memcpy(wptr, mem, len);
    wptr[len] = c;
    return len + 1;
}

static inline size_t strbuf_putmz(struct strbuf *buf, const char *mem, size_t len)
{
    return strbuf_putmc(buf, mem, len, '\0');
}

static inline size_t strbuf_putcm(struct strbuf *buf, int c, const char *mem, size_t len)
{
    uint8_t *wptr = strbuf_mksp(buf, len + 1);
    if (!wptr) return 0;
    *wptr++ = c;
    memcpy(wptr, mem, len);
    return len + 1;
}

static inline size_t strbuf_puticm(struct strbuf *buf, int ch, const char *mem, size_t len)
{
    return strbuf_pos(buf)
        ? strbuf_putcm(buf, ch, mem, len)
        : strbuf_putm(buf, mem, len);
}

static inline size_t strbuf_puts(struct strbuf *buf, const char *str)
{
    return str ? strbuf_putm(buf, str, strlen(str)) : 0;
}

static inline size_t strbuf_putcs(struct strbuf *buf, int ch, const char *str)
{
    return str ? strbuf_putcm(buf, ch, str, strlen(str)) : 0;
}

static inline size_t strbuf_putn(struct strbuf *buf, uint32_t num)
{
    return strbuf_puts(buf, u32_tostr(num));
}

static inline size_t strbuf_putcn(struct strbuf *buf, int ch, uint32_t num)
{
    uint8_t *wptr = strbuf_mksp(buf, 1);
    if (!wptr) return 0;
    *wptr = ch;
    return 1 + strbuf_putn(buf, num);
}

#define RUN_MAXARG 32
#define RUN_CAPS 0x1
#define RUN_NULL 0x2
int run_cmd(struct strbuf *buf, int flags, const char *fmt, ...) \
    __attribute__((format(printf, 3, 4)));

/*
 * String slice API
 * ----------------
 * A simple structure that stores a ptr + len
 * - Ensures buffer + len always available
 * - No more strlen() to check
 * - Can pass by value a ptr + len
 * - Can return by value a ptr + len
 */

// slice state
struct str_slice {
    char *ptr;
    size_t len;
};

/* str_slice API
 * -------------
 * SLICE(str)             : macro to extract the slice len and ptr
 * slice_make(str, len)   : return a slice set with str and len
 * slice_make_cstr(str)   : return a slice set with str
 * slice_copy(str)        : return a copy of str
 * slice_tomem(slice, men, len) : copy slice to mem
 * -
 * slice_cmp(s1, s2)             : cmp slices - return < 0, 0, > 0 if lt, eq or gt
 * slice_cmpmem(slice, mem, len) : cmp slice to mem - return < 0, 0, > 0 if lt, eq or gt
 * slice_cmpstr(slice, str)      : cmp slice to str - return < 0, 0, > 0 if lt, eq or gt
 * -
 * slice_casecmp(s1, s2)             : cmp slice - ignore case
 * slice_casecmpmem(slice, mem, len) : cmp slice to mem - return < 0, 0, > 0 if lt, eq or gt
 * slice_casecmpstr(slice, str)      : cmp slice to str - return < 0, 0, > 0 if lt, eq or gt
 * -
 * slice_startswith(str,ch)      : true if str begins with ch
 * slice_endswith(str,ch)        : true if str ends with ch
 * slice_isnumeric(str)          : true if slice is numeric
 * -
 * slice_unbracket(str, left, right) : strip left and right chars from str
 * slice_chop(str, ch)    : chop str-slice at ch if founc
 * slice_rsplit(src, ch)  : split string from right at ch if found
 * slice_splitch(src, ch)        : split string from left if ch found
 * slice_splitset(src, set, len) : split string from left if ch found in set
 * slice_countch(str,ch)  : count number of ch in slice
 * slice_tou32(str)         : convert str-slice to uint32_t
 * slice_ltrim(str)         : left trim leading whitespace
 * slice_rtrim(str)         : right trim trailing whitespace
 * slice_trim(str)          : trim left and right whitespace
 * slice_toupper(str)       : upper case str
 * slice_tolower(str)       : lowwer case str
 * slice_strdup(str)        : create a memory copy of str
 * slice_memcpy(buf,len,str)  : copy slice to buf
 * slice_dbj2a_hash(str)      : create a dbj2a hash of str
 * slice_ip4_decode(str, dst) : decode IPv4 str
 * slice_ip6_decode(str, dst) : decode IPv6 str
 */
#define SLICE(x) (int) (x).len, (x).ptr

static inline struct str_slice slice_make(char *buf, size_t len)
{
    struct str_slice dst;

    dst.ptr = buf;
    dst.len = len;

    return dst;
}

static inline struct str_slice slice_make_cstr(const char *str)
{
    return slice_make(RMCONST(char *, str), str ? strlen(str) : 0);
}

static inline struct str_slice slice_copy(struct str_slice val)
{
    return val;
}

static inline int slice_tomem(struct str_slice val, void *mem, size_t len)
{
    if (val.len + 1 > len) return 0;
    memcpy(mem, val.ptr, val.len);
    ((char *) mem)[val.len] = '\0';
    return len;
}

static inline int slice_cmp(struct str_slice str1, struct str_slice str2)
{
    return str_cmp(str1.ptr, str1.len, str2.ptr, str2.len);
}

static inline int slice_cmpmem(struct str_slice str, const char *mem, size_t len)
{
    return str_cmp(str.ptr, str.len, mem, len);
}

static inline int slice_cmpstr(struct str_slice slice, const char *str)
{
    return str_cmp(slice.ptr, slice.len, str, safe_strlen(str));
}

static inline int slice_casecmp(struct str_slice str1, struct str_slice str2)
{
    return str_casecmp(str1.ptr, str1.len, str2.ptr, str2.len);
}

static inline int slice_casecmpmem(struct str_slice slice, const char *mem, size_t len)
{
    return str_casecmp(slice.ptr, slice.len, mem, len);
}

static inline int slice_casecmpstr(struct str_slice slice, const char *str)
{
    return str_casecmp(slice.ptr, slice.len, str, safe_strlen(str));
}

static inline int slice_startswith(struct str_slice str, int ch)
{
    return str_startswith(str.ptr, str.len, ch);
}

static inline int slice_endswith(struct str_slice str, int ch)
{
    return str_endswith(str.ptr, str.len, ch);
}

static inline int slice_isnumeric(struct str_slice str)
{
    return str_isnumeric(str.ptr, str.len);
}

static inline struct str_slice slice_unbracket(struct str_slice str, int left, int right)
{
    if (str.len && str.ptr[0] == left) {
        str.ptr++; str.len--;
        if (str.ptr[str.len] == right) str.len--;
    }

    return str;
}

static inline struct str_slice *slice_chop(struct str_slice *str, int ch)
{
    char *ptr = memchr(str->ptr, ch, str->len);
    if (ptr) str->len = ptr - str->ptr;

    return str;
}

static inline struct str_slice slice_rsplit(struct str_slice *src, int ch)
{
    struct str_slice dst;

    dst.ptr = memrchr(src->ptr, ch, src->len);

    if (dst.ptr) {
        dst.len = src->len - (dst.ptr - src->ptr + 1);
        src->len -= dst.len + 1;
        dst.ptr++;
    }
    else {
        dst.len = 0;
    }

    return dst;
}

static inline struct str_slice slice_splitch(struct str_slice *src, int ch)
{
    struct str_slice dst = { src->ptr, 0 };
    char *ptr = memchr(src->ptr, ch, src->len);

    if (ptr) {
        // match
        dst.len = ptr - src->ptr;
        src->ptr += dst.len + 1;
        src->len -= dst.len + 1;
    }
    else {
        // no match
        dst.len = src->len;
        src->ptr += src->len;
        src->len = 0;
    }

    return dst;
}

static inline struct str_slice slice_splitset(struct str_slice *src, const char *set, size_t len)
{
    struct str_slice dst = { src->ptr, 0 };
    size_t i = 0;

    // find first delimiter
    while (i < src->len && !memchr(set, src->ptr[i], len)) {
        i++;
    }
    dst.len = i;

    // skip delimiters
    while (i < src->len && memchr(set, src->ptr[i], len)) {
        i++;
    }

    src->ptr += i;
    src->len -= i;

    return dst;
}

static inline size_t slice_countch(struct str_slice str, int ch)
{
    return str_countch(str.ptr, str.len, ch);
}

static inline uint32_t slice_tou32(struct str_slice str)
{
    return str_tou32(str.ptr, str.len);
}

static inline struct str_slice *slice_ltrim(struct str_slice *str)
{
    while (str->len && is_white(*str->ptr)) {
        str->ptr++;
        str->len--;
    }

    return str;
}

static inline struct str_slice *slice_rtrim(struct str_slice *str)
{
    while (str->len && is_white(str->ptr[str->len - 1])) {
        str->len--;
    }

    return str;
}

static inline struct str_slice *slice_trim(struct str_slice *str)
{
    return slice_ltrim(slice_rtrim(str));
}

static inline struct str_slice slice_toupper(struct str_slice str)
{
    str_toupper(str.ptr, str.len);

    return str;
}

static inline struct str_slice slice_tolower(struct str_slice str)
{
    str_tolower(str.ptr, str.len);

    return str;
}

static inline uint64_t slice_dbj2a_hash(const struct str_slice str)
{
    return dbj2a_hash(str.ptr, str.len);
}

static inline size_t slice_ip4_decode(const struct str_slice str, uint8_t dst[static 4])
{
    return ip4_str_decode(str.ptr, str.len, dst);
}

static inline size_t slice_ip6_decode(const struct str_slice str, uint8_t dst[static 16])
{
    return ip6_str_decode(str.ptr, str.len, dst);
}

/*
 * Setter API
 * ----------
 * str_setval : set str with strdup(val)
 * int_setval :  set int with atoi(val)
 * uint_setval : set uint with atoi(val)
 */
int str_setval(char **str, const char *name, const char *val_str);
int int_setval(int *ival, const char *name, const char *val_str);
int uint_setval(uint32_t *uval, const char *name, const char *val_str);


/* cmd-line */

/*
 *  cmd-argv parser API
 *  -------------------
 *  Uses a simple stateful iterator over cmd-line args.
 *  No malloc just pass it arg,argv and array of opts
 *
 *  Example Usage:
 *  =============
 *  struct cmd_opt opts[] = {
 *      // name,    desc,          def,     has_arg, code
 *      { "--opt1", "description", "default", 1,      0 }:
 *      { "--opt2", "description", "default", 1,      1 }:
 *  };
 *  struct cmd_argv parser = { argc, argv, opts };
 *  while ( (rc = cmd_argv_next(&parser)) >= 0) {
 *      switch(rc) {
 *      case 0: // opt 1
 *      case 1: // opt 2
 *      }
 *   }
 *   if (rc != OPT_EOF) { printf("Error\n"); exit(1));
 */

// has_arg codes
#define OPT_NOARG  0 // option has no arg
#define OPT_REQARG 1 // option requires arg
#define OPT_OPTARG 2 // ???

// error codes
#define OPT_EOF     -2 // no more options
#define OPT_UNSUPP  -3 // option not found
#define OPT_MISSVAL -4 // option missing value

// cmd-line option
struct cmd_opt {
    const char *name;
    const char *desc;
    const char *def_str;
    int has_arg;  // 0=none, 1=required, 2=optional
    int code;
};

// cmd-line parser state
struct cmd_argv {
    int argc;             // number of cmd-line arg
    char **argv;          // array of cmd-line arg
    struct cmd_opt *opts; // array of options
    int argv_idx;         // current argv index
    int opt_idx;          // index of matched option
    struct cmd_opt *opt;  // matched option
    const char *name;     // matched argv name
    const char *value;    // matched argv value
};

/*
 * cmd_argv API
 * ------------
 * cmd_argv_next(parser)   : return next option code
 * opt_setstr(str, parser) : set string with option value
 * opt_setint(str, parser) : set int with option value
 * opt_setuint(str, parser) : set int with option value
 */
int cmd_argv_next(struct cmd_argv *parse);
int opt_setstr(char **str, struct cmd_argv *parse);
int opt_setint(int *iptr, struct cmd_argv *parse);
int opt_setuint(uint32_t *uptr, struct cmd_argv *parse);

/*
 * cmd-API
 * -------
 * cmd_mode = cmd_mode_find(mode, modes) : find cmd-mode entry
 * prog_usage(prog_name, opts, examples) : print cmd usage
 * mode_usage(prog_name, modes, examples) : print modes usage
 */

#define MODE_RUN(f) ((int (*)(void *))(f))

// cmd runner
struct cmd_mode {
    int (*run)(void *state);
    int mode;
    struct cmd_opt *opts;
    char *name;
    char *desc;
};

struct cmd_mode *cmd_mode_find(char *mode, struct cmd_mode modes[]);
void prog_usage(const char *prog_name, const struct cmd_opt opts[], const char *examples[]);
void mode_usage(const char *prog_name, const struct cmd_mode modes[], const char *examples[]);

#endif
