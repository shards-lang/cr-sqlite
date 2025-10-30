#ifndef CRSQLITE_CREATE_CRR_H
#define CRSQLITE_CREATE_CRR_H

#include "sqlite3ext.h"

// Create a CRR (conflict-free replicated relation) from a table
// Creates clock tables, triggers, and backfills data
int crsql_create_crr(sqlite3 *db, const char *schema, const char *table,
                     int is_commit_alter, int no_tx, char **err);

#endif
