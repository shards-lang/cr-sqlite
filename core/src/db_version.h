#ifndef CRSQLITE_DB_VERSION_H
#define CRSQLITE_DB_VERSION_H

#include "sqlite3ext.h"
#include "ext-data.h"

// Fill the dbVersion in ext_data if needed (checks if data has changed)
int crsql_fill_db_version_if_needed(sqlite3 *db, crsql_ExtData *ext_data, char **errmsg);

// Get the next db_version (increments from current, handles merging versions)
// Returns -1 on error
sqlite3_int64 crsql_next_db_version(sqlite3 *db, crsql_ExtData *ext_data,
                                    sqlite3_int64 merging_version, char **errmsg);

#endif
