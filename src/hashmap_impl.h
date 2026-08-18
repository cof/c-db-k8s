/* SPDX-License-Identifier: MIT | (c) 2026 [cof] */

/* 
 * X-Macro hash map - XXX dont add include guards
 *
 * Design:
 * - key 0 reserved as empty, key 0 remaps 0 to -1
 * - Fibonacci hash
 * - open addressing / linear Probing
 * - backshift-shift for deletes
 */
typedef struct MAP_FN(entry) {
    KEY_TYPE key;
    VAL_TYPE val;
} MAP_FN(entry);

typedef struct HASHMAP_TYPE {
    MAP_FN(entry) *buckets;
    uint32_t capacity;
    uint32_t threshold;
    uint32_t size;
    uint32_t mask;
    uint32_t nbits;
    unsigned int is_fixed : 1;
} HASHMAP_TYPE;

static inline KEY_TYPE MAP_FN(sanitize)(KEY_TYPE k)
{
    return k == 0 ? (KEY_TYPE) -1 : k;
}

static inline uint32_t MAP_FN(calc_bits)(uint32_t n)
{
    return n ? 32 - __builtin_clz(n) : 0;
}

// resize map to new capacity
static inline uint32_t MAP_FN(resize)(HASHMAP_TYPE *m, uint32_t capacity)
{
    if (m->is_fixed) return 0;

    // calc new capacity
    uint32_t nbits = MAP_FN(calc_bits)(capacity - 1);
    if (nbits <  2) nbits = 2;
    capacity = 1 << nbits;

    // alloc memory
    MAP_FN(entry) *buckets = calloc(capacity, sizeof(*buckets));
    if (!buckets) return 0;

    // rehash keys
    uint32_t size = 0;
    uint32_t mask = capacity - 1;
    for (uint32_t i = 0; i < m->capacity; i++) {
        KEY_TYPE key = m->buckets[i].key;
        if (key) {
            VAL_TYPE val = m->buckets[i].val;
            uint32_t idx = KEY_HASH(key, nbits);
            while (buckets[idx].key) {
                idx = (idx + 1) & mask;
            }
            buckets[idx] = (MAP_FN(entry)) { key, val };
            size++;
        }
    }

    if (m->buckets) free(m->buckets);
    m->buckets  = buckets;
    m->capacity = capacity;
    m->threshold = (capacity >> 1) + (capacity >> 2);
    m->size     = size;
    m->mask     = mask;
    m->nbits    = nbits;

    // all done
    return 1;
}

// initialize map
static inline uint32_t MAP_FN(init)(HASHMAP_TYPE *m, uint32_t capacity)
{
    return MAP_FN(resize)(m, capacity);
}

// free map memory
static inline void MAP_FN(free)(HASHMAP_TYPE *m)
{
    if (m->buckets && !m->is_fixed) {
        free(m->buckets);
        m->buckets = NULL;
    }
}

static inline int MAP_FN(attach)(HASHMAP_TYPE *m, MAP_FN(entry) *buffer, uint32_t capacity)
{
    // capacity MUST be at least 4 and a power of 2
    if (capacity < 4 || (capacity & (capacity - 1))) return 0;

    // setup static buffer
    m->buckets = buffer;
    m->capacity = capacity;
    m->threshold = (capacity >> 1) + (capacity >> 2);
    m->size = 0;
    m->mask  = m->capacity -1;
    m->nbits = MAP_FN(calc_bits)(capacity - 1);
    m->is_fixed = 1;

    return 1;
}

// return map end - aka capacity
static inline uint32_t MAP_FN(end)(HASHMAP_TYPE *m)
{
    return m->capacity;
}

// return elements in map
static inline uint32_t MAP_FN(size)(HASHMAP_TYPE *m)
{
    return m->size;
}

