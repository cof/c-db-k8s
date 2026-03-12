#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

// general purpose macros
#define ARR_LEN(a) (sizeof(a) / sizeof(a[0]))
#define STR_LIT(s) (s), (sizeof(s) - 1)
#define containerof(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define ALIGN_UP(n, a) (((n) + (a) - 1) & ~((a) - 1))

// Stringification macros
#define XSTR(a) #a
#define STR(a) XSTR(a)

// logger
void log_info(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

int _log_error(const char *file, int line, const char *func, int ec, const char *fmt, ...) 
    __attribute__((format(printf, 5, 6)));

void _fatal_error(const char *file, int line, const char *func, int ec, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

#define log_error(...)  _log_error(__FILE__, __LINE__, __func__, 0,  __VA_ARGS__)
#define log_errno(...)  _log_error(__FILE__, __LINE__, __func__, errno,  __VA_ARGS__)
#define log_errnon(...)  (_log_error(__FILE__, __LINE__, __func__, errno,  __VA_ARGS__), (void *) NULL) 
#define fatal_error(...) _fatal_error(__FILE__, __LINE__, __func__, 0,  __VA_ARGS__)
#define fatal_errno(...) _fatal_error(__FILE__, __LINE__, __func__, errno,  __VA_ARGS__)
#define log_errorn(...) (log_error(__VA_ARGS__), (void*)NULL)
#define log_debug(fmt, ...) { fprintf(stderr, fmt, ##__VA_ARGS__); fprintf(stderr, "\n"); }


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

static inline int slice_cmp_cstr(struct str_slice str, const char *cstr, int len)
{
    return len == str.len && memcmp(str.ptr, cstr, len) == 0;
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

// doubly linked list code
struct list_elem {
    struct list_elem *next;
    struct list_elem *prev;
};

#define list_entry(ptr, type, member) containerof(ptr, type, member)
#define list_empty(list) ((list)->next == (list))
#define list_inuse(elem) ((elem)->next != NULL)

#define list_first_entry(ptr, type, field) list_entry((ptr)->next, type, field)
#define list_next_entry(ptr, field) list_entry((ptr)->field.next, __typeof__(*ptr), field)
#define list_prev_entry(ptr, field) list_entry((ptr)->field.prev, __typeof__(*ptr), field) 

// iterate over a list (cannot be modifed)
#define list_fornext(head, elem) \
    for ((elem) = (head)->next; (elem) != (head); (elem) = (elem)->next)

#define list_forprev(head, elem) \
    for ((elem) = (head)->prev; (elem) != (head); (elem) = (elem)->prev)

// iterate over a list (can be modifed)
#define list_fornext_safe(head, elem, next) \
    for ((elem) = (head)->next, (next) = (elem)->next; \
        (elem) != (head); \
        (elem) = (next), (next) = (elem)->next)

#define list_forprev_safe(head, elem, prev) \
    for ((elem) = (head)->prev, (prev) = (elem)->prev; \
        (elem) != (head); \
        (elem) = (prev), (prev) = (elem)->prev)

// iterate over list entries (cannot be modifed)
#define list_fornext_entry(entry, head, field) \
    for ((entry) = list_first_entry(head, entry, field); \
        &(entry)->field != (head); \
        (entry) = list_next_entry(entry, field))

// iterate over list entries (can be modifed)
#define list_fornext_entry_safe(entry, next, head, field) \
    for ((entry) = list_first_entry(head, __typeof__(*entry), field), \
        (next) = list_next_entry(entry, field); \
        &(entry)->field != (head); \
        (entry) = (next), (next) = list_next_entry(next, field))


// init list elem to point to itself
static inline void list_init(struct list_elem *elem)
{
    elem->next = elem;
    elem->prev = elem;
}

// prev <-> node <-> next
static inline void list_chain(struct list_elem *prev,
    struct list_elem *node, struct list_elem *next)
{
    next->prev = node;
    node->next = next;
    node->prev = prev;
    prev->next = node;
}

// add node to start of list
static inline void list_prepend(struct list_elem *head, struct list_elem *node)
{
    list_chain(head, node, head->next);
}

// add node to end of list
static inline void list_append(struct list_elem *head, struct list_elem *node)
{
     list_chain(head->prev, node, head);
}

// remove node from list
static inline void list_remove(struct list_elem *elem)
{
    elem->prev->next = elem->next;
    elem->next->prev = elem->prev;
    elem->next = NULL;
    elem->prev = NULL;
}

static inline void list_replace(struct list_elem *old_elem, struct list_elem *new_elem)
{
    new_elem->prev = old_elem->prev;
    new_elem->next = old_elem->next;

    old_elem->prev = NULL;
    old_elem->next = NULL;

    new_elem->prev->next = new_elem;
    new_elem->next->prev = new_elem;
}

#endif
