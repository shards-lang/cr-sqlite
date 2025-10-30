#ifndef CRSQLITE_TEARDOWN_H
#define CRSQLITE_TEARDOWN_H

#include "sqlite3ext.h"

// Remove CRR clock tables
int crsql_remove_crr_clock_table_if_exists(sqlite3 *db, const char *table);

// Remove CRR triggers
int crsql_remove_crr_triggers_if_exist(sqlite3 *db, const char *table);

#endif
