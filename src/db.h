/*
 * DB implements a key:value store

 * API
 * ---
 * db_init  - create database  -> rc = db_init(file_name)
 * db_deinit - shutdown database
 * db_set  - set a key value   -> db_set(key, value)
 * db_get  - get value for key -> value = db_get(ket)
 * db_del  - delete key e.g    -> rc = db_del(key)
 *
 */
#ifndef __DB_H__
#define __DB_H__

#define DB_FAIL   -1
#define DB_REINIT -2
#define DB_MAGIC 0x4D594442 // MYDB

int db_init(const char *file_name);
void db_deinit(void);

int db_set(struct str_slice key, struct str_slice val);
struct str_slice db_get(struct str_slice key);
int db_del(struct str_slice key);


#endif
