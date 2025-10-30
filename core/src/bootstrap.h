#ifndef CRSQLITE_BOOTSTRAP_H
#define CRSQLITE_BOOTSTRAP_H

#include "sqlite3ext.h"
#include "tableinfo.h"

// Generate a UUID v4 for site_id
void crsql_gen_uuid(unsigned char *blob);

// Initialize or load site_id for this database
int crsql_init_site_id(sqlite3 *db, unsigned char *ret);

// Create the peer tracking table
int crsql_init_peer_tracking_table(sqlite3 *db);

// Create the schema/master table
int crsql_create_schema_table_if_not_exists(sqlite3 *db);

// Check if database needs migration and perform it
int crsql_maybe_update_db(sqlite3 *db, char **err_msg);

// Create clock tables for a CRR table
int crsql_create_clock_table(sqlite3 *db, crsql_TableInfo *table_info, char **err);

#endif
