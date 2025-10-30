#ifndef CRSQLITE_STMT_CACHE_H
#define CRSQLITE_STMT_CACHE_H

#include "sqlite3ext.h"

// Reset a cached statement (clear bindings and reset)
int crsql_reset_cached_stmt(sqlite3_stmt *stmt);

#endif
