#ifndef CRSQLITE_CONFIG_H
#define CRSQLITE_CONFIG_H

#include "sqlite3ext.h"

#define MERGE_EQUAL_VALUES "merge-equal-values"

// SQL functions for config management
void crsql_config_set(sqlite3_context *ctx, int argc, sqlite3_value **argv);
void crsql_config_get(sqlite3_context *ctx, int argc, sqlite3_value **argv);

#endif
