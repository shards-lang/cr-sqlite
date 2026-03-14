#ifndef CRSQLITE_UTIL_H
#define CRSQLITE_UTIL_H

#include <ctype.h>
#include <stddef.h>

#include "crsqlite.h"
#include "tableinfo.h"

/**
 * Escape double-quotes in an identifier for use in SQL.
 * Returns a sqlite3_malloc'd string. Caller must sqlite3_free.
 */
char *crsql_escape_ident(const char *ident);

/**
 * Escape single-quotes in a value for use in SQL.
 * Returns a sqlite3_malloc'd string. Caller must sqlite3_free.
 */
char *crsql_escape_ident_as_value(const char *ident);

/**
 * Generate a binding list like "?, ?, ?"
 * Returns a sqlite3_malloc'd string. Caller must sqlite3_free.
 */
char *crsql_binding_list(int numSlots);

/**
 * Generate a WHERE clause like "\"a\" IS ? AND \"b\" IS ?"
 * If prefix is non-NULL, it is prepended to each column reference.
 * Returns a sqlite3_malloc'd string. Caller must sqlite3_free.
 */
char *crsql_where_list(crsql_ColumnInfo *cols, int numCols,
                       const char *prefix);

/**
 * Generate a quoted identifier list like "\"a\",\"b\",\"c\""
 * If prefix is non-NULL, it is prepended to each identifier.
 * Returns a sqlite3_malloc'd string. Caller must sqlite3_free.
 */
char *crsql_as_identifier_list(crsql_ColumnInfo *cols, int numCols,
                               const char *prefix);

/**
 * Compute a slab-allocated rowid from a table index and local rowid.
 */
sqlite3_int64 crsql_slab_rowid(int idx, sqlite3_int64 rowid);

/**
 * Generate UNION query to find max db_version across all clock tables.
 * tblNames is an array of numTables strings.
 * Returns a sqlite3_malloc'd string. Caller must sqlite3_free.
 */
char *crsql_get_db_version_union_query(char **tblNames, int numTables);

/**
 * Get the default value for a column from pragma_table_info.
 * Returns SQLITE_OK on success.
 * If column is nullable with no default, *outDflt is set to "NULL".
 * If column is NOT NULL with no default, *outDflt is set to NULL.
 * Otherwise *outDflt is set to the default value string.
 * *outDflt is sqlite3_malloc'd if non-NULL. Caller must sqlite3_free.
 */
int crsql_get_dflt_value(sqlite3 *db, const char *table, const char *col,
                         char **outDflt);

/**
 * Compare two sqlite3_value pointers.
 * Returns negative if l < r, 0 if equal, positive if l > r.
 * NULL is less than all other types.
 */
int crsql_compare_sqlite_values(sqlite3_value *l, sqlite3_value *r);

/**
 * Check if any values differ between two arrays.
 * Returns 1 if any differ, 0 if all equal, -1 on error.
 */
int crsql_any_value_changed(sqlite3_value **left, sqlite3_value **right,
                            int numValues);

#endif
