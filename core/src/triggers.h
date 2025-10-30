#ifndef CRSQLITE_TRIGGERS_H
#define CRSQLITE_TRIGGERS_H

#include "sqlite3ext.h"
#include "tableinfo.h"

int crsql_create_triggers(sqlite3 *db, crsql_TableInfo *table_info, char **err);
int crsql_create_insert_trigger(sqlite3 *db, crsql_TableInfo *table_info, char **err);
int crsql_create_update_trigger(sqlite3 *db, crsql_TableInfo *table_info, char **err);
int crsql_create_delete_trigger(sqlite3 *db, crsql_TableInfo *table_info, char **err);

#endif
