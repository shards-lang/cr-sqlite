#ifndef CRSQLITE_STR_UTIL_H
#define CRSQLITE_STR_UTIL_H

#include "sqlite3ext.h"
#include "tableinfo.h"

// Escape double quotes in SQL identifiers (foo"bar -> foo""bar)
char *crsql_escape_ident(const char *ident);

// Escape single quotes in SQL string values (foo'bar -> foo''bar)
char *crsql_escape_ident_as_value(const char *ident);

// Build comma-separated identifier list: "col1", "col2" or prefix."col1", prefix."col2"
// Caller must sqlite3_free the result
char *crsql_as_identifier_list(crsql_ColumnInfo **columns, int columns_len, const char *prefix);

// Build WHERE clause: "col1" IS ? AND "col2" IS ? (with optional prefix)
char *crsql_where_list(crsql_ColumnInfo **columns, int columns_len, const char *prefix);

// Build binding list: "?, ?, ?" for num_slots parameters
char *crsql_binding_list(int num_slots);

// Get default value for a column from pragma_table_info
// Returns NULL if no default, "NULL" if nullable, or the default value string
char *crsql_get_dflt_value(sqlite3 *db, const char *table, const char *col);

// Build UNION query to get max db_version from all clock tables
// table_names is a NULL-terminated array of table names
char *crsql_get_db_version_union_query(char **table_names, int num_tables);

// Calculate slab rowid: (idx * ROWID_SLAB_SIZE) + (rowid % ROWID_SLAB_SIZE)
sqlite3_int64 crsql_slab_rowid(int idx, sqlite3_int64 rowid);

#endif
