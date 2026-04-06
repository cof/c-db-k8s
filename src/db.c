/*
 * DB a simple key:value store API
 * --------------------------------
 * See db.h for API description
 */
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <endian.h>

#include "config.h"
#include "util.h"
#include "log.h"
#include "db.h"

/* 
 * 10000 entries
 * load factor 2/3
 * table size 10000 * 3/2 = 15000
 * nearest prime is 15013
 */
#define DB_MAX_REC     10000
#define DB_NUM_BUCKETS 15013

#define DB_REC_DEL 0x1
#define DB_FILE_MODE 0666

// databse record - note key and val are stored at end of rec
struct db_rec {
    uint64_t next;
    uint64_t flags;
    size_t key_len;
    size_t val_len;
    char data[];
};

// database file header
struct db_hdr {
    uint32_t magic;
    uint32_t version;
    uint64_t num_rec;
    uint64_t num_del;
    uint64_t next_offset;
};

// our database
static uint64_t *db_buckets;
static void     *db_mmap_ptr;
static size_t   db_size;
static struct db_hdr *db_hdr;
static int      db_file;
static int      init_done;

// calc hash for key
static int hash(struct str_slice key)
{
    return dbj2a_hash(key.ptr, key.len) % DB_NUM_BUCKETS;
}

// note key and val are stored directly in entry
static struct db_rec *create_rec(struct str_slice key, struct str_slice val)
{
    struct db_rec *rec;
    size_t rec_size = sizeof(*rec) + key.len + val.len;

    if (db_file) {
         if (db_hdr->next_offset + rec_size > db_size) {
            return log_error_rn("mmap rec_size %zu failed", rec_size);
         }
         rec = make_ptr(db_mmap_ptr, db_hdr->next_offset);
         db_hdr->next_offset += rec_size;
         db_hdr->num_rec++;
    }
    else {
        rec = malloc(rec_size);
        if (!rec) {
            return log_errno_rn("malloc rec_size %zu failed", rec_size);
        }
    }

    rec->next = 0;
    rec->flags = 0;
    rec->key_len = key.len;
    rec->val_len = val.len;
    memcpy(rec->data, key.ptr, key.len);
    memcpy(rec->data + key.len, val.ptr, val.len);

    return rec;
}

static void del_rec(struct db_rec *rec)
{
    if (db_file) {
        rec->flags |= DB_REC_DEL;
        db_hdr->num_del++;
    }
    else {
        free(rec);
    }
}

// delete from hash table
static int hash_del(struct str_slice key)
{
    uint32_t idx = hash(key);
    uint64_t *pp = &db_buckets[idx];

    while (*pp) {
        // get hash or mmap entry
        struct db_rec *rec = db_file
            ? make_ptr(db_mmap_ptr, *pp)
            : make_mem(*pp);
        if (slice_cmp_mem(key, rec->data, key.len)) {
            // unchain
            *pp = rec->next; 
            // delete existing entry
            del_rec(rec);
            // all done
            return 0;
        }
        // next entry
        pp = &rec->next; 
    }

    // not found
    return -1;
}

// lookup key in hash bucket
static struct db_rec *hash_search(int idx, struct str_slice key)
{
    uint64_t db_link = db_buckets[idx];

    while (db_link) {
        // get hash or mmap entry
        struct db_rec *rec = db_file    
            ? make_ptr(db_mmap_ptr, db_link)
            : make_mem(db_link);
        if (slice_cmp_mem(key, rec->data, key.len)) return rec;
        db_link = rec->next;
    }

    return NULL;
}

// lookup key in hash table
static struct db_rec *hash_find(struct str_slice key)
{
    int idx = hash(key);
    return hash_search(idx, key);
}

// store key value in database
static struct db_rec *hash_put(struct str_slice key, struct str_slice val)
{
    int idx = hash(key);
    uint64_t *pp = &db_buckets[idx];

