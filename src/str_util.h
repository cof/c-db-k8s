/* SPDX-License-Identifier: MIT | (c) 2026 [cof] */

/* 
 * STR_UTIL API
 * ------------
 * codec  : simple encoders/decoders
 * inet   : inet addr/string encoder/decoder
 * string : misc string api
 * sbuf   : string buffer api
 * slice  : string slice api
 */
#ifndef _STR_UTIL_H_
#define _STR_UTIL_H_

#include <stdlib.h> // malloc
#include <string.h> // memset, memove
#include "macros.h"

/*
 * Codec - Simple encoders/decoders
 * --------------------------------
 * uint8_toa(buf, val)        : a fast 8-bit value to ascii encoder
 * uint16_toa(buf, val)       : a fast 16-bit value to ascii encoder
 * uint16_toax(buf, val)      : a fast 16-bit value to hex encoder
 * uint8_tostr(val, str, len) : print 8-bit value to string buffer
 * hex_to_nibble : convert hex-char to 4-bit nibble
 * enc_u32(wptr, value) : encode 32-bit at wptr return wptr+4
 * enc_u16(wptr, value) : encode 16-bit at wptr return wptr+2
 * enc_mem(wptr, mem, len) : encode mem at wptr return wptr + len
 * dec_u32(buf) : decode a 32-bit value at buf
 * dec_u16(buf) : decode a 16-bit value at buf
 */

// A fast 8-bit value to ascii encoder
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
 * String API
 * ----------
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
 */

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


/*
 * a simple string buffer API
 */

// sbuf state
struct sbuf {
    uint8_t *mem;
    uint8_t *ptr;
    uint8_t *end;
};

/* sbuf api
 * ----------
 * SBUF_INIT(mem, len)      : macro for compile-time init
 * sbuf_init(buf, mem, len) : load buffer with mem and size
 * sbuf_reset(buf)          : rewind buffer ptr to start
 * -
 * sbuf_start(buf)    : return buffer start pointer
 * sbuf_ptr(buf)      : return buffer position pointer
 * sbuf_end(buf)      : return 1 if ptr at end else 0
 * sbuf_len(buf)      : return buffer size
 * sbuf_rem(buf)      : return space remaining
 * sbuf_pos(buf)      : return space used
 * sbuf_mksp(buf,len) : return ptr if space else null
 * sbuf_endz(buf)     : set ptr pos to nul char
 * -
 * sbuf_putm(buf,  mem, len)      : append mem
 * sbuf_putmc(buf, mem, len, ch)  : append mem + ch
 * sbuf_putmz(buf, mem, len)      : append mem + 0
 * sbuf_putcm(buf, ch, mem,len)   : append ch + mem
 * sbuf_puticm(buf, ch, mem, len) : append ch + mem if used else mem
 * sbuf_puts(buf, str)            : append str
 * sbuf_putn(buf, num)            : append number
 * sbuf_putcn(buf, num)           : append ch + number
 * run_cmd(buf, flags, fmt, ...)    : run a system cmd
 */

#define SBUF_INIT(_mem, _len) { \
    (uint8_t *) _mem, \
    (uint8_t *) _mem, \
    (uint8_t *) _mem + _len \
}

static inline struct sbuf *sbuf_init(struct sbuf *buf, void *mem, size_t len)
{
    buf->mem = mem;
    buf->ptr = buf->mem;
    buf->end = buf->mem + len;

    return buf;
}

static inline void sbuf_reset(struct sbuf *buf)
{
    buf->ptr = buf->mem;
}

static inline char *sbuf_start(struct sbuf *buf)
{
    return (char *) buf->mem;
}

static inline char *sbuf_ptr(struct sbuf *buf)
{
    return (char *) buf->ptr;
}

static inline size_t sbuf_len(struct sbuf *buf)
{
    return buf->end - buf->mem;
}

static inline size_t sbuf_rem(struct sbuf *buf)
{
    return buf->end - buf->ptr;
}

static inline size_t sbuf_pos(struct sbuf *buf)
{
    return buf->ptr - buf->mem;
}

static inline int sbuf_end(struct sbuf *buf)
{
    return buf->ptr >= buf->end;
}

static inline uint8_t *sbuf_mksp(struct sbuf *buf, size_t len)
{
    if (len > sbuf_rem(buf)) return NULL;
    uint8_t *ptr = buf->ptr;
    buf->ptr += len;
    return ptr;
}

static inline void sbuf_endz(struct sbuf *buf)
{
    if (buf->ptr < buf->end) *buf->ptr = '\0';
}

static inline size_t sbuf_putm(struct sbuf *buf, const char *mem, size_t len)
{
    uint8_t *wptr = sbuf_mksp(buf, len);
    if (!wptr) return 0;
    memcpy(wptr, mem, len);
    return len;
}

static inline size_t sbuf_putmc(struct sbuf *buf, const char *mem, size_t len, int c)
{
    uint8_t *wptr = sbuf_mksp(buf, len + 1);
    if (!wptr) return 0;
    memcpy(wptr, mem, len);
    wptr[len] = c;
    return len + 1;
}

static inline size_t sbuf_putmz(struct sbuf *buf, const char *mem, size_t len)
{
    return sbuf_putmc(buf, mem, len, '\0');
}

static inline size_t sbuf_putcm(struct sbuf *buf, int c, const char *mem, size_t len)
{
    uint8_t *wptr = sbuf_mksp(buf, len + 1);
    if (!wptr) return 0;
    *wptr++ = c;
    memcpy(wptr, mem, len);
    return len + 1;
}

