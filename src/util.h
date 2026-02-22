#ifndef __UTIL_H__
#define __UTIL_H__

#define ARR_LEN(a) (sizeof(a) / sizeof(a[0]))
#define STR_LIT(s) (s), (sizeof(s) - 1)

struct str_slice {
    char *ptr;
    size_t len;
};

static inline struct str_slice make_slice(char *str, size_t len)
{
    struct str_slice dst;

    dst.ptr = str;
    dst.len = len;

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

static inline struct str_slice ltrim(struct str_slice str)
{
    while (str.len && iswhite(*str.ptr)) {
        str.ptr++;
        str.len--;
    }

    return str;
}

#endif
