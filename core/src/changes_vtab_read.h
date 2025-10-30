#ifndef CRSQLITE_CHANGES_VTAB_READ_H
#define CRSQLITE_CHANGES_VTAB_READ_H

#include "sqlite3ext.h"
#include "tableinfo.h"

// Build a UNION ALL query of all table changes
// Returns allocated string that caller must free with sqlite3_free
// idx_str: optional WHERE clause (can be empty string)
char *crsql_changes_union_query(crsql_TableInfo **table_infos, int num_tables,
                                const char *idx_str);

#endif
