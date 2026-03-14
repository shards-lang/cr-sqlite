#ifndef CRSQLITE_CL_SET_VTAB_H
#define CRSQLITE_CL_SET_VTAB_H

#include "crsqlite.h"

/**
 * Register the clset virtual table module.
 * Usage: CREATE VIRTUAL TABLE foo_schema USING clset(col1 type, col2 type, ...)
 * Creates a base table 'foo' + CRR infrastructure.
 */
int crsql_create_cl_set_module(sqlite3 *db);

#endif
