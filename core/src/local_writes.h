#ifndef CRSQLITE_LOCAL_WRITES_H
#define CRSQLITE_LOCAL_WRITES_H

#include "sqlite3ext.h"

// Trigger functions called from INSERT/UPDATE/DELETE triggers
// These update clock tables with version and sequence information

// Called after INSERT: crsql_after_insert("table", pk_new_values...)
void crsql_after_insert(sqlite3_context *ctx, int argc, sqlite3_value **argv);

// Called after UPDATE: crsql_after_update("table", pk_new..., pk_old..., col_new..., col_old...)
void crsql_after_update(sqlite3_context *ctx, int argc, sqlite3_value **argv);

// Called after DELETE: crsql_after_delete("table", pk_old_values...)
void crsql_after_delete(sqlite3_context *ctx, int argc, sqlite3_value **argv);

#endif
