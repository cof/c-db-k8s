/* SPDX-License-Identifier: MIT | (c) 2026 [cof] */

/*
 * DB - a simple key:value store API
 * ---------------------------------
 * A simple API for key value store management featuring
 * - Flexible storage : can use a pure memory store or mmap file for database
 * - Single block     : record header, key and value stored as one block
 * - file : fixed-size file with header, bucket table and aligned records
 * - file : validated on open (magic, version, bounds, alignment, chain loops)
 * - file : append-only - deleted records are flagged, space not yet reclaimed
 */
#ifndef _DB_H_
#define _DB_H_

#include "str_util.h" // struct slice

// header id
#define DB_MAGIC 0x4D594442U // MYDB

// error codes
#define DB_FAIL   (-1)
#define DB_REINIT (-2)

/*
 * Initialization
 * --------------
 * db_init(file) : file store if set, else memory - returns 0 ok, < 0 on error
 * db_deinit()   : shutdown store
 *
 * key-value api
 * -------------
 * db_set(key, value) : store key value - returns 0 ok, DB_FAIL on error
 * db_get(key)        : get value for key - returns empty slice if not found
 * db_del(key)        : delete key - returns 0 if ok, -1 if not found
 */

int db_init(const char *file);
void db_deinit(void);

int db_set(struct slice key, struct slice val);
struct slice db_get(struct slice key);
int db_del(struct slice key);

#endif
