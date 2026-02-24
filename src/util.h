#ifndef __UTIL_H__
#define __UTIL_H__

// general purpose macros
#define ARR_LEN(a) (sizeof(a) / sizeof(a[0]))
#define STR_LIT(s) (s), (sizeof(s) - 1)

#define containerof(ptr, type, member) ((type *)((char *)(ptr)-offsetof(type, member)))

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


// doubly linked list
struct list_elem {
    struct list_elem *next;
    struct list_elem *prev;
};

#define list_entry(ptr, type, member) containerof(ptr, type, member)
#define list_empty(list) ((list)->next == (list))
#define list_inuse(elem) ((elem)->next != NULL)

#define list_first_entry(list, ptr, field) list_entry((list)->next, __typeof__(ptr), field)
#define list_next_entry(ptr, field) list_entry((ptr)->field.next, __typeof__(ptr), field)
#define list_prev_entry(ptr, field) list_entry((ptr)->field.prev, __typeof__(ptr), field) 

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
    for ((entry) = list_first_entry(head, entry, field), \
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
