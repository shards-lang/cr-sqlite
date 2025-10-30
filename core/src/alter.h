#ifndef CRSQLITE_ALTER_H
#define CRSQLITE_ALTER_H

#include "sqlite3ext.h"
#include "ext-data.h"

// Compact clock tables after schema alterations
// Handles:
// - Primary key changes (drops and recreates clock tables)
// - Column removals (deletes obsolete clock entries)
// - Row deletions (removes orphaned clock entries, preserves tombstones)
int crsql_compact_post_alter(sqlite3 *db, const char *tbl_name,
                             crsql_ExtData *ext_data, char **errmsg);

#endif
