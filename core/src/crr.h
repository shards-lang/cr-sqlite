#ifndef CRSQLITE_CRR_H
#define CRSQLITE_CRR_H

#include "crsqlite.h"
#include "ext-data.h"
#include "tableinfo.h"

/**
 * Create a CRR from an existing table: check compatibility, create clock
 * table, create triggers, backfill existing rows.
 */
int crsql_create_crr(sqlite3 *db, const char *schemaName, const char *tblName,
                     int isCommitAlter, int noTx, char **errmsg);

/**
 * Create the __crsql_clock and __crsql_pks tables for the given table.
 */
int crsql_create_clock_table(sqlite3 *db, crsql_TableInfo *tableInfo,
                             char **errmsg);

/**
 * Backfill clock entries for all existing rows in a table.
 */
int crsql_backfill_table(sqlite3 *db, const char *tblName,
                         crsql_ColumnInfo *pkCols, int numPks,
                         crsql_ColumnInfo *nonPkCols, int numNonPks,
                         int isCommitAlter, int noTx);

/**
 * crsql_config_set - set a crsql configuration value.
 * SQLite custom function callback.
 */
void crsql_config_set(sqlite3_context *ctx, int argc, sqlite3_value **argv);

/**
 * crsql_config_get - get a crsql configuration value.
 * SQLite custom function callback.
 */
void crsql_config_get(sqlite3_context *ctx, int argc, sqlite3_value **argv);

#endif
