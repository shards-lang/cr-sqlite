#ifndef CRSQLITE_AUTOMIGRATE_H
#define CRSQLITE_AUTOMIGRATE_H

#include "sqlite3ext.h"

// Automatic schema migration function
// Compares current schema to desired schema and applies changes:
// - Drops removed tables
// - Adds/drops columns
// - Updates indices
// - Handles CRR tables with begin/commit alter
//
// Args:
//   argv[0]: desired schema SQL
//   argv[1]: optional cleanup SQL to run on temp db
void crsql_automigrate(sqlite3_context *ctx, int argc, sqlite3_value **argv);

#endif
