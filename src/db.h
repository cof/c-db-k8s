#ifndef __DB_H__
#define __DB_H__

int db_init(void);
void db_deinit(void);

int db_set(struct str_slice key, struct str_slice val);
struct str_slice db_get(struct str_slice key);
int db_del(struct str_slice key);


#endif
