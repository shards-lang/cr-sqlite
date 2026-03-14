#ifndef CRSQLITE_UNPACK_COLUMNS_VTAB_H
#define CRSQLITE_UNPACK_COLUMNS_VTAB_H

#include "crsqlite.h"

/**
 * Register the crsql_unpack_columns virtual table module.
 * Usage: SELECT cell FROM crsql_unpack_columns WHERE package = ?
 * Decomposes a packed column blob into individual rows.
 */
int crsql_create_unpack_columns_module(sqlite3 *db);

#endif
