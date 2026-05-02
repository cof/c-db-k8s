/*
 * DB - a simple key:value store API
 * ---------------------------------
 * A simple API for key value store management featuring
 * - Flexible storage : can use a pure memory store or mmap file for database
 * - file : mmap file is secure and safe from corruption
 * - file : uses a header and
 * - No fragmention : record state, key, value are stored as one block in memory
 */
#ifndef _DB_H_
#define _DB_H_

// errors hdr id
#define DB_FAIL   -1
#define DB_REINIT -2
#define DB_MAGIC 0x4D594442 // MYDB

/*
 * Initialization
 * --------------
 * db_init(file) : setup database - use file store if set else use memory store
 * db_deinit()   : shutdown database
 *
 * key-value api
 * -------------
 * db_set(key,vale) : store a key value
 * db_get(key)      : get value for key
 * db_del(key)      : delete key
 */

int db_init(const char *file);
void db_deinit(void);

int db_set(struct str_slice key, struct str_slice val);
struct str_slice db_get(struct str_slice key);
int db_del(struct str_slice key);

#endif