    while (*pp) {
        // get hash or mmap entry
        struct db_rec *rec = db_file
            ? make_ptr(db_mmap_ptr, *pp)
            : make_mem(*pp);
        // does record match key
        if (slice_cmp_mem(key, rec->data, key.len)) {
            // replace existing entry
            struct db_rec *new_rec = create_rec(key, val);
            if (!new_rec) return NULL;
            // chain
            new_rec->next = rec->next;
            *pp = db_file 
                ? make_offset(db_mmap_ptr, new_rec)
                : unmake_mem(new_rec);
            // all done
            del_rec(rec);
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
        ? make_offset(db_mmap_ptr, new_rec)
        : unmake_mem(new_rec);

    return new_rec;
}

// check database file is valid
static int file_check(void)
{
    uint64_t db_link, db_offset;
    struct db_rec *rec;
    size_t rec_size;
    
    if (be32toh(db_hdr->magic) != DB_MAGIC) {
        return log_error_rf("db-hdr magic 0x%08x != 0x%08d", db_hdr->magic, DB_MAGIC);
    }

    if (db_hdr->version != 1) {
        return log_error_rf("db-hdr version %u != %d", db_hdr->version, 1);
    }

    for (int i = 0; i < DB_NUM_BUCKETS; i++) {
        db_link = db_buckets[i];
        while (db_link) {
            rec_size = sizeof(*rec);
            if (db_link + rec_size > db_size) {
                return log_error_rf("Truncated rec-hdr at offset %lu", db_link);
            }
            rec = make_ptr(db_mmap_ptr, db_link);
            rec_size += rec->key_len + rec->val_len;
            if (db_link + rec_size > db_size) {
                return log_error_rf("truncated rec-data at offset %lu", db_link);
            }
            db_offset += rec_size;
            db_link = rec->next;
        }
    }

    return 0;
}

// create a mmap database file
static int file_init(const char *file)
{
    // create file
    int new_file = 1;
    int fd = open(file, O_RDWR | O_CREAT | O_EXCL, DB_FILE_MODE);
    if (fd == -1) {
        // open file if it exists
        if (errno != EEXIST) {
            return log_errno_rf("open db-file %s failed", file);
        }
        // open existing file
        fd = open(file, O_RDWR, 0);
        if (fd == -1) {
            return log_errno_rf("open db-file %s failed", file);
        }
        new_file = 0;
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        int ec = errno;
        close(fd);
        return log_ec_rf(ec, "fstat db-file %s failed", file);
    }

    // calc min file size
    size_t table_size = DB_NUM_BUCKETS * sizeof(uint64_t);
    size_t file_size = 0;
    file_size += sizeof(*db_hdr);
    file_size += table_size;
    file_size += DB_MAX_REC * MAX_LINE;

    if ((size_t) st.st_size < file_size) {
        /// need to resize file
        if (ftruncate(fd, file_size) == -1) {
            int ec = errno;
            close(fd);
            return log_ec_rf(ec, "open db-file %s failed", file);
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
    db_size = file_size;
    db_hdr = make_ptr(db_mmap_ptr, 0);
    db_buckets = make_ptr(db_mmap_ptr, sizeof(*db_hdr));

    if (new_file) {
        db_hdr->magic = htobe32(DB_MAGIC);
        db_hdr->version = 1;
        db_hdr->num_rec = 0;
        db_hdr->next_offset = sizeof(*db_hdr) + table_size;
        memset(db_buckets, 0, table_size);
    }
    else {
        int rc = file_check();
        if (rc) return rc;
    }

    return 0;
}

// free hash table memory
static void hash_deinit(void)
{
    if (!db_buckets) return;

    for (int i = 0; i < DB_NUM_BUCKETS; i++) {
        // free chain in every slot
        while (db_buckets[i]) {
            struct db_rec *rec = (struct db_rec *) db_buckets[i];
            db_buckets[i] = rec->next;
            del_rec(rec);
        }
    }

    free(db_buckets);
    db_buckets = NULL;
}

// allocate hash table memroy
static int hash_init(void)
{
    db_buckets = calloc(DB_NUM_BUCKETS, sizeof(uintptr_t));

    if (!db_buckets) {
        return log_errno_rf("calloc buckets %d failed", DB_NUM_BUCKETS);
    }

    return 0;
}

// setup database - if file set use file store else use memory store
int db_init(const char *file)
{
    if (init_done) return DB_REINIT;

    int rc = file
        ? file_init(file)
        : hash_init();

    if (rc) return rc;

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
    struct db_rec *rec = hash_find(key);

    if (!rec) {
        // not found
        return slice_make(NULL, 0);
    }

    return slice_make(rec->data + rec->key_len, rec->val_len);
}

int db_del(struct str_slice key)
{
    int rc = hash_del(key);

    return rc;
}

