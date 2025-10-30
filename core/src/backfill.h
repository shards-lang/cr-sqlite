#ifndef CRSQLITE_BACKFILL_H
#define CRSQLITE_BACKFILL_H

#include "sqlite3ext.h"
#include "tableinfo.h"

// Backfill clock table with entries for existing rows
int crsql_backfill_table(sqlite3 *db, const char *table, crsql_ColumnInfo **pks,
                         int pks_len, crsql_ColumnInfo **non_pks, int non_pks_len,
                         int is_commit_alter, int no_tx);

#endif
