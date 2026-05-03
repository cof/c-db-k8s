/*
 * ARR_LEN(a) - return length of array
 * ARRAY(a) 
 * STR_LIT(s)  - return a string literal and its length
 * ALIGN_UP(n, a)
 * -
 * RMCONST(_t, _v)  - de-const a pointer without a naked (char*)
 * containerof(ptr, type, member)  - retutn ptr to strucure containg field
 * mkptr(ptr, offset)  - cast ptr to char *, adding offset
 * mkoffset(base, ptr) - calc offset in bytes between 2 ptrs 
 * mkmem(val)   - convert uint64_t to ptr  (aka TOPTR)
 * umkmem(val)  - convert ptr to uint64_t  (aka FROMPTR)
 * -
 * XSTR(a)
 * STR(a) 
 */
#ifndef _MACROS__H_
#define _MACROS_H_

#include <stddef.h>
#include <stdint.h>

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

#endif
