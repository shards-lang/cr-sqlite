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

#endif
