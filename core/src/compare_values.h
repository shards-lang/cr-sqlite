#ifndef CRSQLITE_COMPARE_VALUES_H
#define CRSQLITE_COMPARE_VALUES_H

#include "sqlite3ext.h"

// Compare two SQLite values (returns -1, 0, or 1 like strcmp)
// NULL is treated as less than all other values
int crsql_compare_sqlite_values(sqlite3_value *l, sqlite3_value *r);

// Check if any value in two arrays differs
// Returns 1 if any differ, 0 if all same, -1 on error
int crsql_any_value_changed(sqlite3_value **left, sqlite3_value **right, int count);

#endif
