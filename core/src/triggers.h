#ifndef CRSQLITE_TRIGGERS_H
#define CRSQLITE_TRIGGERS_H

#include "crsqlite.h"
#include "tableinfo.h"

/**
 * Create AFTER INSERT/UPDATE/DELETE triggers on a table that call
 * crsql_after_insert/update/delete. Triggers are guarded by
 * crsql_internal_sync_bit() = 0 to prevent recursion during merge.
 */
int crsql_create_triggers(sqlite3 *db, crsql_TableInfo *tableInfo,
                          char **errmsg);

/**
 * Drop the CRR triggers (insert, update, delete) for a table.
 */
int crsql_remove_crr_triggers_if_exist(sqlite3 *db, const char *table);

/**
 * Drop the clock and pks tables for a table.
 */
int crsql_remove_crr_clock_table_if_exists(sqlite3 *db, const char *table);

#endif
