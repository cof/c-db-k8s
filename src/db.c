#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "db.h"


// assume 10000 entries,load factor 2/3
// table size 10000 * 3/2 = 15000
// nearest prime is 15013
#define HASHSIZE 15013

// note key and val are stored directly in db entry
struct db_rec {
    struct db_rec *next;
    size_t key_len;
    size_t val_len;
    char data[];
};

static struct db_rec *hash_table[HASHSIZE];

static int hash(const void *key, int klen)
{
    return dbj2a_hash(key, klen) % HASHSIZE;
}

static void hash_init(void)
{
    memset(hash_table, 0, sizeof(hash_table));
}

static void hash_deinit(void)
{
    struct db_rec *np;
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
static struct db_rec *create_entry(const void *key, int klen, const void *val, int vlen)
{
    struct db_rec *rec;

    rec = malloc(sizeof(*rec) + klen + vlen);;
    if (!rec) return NULL;

    rec->next = NULL;
    rec->key_len = klen;
    rec->val_len = vlen;
    memcpy(rec->data, key, klen);
    memcpy(rec->data + klen, val, vlen);

    return rec;
}

static void free_entry(struct db_rec *rec)
{
    free(rec);
}

static struct db_rec *hash_search(int idx, const char *key, int key_len)
{
    struct db_rec *rec;

    for (rec = hash_table[idx]; rec; rec = rec->next) {
        if (key_len == rec->key_len && !memcmp(rec->data, key, key_len)) {
            return rec;
        }
    }

    return NULL;
}

static struct db_rec *hash_put(const void *key, int klen, const void *val, int vlen)
{
    int idx = hash(key, klen);

    // fist check if entry already exists
    struct db_rec **pp = &hash_table[idx];
    while (*pp) {
        struct db_rec *crec = *pp;
        if (klen == crec->key_len && !memcmp(crec->data, key, klen)) {
            // replace existing entry
            struct db_rec *nrec = create_entry(key, klen, val, vlen);
            if (!nrec)  {
                return NULL;
            }
            // chain
            nrec->next = crec->next;
            *pp = nrec;
            // all done
            free_entry(crec);
            return nrec;
        }
        pp = &crec->next;
    }

    // new entry
    struct db_rec *nrec = create_entry(key, klen, val, vlen);
    if (!nrec) {
        return NULL;
    }

    // chain
    nrec->next = hash_table[idx];
    hash_table[idx] = nrec;

    return nrec;
}

static int hash_del(const void *key, int klen)
{
    int idx = hash(key, klen);

    // fist check if entry already exists
    struct db_rec **pp = &hash_table[idx];
    while (*pp) {
        struct db_rec *np = *pp;
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

static struct db_rec *hash_find(const void *key, int klen)
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
    struct db_rec *rec;

    rec = hash_put(key.ptr, key.len, val.ptr, val.len);
    
    return rec ? 0 : DB_FAIL;
}

struct str_slice db_get(struct str_slice key)
{
    struct db_rec *rec;

    rec = hash_find(key.ptr, key.len);
    if (!rec) return slice_make(NULL, 0);

    return slice_make(rec->data + rec->key_len, rec->val_len);
}

int db_del(struct str_slice key)
{
    int rc = hash_del(key.ptr, key.len);

    return rc;
}