// put key, value in map - return index or end
static inline uint32_t MAP_FN(put)(HASHMAP_TYPE *m, KEY_TYPE key, VAL_TYPE val)
{
    if (m->size >= m->threshold && !MAP_FN(resize)(m, m->capacity + 1)) {
        return m->capacity;
    }

    if (key == (KEY_TYPE) -1) return m->capacity;
    key = MAP_FN(sanitize)(key);
    uint32_t idx = KEY_HASH(key, m->nbits);
    uint32_t end = idx;

    while (m->buckets[idx].key != 0) {
        if (KEY_EQ(m->buckets[idx].key, key)) break;
        idx = (idx + 1) & m->mask;
        if (idx == end) return m->capacity;
    }

    if (!m->buckets[idx].key) m->size++;
    m->buckets[idx] = (MAP_FN(entry)) { key, val };

    return idx;
}

// get key in map - return index or end
static inline uint32_t MAP_FN(get)(HASHMAP_TYPE *m, KEY_TYPE key)
{
    if (m->size == 0) return m->capacity;

    key = MAP_FN(sanitize)(key);
    uint32_t idx = KEY_HASH(key, m->nbits);
    uint32_t end = idx;

    while (m->buckets[idx].key != 0) {
        if (KEY_EQ(m->buckets[idx].key, key)) break;
        idx = (idx + 1) & m->mask;
        if (idx == end) return m->capacity;
    }

    return m->buckets[idx].key ? idx : m->capacity;
}

/*
 * Backshift - should the key at nidx move into hole idx?
 * =========================================
 * i = hole slot where key deleted
 * n = current key location
 * k = home location of key (hash(key)
 *
 * Two circular probe cases
 *
 * Case 1: i <= n
 *
 *  0|1|2|3|4|5|6|7
 *    a b c
 *    k i n
 *
 *    k <= i OR k > n
 *
 * Case 2: i > n
 *
 *  0|1|2|3|4|5|6|7
 *    a   b     c
 *    n   k     i
 *
 *    k > n AND k <= i
 */
static inline int MAP_FN(must_backshift)(uint32_t idx, uint32_t nidx, uint32_t kidx)
{
#ifdef HASHMAP_BACKSHIFT_NORMAL
    // gcc branch, clang branhless
    return idx <= nidx
        ? kidx <= idx || kidx > nidx
        : kidx > nidx && kidx <= idx;
#else
    // gcc/clang branchless
    return (nidx < idx) ^ (kidx <= idx) ^ (kidx > nidx);
#endif
}

// remove index entry from map - uses "Algorithm R" aka backshifting
static inline uint32_t MAP_FN(rem)(HASHMAP_TYPE *m, uint32_t idx)
{
    if (m->size == 0 || idx >= m->capacity) return m->capacity;
    uint32_t nidx = idx;

    while (1) {
        nidx = (nidx + 1) & m->mask;
        if (nidx == idx || m->buckets[nidx].key == 0) break;
        uint32_t kidx = KEY_HASH(m->buckets[nidx].key, m->nbits);
        if (MAP_FN(must_backshift)(idx, nidx, kidx)) {
            m->buckets[idx] = m->buckets[nidx];
            idx = nidx;
        }
    }

    m->buckets[idx].key = 0;
    m->size--;

    return idx;
}

// delete key from map - return index or end
static inline uint32_t MAP_FN(del)(HASHMAP_TYPE *m, KEY_TYPE key)
{
    key = MAP_FN(sanitize)(key);
    uint32_t idx = MAP_FN(get)(m, key);
    return idx == m->capacity ? idx : MAP_FN(rem)(m, idx);
}

// return key at index
static inline KEY_TYPE MAP_FN(key)(HASHMAP_TYPE *m, uint32_t idx)
{
    if (idx >= m->capacity || !m->buckets[idx].key) return 0;
    return m->buckets[idx].key;
}

// return value at index
static inline VAL_TYPE MAP_FN(val)(HASHMAP_TYPE *m, uint32_t idx)
{
    if (idx >= m->capacity || !m->buckets[idx].key) return 0;
    return m->buckets[idx].val;
}

// update value at index
static inline uint32_t MAP_FN(set)(HASHMAP_TYPE *m, uint32_t idx, VAL_TYPE val)
{
    if (idx >= m->capacity || !m->buckets[idx].key) return m->capacity;
    m->buckets[idx].val = val;
    return idx;
}
