/*
 * DB  implements a simple key:value store
 */

#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "util.h"
#include "log.h"
#include "db.h"


// assume 10000 entries,load factor 2/3
// table size 10000 * 3/2 = 15000
// nearest prime is 15013
#define DB_TABLE_SIZE 15013
#define DB_HDR_SIZE (2 * sizeof(uint64_t))
#define DB_FILE_SIZE (4 * 1024)

// note key and val are stored directly in db record
struct db_rec {
    uint64_t next;
    size_t key_len;
    size_t val_len;
    char data[];
};

// our database
static uint64_t *db_buckets;
static void     *db_mmap_ptr;
static size_t   db_file_size;
static size_t   db_offset;
static int      db_file;
static int      init_done;

static int hash(const void *key, int klen)
{
    return dbj2a_hash(key, klen) % DB_TABLE_SIZE;
}

// note key and val are stored directly in entry
static struct db_rec *create_rec(struct str_slice key, struct str_slice val)
{
    struct db_rec *rec;
    size_t rec_size = sizeof(*rec) + key.len + val.len;

    if (db_file) {
         rec = (struct db_rec *) make_ptr(db_mmap_ptr, db_offset);
         db_offset += rec_size;
    }
    else {
        rec = malloc(rec_size);
    }

    if (!rec) return NULL;

    rec->next = 0;
    rec->key_len = key.len;
    rec->val_len = val.len;
    memcpy(rec->data, key.ptr, key.len);
    memcpy(rec->data + key.len, val.ptr, val.len);

    return rec;
}

static void free_rec(struct db_rec *rec)
{
    free(rec);
}

static int hash_del(struct str_slice key)
{
    uint32_t idx = hash(key.ptr, key.len);
    uint64_t *pp = &db_buckets[idx];

    while (*pp) {
        // get hash or mmap entry
        struct db_rec *rec = db_file
            ? (struct db_rec *) make_ptr(db_mmap_ptr, *pp)
            : (struct db_rec *) (uintptr_t)(*pp);
        if (slice_cmp_cstr(key, rec->data, key.len)) {
            // unchain
            *pp = rec->next; 
            // delete existing entry
            if (!db_file) free_rec(rec);
            // all done
            return 0;
        }
        // next entry
        pp = &rec->next; 
    }

    // not found
    return -1;
}

static struct db_rec *hash_search(int idx, struct str_slice key)
{
    uint64_t db_link = db_buckets[idx];

    while (db_link) {
        // get hash or mmap entry
        struct db_rec *rec = db_file    
            ?  (struct db_rec *) make_ptr(db_mmap_ptr, db_link)
            :  (struct db_rec *) (uintptr_t) db_link;
        if (slice_cmp_cstr(key, rec->data, key.len)) {
            return rec;
        }
        db_link = rec->next;
    }

    return NULL;
}

static struct db_rec *hash_find(struct str_slice key)
{
    return hash_search(hash(key.ptr, key.len), key);
}

static struct db_rec *hash_put(struct str_slice key, struct str_slice val)
{
    int idx = hash(key.ptr, key.len);

    // fist check if entry already exists
    uint64_t *pp = &db_buckets[idx];
    while (*pp) {
        // get hash or mmap entry
        struct db_rec *rec = db_file
            ? (struct db_rec *) make_ptr(db_mmap_ptr, *pp)
            : (struct db_rec *) (uintptr_t)(*pp);
        if (slice_cmp_cstr(key, rec->data, key.len)) {
            // replace existing entry
            struct db_rec *new_rec = create_rec(key, val);
            if (!new_rec) return NULL;
            // chain
            new_rec->next = rec->next;
            *pp = new_rec->next;
            // all done
            if (!db_file) free_rec(rec);
            return new_rec;
        }
        pp = &rec->next;
    }

    // new entry
    struct db_rec *new_rec = create_rec(key, val);
    if (!new_rec) return NULL;

    // chain
    new_rec->next = db_buckets[idx];
    db_buckets[idx] = db_file
        ? (uint64_t) ((char*) new_rec - (char*) db_mmap_ptr)
        : (uintptr_t) new_rec;

    return new_rec;
}

// create a mmap db file
static int file_init(const char *file_name)
{
    int fd = open(file_name, O_RDWR | O_CREAT, 0666);
    if (fd == -1) {
        return log_errno_rf("open db-file %s failed", file_name);
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        int _errno = errno;
        close(fd);
        errno = _errno;
        return log_errno_rf("fstat db-file %s failed", file_name);
    }

    size_t file_size = DB_HDR_SIZE + DB_TABLE_SIZE * sizeof(uint64_t) + 1024;
    if (st.st_size < file_size) {
        if (ftruncate(fd, file_size) == -1) {
            int _errno = errno;
            close(fd);
            errno = _errno;
            return log_errno_rf("open db-file %s failed", file_name);
        }
    }

    void *mmap_ptr = mmap(NULL, file_size,
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0
    ); 
    close(fd);
    if (mmap_ptr == MAP_FAILED) {
        return log_errno_rf("mmap stack %zu failed", file_size);
    }

    // setup db file
    db_file = 1;
    db_mmap_ptr = mmap_ptr;
    db_file_size = file_size;
    db_buckets = make_ptr(db_mmap_ptr, DB_HDR_SIZE);
    db_offset = DB_HDR_SIZE + DB_TABLE_SIZE * sizeof(uint64_t); 
    memset(db_buckets, 0, DB_TABLE_SIZE * sizeof(uint64_t));

    return 0;
}

static void hash_deinit(void)
{
    for (int i = 0; i < DB_TABLE_SIZE; i++) {
        while (db_buckets[i]) {
            struct db_rec *rec = (struct db_rec *) db_buckets[i];
            db_buckets[i] = rec->next;
            free_rec(rec);
        }
    }
}

static void hash_init(void)
{
    db_buckets = calloc(DB_TABLE_SIZE, sizeof(uintptr_t));
}

int db_init(const char *file_name)
{
    if (init_done) return -1;

    if (file_name) {
        file_init(file_name);
    }
    else {
        hash_init();
    }

    init_done = 1;

    return 0;
}

void db_deinit()
{
    if (!db_file) {
        hash_deinit();
    }
}

int db_set(struct str_slice key, struct str_slice val)
{
    struct db_rec *rec;

    rec = hash_put(key, val);
    
    return rec ? 0 : DB_FAIL;
}

struct str_slice db_get(struct str_slice key)
{
    struct db_rec *rec;

    rec = hash_find(key);
    if (!rec) return slice_make(NULL, 0);

    return slice_make(rec->data + rec->key_len, rec->val_len);
}

int db_del(struct str_slice key)
{
    int rc = hash_del(key);

    return rc;
}

