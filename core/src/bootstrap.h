#ifndef CRSQLITE_BOOTSTRAP_H
#define CRSQLITE_BOOTSTRAP_H

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT3

/**
 * Initialize the site_id for this database.
 * Creates the site_id table if it does not exist, generates a UUID v4,
 * and writes it into ret (which must be at least SITE_ID_LEN bytes).
 * Returns SQLITE_OK on success.
 */
int crsql_init_site_id(sqlite3 *db, unsigned char *ret);

/**
 * Create the peer tracking table if it does not exist.
 * Returns SQLITE_OK on success.
 */
int crsql_init_peer_tracking_table(sqlite3 *db);

/**
 * Check schema version and run any needed migrations.
 * On error, *errmsg is set to a sqlite3_malloc'd error string.
 * Returns SQLITE_OK on success.
 */
int crsql_maybe_update_db(sqlite3 *db, char **errmsg);

/**
 * Create the crsql_master schema table if it doesn't exist.
 * Returns SQLITE_OK on success.
 */
int crsql_create_schema_table_if_not_exists(sqlite3 *db);

/**
 * Returns 1 if the given table has already been upgraded to a CRR
 * (by checking for the existence of its insert trigger), 0 if not,
 * or -1 on error.
 */
int crsql_is_crr(sqlite3 *db, const char *table);

#endif
