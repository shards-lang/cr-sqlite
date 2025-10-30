#ifndef CRSQLITE_CREATE_CL_SET_VTAB_H
#define CRSQLITE_CREATE_CL_SET_VTAB_H

#include "sqlite3ext.h"

// Create the clset virtual table module
// Used to create causal length set backed tables
// Virtual table name must end with "_schema"
// Creates base table and automatically converts to CRR
int crsql_create_cl_set_module(sqlite3 *db);

#endif
