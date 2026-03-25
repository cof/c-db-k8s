/*
 * A util api for string and cmdline processing
 *
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
#define RMCONST(_t, _v) ((_t)(uintptr_t)(_v))

// ptr macros
#define containerof(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define make_ptr(ptr, offset)  ((void *)  ( ((char *) ptr) + offset))
#define make_offset(base, ptr) ((uint64_t) ((char *) (ptr) - (char *) (base)))
#define make_mem(val) ((void *) ((uintptr_t) val))
#define unmake_mem(val) ((uintptr_t) (val))

// Stringification macros
#define XSTR(a) #a
#define STR(a) XSTR(a)

static inline size_t safe_strlen(const char *str)
{
    return str ? strlen(str) : 0;
}

// return str if set else use default
static inline const char *str_def(const char *str, const char *def_str)
{
    return str && *str ? str : def_str;
}

static inline size_t max(size_t x, size_t y)
{
    return x > y ? x : y;
}

static inline size_t min(size_t x, size_t y)
{
    return x < y ? x : y;
}

static inline const char *ec_tostr(int len, const char *estr[len], int ec, const char *def)
{
    const char *str;

    str = ec >= 0 && ec < len
        ? estr[ec] 
        : NULL;

    return str ?: def;
}

// string handling code
struct str_slice {
    char *ptr;
    size_t len;
};

#define SLICE(x) (int) (x).len, (x).ptr

static inline struct str_slice slice_make(char *str, size_t len)
{
    struct str_slice dst;

    dst.ptr = str;
    dst.len = len;

    return dst;
}

static inline struct str_slice slice_make_cstr(char *str)
{
    return slice_make(str, str ? strlen(str) : 0);
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
    return ch == ' ' || ch == '\t' || ch == '\v' || ch == '\r' || ch == '\t' ? 1 : 0;
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


//  misc
char *slice_strdup(const struct str_slice str);
char *itoa(char *buf, int len, int val);
char *int_tostr(int val);

int gen_str(char *buf, size_t len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

//  djb2a hash algorhtim
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

static inline uint64_t dbj2a_hash_slice(const struct str_slice str)
{
    return dbj2a_hash(str.ptr, str.len);
}

// signal handler API
struct simple_sig {
    volatile sig_atomic_t run;
    int signo;
    uid_t uid;
    pid_t pid;
};

int setup_signals(struct simple_sig *sig);

static inline const char *get_basename(const char *name)
{
    if (!name) return NULL;
    const char *base = strrchr(name, '/');
    return base ? base + 1 : name;
}

// generic setters
int str_setval(char **str, const char *name, const char *val_str);
int int_setval(int *ival, const char *name, const char *val_str);

// cmd-line parsing
#define OPT_NOARG  0
#define OPT_REQARG 1
#define OPT_OPTARG 2

#define OPT_EOF     -2
#define OPT_UNSUPP  -3
#define OPT_MISSVAL -4

struct cmd_opt {
    const char *name;
    const char *desc;
    const char *def_str;
    int has_arg;  // 0=none, 1=requried, 2=optional
};

struct cmd_argv {
    int argc;
    char **argv;
    struct cmd_opt *opts;
    int argv_idx;
    int opt_idx;
    int val_idx;
    char *name;
    char *value;
};

int cmd_argv_next(struct cmd_argv *parse);
int opt_setstr(char **str, struct cmd_argv *parse);
int opt_setint(int *iptr, struct cmd_argv *parse);

void print_usage(const char *cmd, const struct cmd_opt opts[], const char *examples[]);

#endif
