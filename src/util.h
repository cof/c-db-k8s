/*
 * UTIL api
 * --------
 * A misc utility API for apps featuring:
 * - signal handling
 * - string buffer
 * - string parsing
 * - codecs
 * - cmd-line parsing
 *
 * API sections
 * ------------
 * sys errors : general error codes
 * gen macros : array len, string literal, aligment, rmconst
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

// ptr macros
#define RMCONST(_t, _v) ((_t)(uintptr_t)(_v))
#define containerof(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define make_ptr(ptr, offset)  ((void *)  ( ((char *) ptr) + offset))
#define make_offset(base, ptr) ((uint64_t) ((char *) (ptr) - (char *) (base)))
#define make_mem(val) ((void *) ((uintptr_t) val))
#define unmake_mem(val) ((uintptr_t) (val))

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
 * Simple single hander API for apps featuring
 * - Structure-composable: built for inline embedding, object compostion & memory locality
 * - uses sigaction
 * - catchs  SIGINT|SIGTERM 
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
 * dbj2a_hash(key, len)     : return dbj2a hash of key buffer
 * dbj2a_hash_str(str)      : return dbj2a hash of string
 * gen_str(buf,len,fmt,..)  : generate a string to buffer
 * get_basename(path)       : return basename of path if found
 * itoa(buf, len, val)      : store an ascii repr of int to string buffer
 * int_tostr(buf, len, val) : convert int val to string repr
 * safe_strlen(str)         : return strlen if not null else 0
 * str_def(str, def_str)    : return str if set else default
 * str_tolower(str, len)    : lower case a string
 * str_toupper(str, len)    : upper case a string
 * iswhite(ch)              : char is whitespace (SP|TAB|VTAB|CR|LF)
 * is_numeric(ch)           : char is a number (0-9)
 * str_isnumeric(str, len)  : str is numeric
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

char *itoa(char *buf, int len, int val);
char *int_tostr(int val);

static inline size_t safe_strlen(const char *str)
{
    return str ? strlen(str) : 0;
}

static inline const char *str_def(const char *str, const char *def_str)
{
    return str && *str ? str : def_str;
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

static inline int iswhite(int ch) 
{
    return ch == ' ' || ch == '\t' || ch == '\v' || ch == '\r' || ch == '\n' ? 1 : 0;
}

static inline int is_numeral(int ch) 
{
    return ch >= '0' && ch <= '9' ? 1 : 0;
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

/*
 * Codec - Simple encoders/decoders
 * --------------------------------
 * enc_u32(wptr, value) : encode 32-bit at wptr return wptr+4
 * enc_u16(wptr, value) : encode 16-bit at wptr return wptr+2
 * enc_raw(wptr, buf, len) : encode buffer at wptr return wptr + len
 * dec_u32(buf) : decode a 32-bit value at buf
 * dec_u16(buf) : decode a 16-bit value at buf
 */
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

static inline uint8_t *enc_buf(uint8_t *wptr, uint8_t *buf, uint16_t len)
{
    memcpy(wptr, buf, len);
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
 * a simple string write buffer API
 */

// strbuf state
struct strbuf {
    char *data;
    char *wptr;
    char *end;
};

/* strbuf api
 * ----------
 * STRBUF_INIT(buf, size) : load buffer with memory addres and size
 * strbuf_avail(buf) : return byte size of writable space
 * strbuf_used(buf) :  retrun byte size of readable data
 * strbuf_putmem(buf, mem, len) : append mem  to buffer
 * strbuf_putstr(buf, str) : append str to to buffer
 * strbuf_putsep(buf, sep, mem, len) : append mem to buffer, add sep if not empty 
 */
#define STRBUF_INIT(_buf, _size) { _buf, _buf, _buf + _size } 

static inline size_t strbuf_avail(struct strbuf *buf)
{
    return buf->end - buf->wptr;
}

static inline size_t strbuf_used(struct strbuf *buf)
{
    return buf->wptr - buf->data;
}

static inline struct strbuf *strbuf_putmem(struct strbuf *buf, const char *mem, size_t len)
{
    if (len > strbuf_avail(buf)) return NULL;

    memcpy(buf->wptr, mem, len);
    buf->wptr += len;

    return buf;
}

static inline struct strbuf *strbuf_putstr(struct strbuf *buf, const char *str)
{
    return str ? strbuf_putmem(buf, str, strlen(str)) : NULL;
}

static inline struct strbuf *strbuf_putsep(struct strbuf *buf, int sep, const char *mem, size_t len)
{
    if (strbuf_used(buf)) {
        if (!strbuf_avail(buf)) return NULL;
        *buf->wptr++ = sep;
    }
    return strbuf_putmem(buf, mem, len);
}

/*
 * String slice API 
 * ----------------
 * A simple structure that stores a ptr + len
 * - Ensures buffer + len alway available
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
 * slice_cmp_cstr(str, cstr, len)    : return 1 if strs match else 0
 * slice_unbracket(str, left, right) : strip left and right chars from str
 * slice_rsplit(src, ch)  : split string from right at ch if found
 * slice_split(src, ch)   : split string from left if ch found
 * slice_isnumeric(str)   : true if slice is numeric 
 * slice_ltrim(str)       : left trim leading whitespace
 * slice_rtrim(str)       : right trim trailing whitespace    
 * slice_trim(str)        : trim left and right whitespace
 * slice_toupper(str)     : upper case str
 * slice_tolower(str)     : lowwer case str
 * slice_strdup(str)      : create a memory copy of str
 * slice_dbj2a_hash(str)  : create a dbj2a hash of str
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

static inline int slice_cmp_cstr(struct str_slice str, const char *cstr, size_t len)
{
    return len == str.len && memcmp(str.ptr, cstr, len) == 0;
}

static inline struct str_slice slice_unbracket(struct str_slice str, int left, int right)
{
    if (str.len && str.ptr[0] == left) {
        str.ptr++; str.len--;
        if (str.ptr[str.len] == right) str.len--;
    }

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

static inline struct str_slice slice_split(struct str_slice *src, int ch)
{
    struct str_slice dst;
   
    dst.ptr = memchr(src->ptr, ch, src->len);

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

static inline int slice_isnumeric(struct str_slice str)
{
    return str_isnumeric(str.ptr, str.len);
}

static inline struct str_slice *slice_ltrim(struct str_slice *str)
{
    while (str->len && iswhite(*str->ptr)) {
        str->ptr++;
        str->len--;
    }

    return str;
}

static inline struct str_slice *slice_rtrim(struct str_slice *str)
{
    while (str->len && iswhite(str->ptr[str->len - 1])) {
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

char *slice_strdup(const struct str_slice str);

static inline uint64_t slice_dbj2a_hash(const struct str_slice str)
{
    return dbj2a_hash(str.ptr, str.len);
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
    int has_arg;  // 0=none, 1=requried, 2=optional
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
void mode_usage(const char *prog_name, struct cmd_mode modes[], const char *examples[]);

#endif
