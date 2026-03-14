#ifndef CRSQLITE_AUTOMIGRATE_H
#define CRSQLITE_AUTOMIGRATE_H

#include "crsqlite.h"
#include "ext-data.h"

/**
 * SQL function: crsql_automigrate(schema_text [, cleanup_stmt])
 * Automatically migrates the database schema to match the provided schema.
 */
void crsql_automigrate(sqlite3_context *ctx, int argc, sqlite3_value **argv);

/**
 * Compacts clock tables after a schema alteration.
 * Handles PK changes (drop/recreate) and column removal (prune orphans).
 */
int crsql_compact_post_alter(sqlite3 *db, const char *tblName,
                             crsql_ExtData *pExtData, char **errmsg);

#endif
