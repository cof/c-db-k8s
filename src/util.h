#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <getopt.h>

#define UTIL_FAIL -1

// general purpose macros
#define ARR_LEN(a) (sizeof(a) / sizeof(a[0]))
#define ARRAY(a)  ARR_LEN(a), a
#define STR_LIT(s) (s), (sizeof(s) - 1)
#define ALIGN_UP(n, a) (((n) + (a) - 1) & ~((a) - 1))

// ptr macros
#define containerof(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define make_ptr(ptr, offset)  ((void *)  ( ((char *) ptr) + offset))
#define make_offset(base, ptr) ((uint64_t) ((char *) (ptr) - (char *) (base)))
#define make_mem(val) ((void *) ((uintptr_t) val))
#define unmake_mem(val) ((uintptr_t) (val))

// Stringification macros
#define XSTR(a) #a
#define STR(a) XSTR(a)

// string handling code
struct str_slice {
    char *ptr;
    size_t len;
};

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

static inline void str2lower(char *str, size_t len)
{
    while (len) {
        int ch = *str;
        if (ch >= 'A' && ch <= 'Z') ch += 0x20;
        *str++ = ch;
        len--;
    }
}

static inline void str2upper(char *str, size_t len)
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

static inline struct str_slice *slice_toupper(struct str_slice *str)
{
    str2upper(str->ptr, str->len);

    return str;
}

static inline struct str_slice *slice_tolower(struct str_slice *str)
{
    str2lower(str->ptr, str->len);

    return str;
}

char *slice_strdup(const struct str_slice str);
char *itoa(char *buf, int len, int val);
char *int_tostr(int val);

char *gen_path(const char *dir, const char *name);

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

// wrapper around getopt_long
#define GETOPT_NOARG  0
#define GETOPT_REQARG 1
#define GETOPT_OPTARG 2
#define GETOPT_MAX 20
#define GETDEF(x) (x), 1

struct get_opt {
    const char *name;
    const char *desc;
    int has_arg;
    int val;
    // TODO use a union
    int def_val;
    unsigned int have_defval : 1;
};

struct getopt_parse {
    char **argv;
    size_t argc;
    size_t num_opt;
    struct get_opt *opts;
    struct str_slice val;
    int opt_idx;
    struct option long_opts[GETOPT_MAX+1];
};

int getopt_init(struct getopt_parse *parse, 
    int argc, char *argv[],
    size_t num_opt, struct get_opt opts[num_opt]);
int getopt_next(struct getopt_parse *parse);

static inline struct str_slice getopt_val(struct getopt_parse *parse)
{
    return parse->val;
}

void print_usage(const char *cmd, 
    int num_opt, const struct get_opt opt[num_opt],
    int num_exa, char *examples[num_exa]);

#endif
