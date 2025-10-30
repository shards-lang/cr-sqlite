#ifndef CRSQLITE_UNPACK_COLUMNS_VTAB_H
#define CRSQLITE_UNPACK_COLUMNS_VTAB_H

#include "sqlite3ext.h"

// Create the crsql_unpack_columns virtual table module
// Schema: CREATE TABLE x(cell ANY, package BLOB hidden)
// Usage: SELECT cell FROM crsql_unpack_columns WHERE package = ?
int crsql_create_unpack_columns_module(sqlite3 *db);

#endif
