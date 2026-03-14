#ifndef CRSQLITE_LOCAL_WRITES_H
#define CRSQLITE_LOCAL_WRITES_H

#include "crsqlite.h"
#include "ext-data.h"
#include "tableinfo.h"

/**
 * AFTER INSERT trigger handler.
 * Signature: crsql_after_insert('table_name', pk_val1, pk_val2, ...)
 */
void x_crsql_after_insert(sqlite3_context *ctx, int argc, sqlite3_value **argv);

/**
 * AFTER UPDATE trigger handler.
 * Signature: crsql_after_update('table_name', new_pk1, ..., old_pk1, ...,
 *                                [new_nonpk1, ..., old_nonpk1, ...])
 */
void x_crsql_after_update(sqlite3_context *ctx, int argc, sqlite3_value **argv);

/**
 * AFTER DELETE trigger handler.
 * Signature: crsql_after_delete('table_name', old_pk1, old_pk2, ...)
 */
void x_crsql_after_delete(sqlite3_context *ctx, int argc, sqlite3_value **argv);

#endif
