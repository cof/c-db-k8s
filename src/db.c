#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "db.h"

//  djb2a hash algor
static unsigned long dbj2a(const void *key, const int klen)
{
    const unsigned char *data,*end;
    unsigned long hash = 5381;

    end = key + klen;
    for (data = key; data < end; data++) {
        hash = ((hash << 5) + hash) ^ *data;
    }

    return hash;
}

// assume 10000 entries,load factor 2/3
// table size 10000 * 3/2 = 15000
// nearest prime is 15013
#define HASHSIZE 15013

// note key and val are stored directly in db entry
struct nlist {
    struct nlist *next;
    size_t key_len;
    size_t val_len;
    char data[];
};

static struct nlist *hash_table[HASHSIZE];

static int hash(const void *key, int klen)
{
    return dbj2a(key, klen) % HASHSIZE;
}

static void hash_init(void)
{
    memset(hash_table, 0, sizeof(hash_table));
}

static void hash_deinit(void)
{
    struct nlist *np;
    int i;

    for (i=0; i < HASHSIZE; i++) {
        while (hash_table[i]) {
            np = hash_table[i];
            hash_table[i] = np->next;
            free(np);
        }
    }
}


// note key and val are stored directly in entry
static struct nlist *create_entry(const void *key, int klen, const void *val, int vlen)
{
    struct nlist *np = malloc(sizeof(*np) + klen + vlen);;
    if (!np) return NULL;

    np->next = NULL;
    np->key_len = klen;
    np->val_len = vlen;
    memcpy(np->data, key, klen);
    memcpy(np->data + klen, val, vlen);

    return np;
}

static void free_entry(struct nlist *np)
{
    free(np);
}

static struct nlist *hash_search(int idx, const char *key, int key_len)
{
    struct nlist *np;

    for (np = hash_table[idx]; np; np = np->next) {
        if (key_len == np->key_len && !memcmp(np->data, key, key_len)) {
            return np;
        }
    }

    return NULL;
}

static struct nlist *hash_put(const void *key, int klen, const void *val, int vlen)
{
    int idx = hash(key, klen);

    // fist check if entry already exists
    struct nlist **pp = &hash_table[idx];
    while (*pp) {
        struct nlist *np = *pp;
        if (klen == np->key_len && !memcmp(np->data, key, klen)) {
            // replace existing entry
            struct nlist *rp = create_entry(key, klen, val, vlen);
            if (!rp) return NULL;
            // chain
            rp->next = np->next;
            *pp = rp;
            // all done
            free_entry(np);
            return rp;
        }
        pp = &np->next;
    }

    // new entry
    struct nlist *np = create_entry(key, klen, val, vlen);
    if (!np) return NULL;

    // chain
    np->next = hash_table[idx];
    hash_table[idx] = np;

    return np;
}

static int hash_del(const void *key, int klen)
{
    int idx = hash(key, klen);

    // fist check if entry already exists
    struct nlist **pp = &hash_table[idx];
    while (*pp) {
        struct nlist *np = *pp;
        if (klen == np->key_len && !memcmp(np->data, key, klen)) {
            // unchain
            *pp = np->next; 
            // delete existing entry
            free_entry(np);
            // all done
            return 0;
        }
        // next entry
        pp = &np->next;
    }

    // not found
    return -1;
}

static struct nlist *hash_find(const void *key, int klen)
{
    return hash_search(hash(key, klen), key, klen);
}

int db_init(void)
{
    hash_init();

    return 0;
}

void db_deinit()
{
    hash_deinit();
}

int db_set(struct str_slice key, struct str_slice val)
{
    return hash_put(key.ptr, key.len, val.ptr, val.len) ? 1 : 0;
}

struct str_slice db_get(struct str_slice key)
{
    struct nlist *np = hash_find(key.ptr, key.len);
    return np
        ? slice_make(np->data + np->key_len, np->key_len) 
        : slice_make(NULL, 0);
}

int db_del(struct str_slice key)
{
    return hash_del(key.ptr, key.len) == 0;
}

