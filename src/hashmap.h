/*
 * HASHMAP 
 * ------
 * a hash map api featuring 
 * - C11 generics : type safe map operations
 * - X-Macros  : code generation, traceable code
 * - Fibonacci hashing : fast hashing
 * - open addressing : L1/L2/L3 cache friendly
 * - static bucket memory (fixed buffer)
 * - dynamic memory allocaiton (grows)
 *
 * MAP api
 * -------
 * hasmap32/hashmap64/hashmap32s/hashmap64s : map types
 * -
 * map_init(m, capacity)  : initialize map
 * map_free(m)            : free map memory
 * map_attach(m, buffer, capacity) : attach static buffer to map
 * -
 * map_end(m)             : return capacity
 * map_size(m)            : return elements in map
 * -
 * map_put(m, key, val)   : insert/update key,value entry
 * map_get(m, key)        : find key returns map_end() if not found
 * map_del(m, key)        : delete key returns map_end() if not found
 * -
 * map_key(m, index)      : get key at index
 * map_val(m, index)      : get value at index
 * map_rem(m, index)      : remove entry at index
 * map_set(m, index, val) : update value at index
 */
#ifndef _HASHMAP_H_
#define _HASHMAP_H_

#include <stdint.h>
#include <stdlib.h>

// 32 bit Fibonacci hashing
static inline uint32_t map_hash32(uint32_t key, int bits) 
{
    key ^= key >> 16;
    return (key * 2654435761U) >> (32 - bits);
}

static inline uint32_t map_hash64(uint64_t key, int bits)
{
    key ^= key >> 33;
    return (uint32_t)((key * 0x9E3779B97F4A7C15ULL) >> (64 - bits));
}

static inline uint32_t map_hashstr(const char *key, int bits)
{
    uint64_t hash = dbj2a_hash_str(key);
    return (hash * 0x9E3779B97F4A7C15ULL) >> (64 - bits);
}

static inline int map_streq(const char *a, const char *b) 
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    return strcmp(a, b) == 0;
}

// macros to add prefix to function names
#define CAT_INNER(a, b) a##_##b
#define CAT(a, b) CAT_INNER(a, b)
#define MAP_FN(name) CAT(MAP_PREFIX, name)

// -- 32-bit key+val --
#define MAP_TYPE   hashmap32
#define MAP_PREFIX map32
#define KEY_HASH   map_hash32
#define KEY_EQ(a,b) ((a) == (b))
#define KEY_TYPE   uint32_t
#define VAL_TYPE   uint32_t
#include "hashmap_impl.h"
#undef MAP_TYPE
#undef MAP_PREFIX
#undef KEY_HASH
#undef KEY_EQ
#undef KEY_TYPE
#undef VAL_TYPE

// -- 64-bit key+val --
#define MAP_TYPE   hashmap64
#define MAP_PREFIX map64
#define KEY_HASH   map_hash64
#define KEY_EQ(a,b) ((a) == (b))
#define KEY_TYPE   uint64_t
#define VAL_TYPE   uint64_t
#include "hashmap_impl.h"
#undef MAP_TYPE
#undef MAP_PREFIX
#undef KEY_HASH
#undef KEY_EQ
#undef KEY_TYPE
#undef VAL_TYPE

// -- str + 32-bit val --
#define MAP_TYPE   hashmap32s
#define MAP_PREFIX map32s
#define KEY_HASH   map_hashstr
#define KEY_EQ     map_streq
#define KEY_TYPE   char *
#define VAL_TYPE   uint32_t
#include "hashmap_impl.h"
#undef MAP_TYPE
#undef MAP_PREFIX
#undef KEY_HASH
#undef KEY_EQ
#undef KEY_TYPE
#undef VAL_TYPE

// -- str + 64-bit val --
#define MAP_TYPE   hashmap64s
#define MAP_PREFIX map64s
#define KEY_HASH   map_hashstr
#define KEY_EQ     map_streq
#define KEY_TYPE   char *
#define VAL_TYPE   uint64_t
#include "hashmap_impl.h"
#undef MAP_TYPE
#undef MAP_PREFIX
#undef KEY_HASH
#undef KEY_EQ
#undef KEY_TYPE
#undef VAL_TYPE

// -- map api -- using generic

#define map_init(M, C) _Generic((M), \
    hashmap32*:  map32_init,  \
    hashmap64*:  map64_init,  \
    hashmap32s*: map32s_init, \
    hashmap64s*: map64s_init  \
)(M, C)

#define map_free(M) _Generic((M), \
    hashmap32*:  map32_free,  \
    hashmap64*:  map64_free,  \
    hashmap32s*: map32s_free, \
    hashmap64s*: map64s_free  \
)(M)

#define map_attach(M, B, C) _Generic((M), \
    hashmap32*:  map32_attach,  \
    hashmap64*:  map64_attach,  \
    hashmap32s*: map32s_attach, \
    hashmap64s*: map64s_attach  \
)(M, B, C)

#define map_end(M) _Generic((M), \
    hashmap32*:  map32_end,  \
    hashmap64*:  map64_end,  \
    hashmap32s*: map32s_end, \
    hashmap64s*: map64s_end  \
)(M)

#define map_size(M) _Generic((M), \
    hashmap32*:  map32_size,  \
    hashmap64*:  map64_size,  \
    hashmap32s*: map32s_size, \
    hashmap64s*: map64s_size  \
)(M)

#define map_put(M, K, V) _Generic((M), \
    hashmap32*:  map32_put,  \
    hashmap64*:  map64_put,  \
    hashmap32s*: map32s_put, \
    hashmap64s*: map64s_put  \
)(M, K, V)

#define map_get(M, K) _Generic((M), \
    hashmap32*:  map32_get,  \
    hashmap64*:  map64_get,  \
    hashmap32s*: map32s_get, \
    hashmap64s*: map64s_get  \
)(M, K)

#define map_del(M, K) _Generic((M), \
    hashmap32*:  map32_del,  \
    hashmap64*:  map64_del,  \
    hashmap32s*: map32s_del, \
    hashmap64s*: map64s_del  \
)(M, K)

#define map_key(M, I) _Generic((M), \
    hashmap32*:  map32_key,  \
    hashmap64*:  map64_key,  \
    hashmap32s*: map32s_key, \
    hashmap64s*: map64s_key  \
)(M, I)

#define map_val(M, I) _Generic((M), \
    hashmap32*:  map32_val,  \
    hashmap64*:  map64_val,  \
    hashmap32s*: map32s_val, \
    hashmap64s*: map64s_val  \
)(M, I)

#define map_rem(M, I) _Generic((M), \
    hashmap32*:  map32_rem,  \
    hashmap64*:  map64_rem,  \
    hashmap32s*: map32s_rem, \
    hashmap64s*: map64s_rem  \
)(M, I)

#define map_set(M, I, V) _Generic((M), \
    hashmap32*:  map32_set,  \
    hashmap64*:  map64_set,  \
    hashmap32s*: map32s_set, \
    hashmap64s*: map64s_set  \
)(M, I, V)

#endif
