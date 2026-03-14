#ifndef CRSQLITE_DB_VERSION_H
#define CRSQLITE_DB_VERSION_H

#include "ext-data.h"

/**
 * Read the current db_version from storage if it has not been cached yet
 * (or if the data_version pragma indicates the db has changed).
 * On error, *errmsg is set to a sqlite3_malloc'd error string.
 * Returns SQLITE_OK on success.
 */
int crsql_fill_db_version_if_needed(sqlite3 *db, crsql_ExtData *pExtData,
                                    char **errmsg);

/**
 * Return the next db_version to use.
 * This is max(dbVersion + 1, pendingDbVersion, merging_version).
 * pendingDbVersion is updated to the returned value.
 * On error, returns -1 and *errmsg is set to a sqlite3_malloc'd error string.
 */
sqlite_int64 crsql_next_db_version(sqlite3 *db, crsql_ExtData *pExtData,
                                   sqlite3_int64 merging_version,
                                   char **errmsg);

/**
 * Rebuild the UNION query statement used to fetch max db_version across
 * all clock tables. Finalizes the old statement and prepares a new one.
 * Returns SQLITE_OK on success, -1 if there are no clock tables (clean db),
 * or an error code.
 */
int crsql_recreate_db_version_stmt(sqlite3 *db, crsql_ExtData *pExtData);

#endif
