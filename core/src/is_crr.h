#ifndef CRSQLITE_IS_CRR_H
#define CRSQLITE_IS_CRR_H

#include "sqlite3ext.h"

// Check if a table has been upgraded to a CRR (checks for trigger existence)
// Returns 1 if CRR, 0 if not, negative on error
int crsql_is_crr(sqlite3 *db, const char *table);

#endif