static inline size_t sbuf_puticm(struct sbuf *buf, int ch, const char *mem, size_t len)
{
    return sbuf_pos(buf)
        ? sbuf_putcm(buf, ch, mem, len)
        : sbuf_putm(buf, mem, len);
}

static inline size_t sbuf_puts(struct sbuf *buf, const char *str)
{
    return str ? sbuf_putm(buf, str, strlen(str)) : 0;
}

static inline size_t sbuf_putcs(struct sbuf *buf, int ch, const char *str)
{
    return str ? sbuf_putcm(buf, ch, str, strlen(str)) : 0;
}

static inline size_t sbuf_putn(struct sbuf *buf, uint32_t num)
{
    return sbuf_puts(buf, u32_tostr(num));
}

static inline size_t sbuf_putcn(struct sbuf *buf, int ch, uint32_t num)
{
    uint8_t *wptr = sbuf_mksp(buf, 1);
    if (!wptr) return 0;
    *wptr = ch;
    return 1 + sbuf_putn(buf, num);
}

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
struct slice {
    char *ptr;
    size_t len;
};

/* str slice API
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
 * slice_ip4_decode(str, dst) : decode IPv4 str
 * slice_ip6_decode(str, dst) : decode IPv6 str
 */
#define SLICE(x) (int) (x).len, (x).ptr

static inline struct slice slice_make(char *buf, size_t len)
{
    struct slice dst;

    dst.ptr = buf;
    dst.len = len;

    return dst;
}

static inline struct slice slice_make_cstr(const char *str)
{
    return slice_make(RMCONST(char *, str), str ? strlen(str) : 0);
}

static inline struct slice slice_copy(struct slice val)
{
    return val;
}

static inline int slice_tomem(struct slice val, void *mem, size_t len)
{
    if (val.len + 1 > len) return 0;
    memcpy(mem, val.ptr, val.len);
    ((char *) mem)[val.len] = '\0';
    return len;
}

static inline int slice_cmp(struct slice str1, struct slice str2)
{
    return str_cmp(str1.ptr, str1.len, str2.ptr, str2.len);
}

static inline int slice_cmpmem(struct slice str, const char *mem, size_t len)
{
    return str_cmp(str.ptr, str.len, mem, len);
}

static inline int slice_cmpstr(struct slice slice, const char *str)
{
    return str_cmp(slice.ptr, slice.len, str, safe_strlen(str));
}

static inline int slice_casecmp(struct slice str1, struct slice str2)
{
    return str_casecmp(str1.ptr, str1.len, str2.ptr, str2.len);
}

static inline int slice_casecmpmem(struct slice slice, const char *mem, size_t len)
{
    return str_casecmp(slice.ptr, slice.len, mem, len);
}

static inline int slice_casecmpstr(struct slice slice, const char *str)
{
    return str_casecmp(slice.ptr, slice.len, str, safe_strlen(str));
}

static inline int slice_startswith(struct slice str, int ch)
{
    return str_startswith(str.ptr, str.len, ch);
}

static inline int slice_endswith(struct slice str, int ch)
{
    return str_endswith(str.ptr, str.len, ch);
}

static inline int slice_isnumeric(struct slice str)
{
    return str_isnumeric(str.ptr, str.len);
}

static inline struct slice slice_unbracket(struct slice str, int left, int right)
{
    if (str.len && str.ptr[0] == left) {
        str.ptr++; str.len--;
        if (str.ptr[str.len] == right) str.len--;
    }

    return str;
}

static inline struct slice *slice_chop(struct slice *str, int ch)
{
    char *ptr = memchr(str->ptr, ch, str->len);
    if (ptr) str->len = ptr - str->ptr;

    return str;
}

static inline struct slice slice_rsplit(struct slice *src, int ch)
{
    struct slice dst;

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

static inline struct slice slice_splitch(struct slice *src, int ch)
{
    struct slice dst = { src->ptr, 0 };
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

static inline struct slice slice_splitset(struct slice *src, const char *set, size_t len)
{
    struct slice dst = { src->ptr, 0 };
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

static inline size_t slice_countch(struct slice str, int ch)
{
    return str_countch(str.ptr, str.len, ch);
}

static inline uint32_t slice_tou32(struct slice str)
{
    return str_tou32(str.ptr, str.len);
}

static inline struct slice *slice_ltrim(struct slice *str)
{
    while (str->len && is_white(*str->ptr)) {
        str->ptr++;
        str->len--;
    }

    return str;
}

static inline struct slice *slice_rtrim(struct slice *str)
{
    while (str->len && is_white(str->ptr[str->len - 1])) {
        str->len--;
    }

    return str;
}


static inline struct slice *slice_trim(struct slice *str)
{
    return slice_ltrim(slice_rtrim(str));
}

static inline struct slice slice_toupper(struct slice str)
{
    str_toupper(str.ptr, str.len);

    return str;
}

static inline struct slice slice_tolower(struct slice str)
{
    str_tolower(str.ptr, str.len);

    return str;
}

static inline char *slice_strdup(const struct slice str)
{
    char *copy = malloc(str.len + 1);

    if (copy) {
        memcpy(copy, str.ptr, str.len);
        copy[str.len] = 0;
    }

    return copy;
}

static inline size_t slice_ip4_decode(const struct slice str, uint8_t dst[static 4])
{
    return ip4_str_decode(str.ptr, str.len, dst);
}

static inline size_t slice_ip6_decode(const struct slice str, uint8_t dst[static 16])
{
    return ip6_str_decode(str.ptr, str.len, dst);
}


#endif
