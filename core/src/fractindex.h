#ifndef CRSQLITE_FRACTINDEX_H
#define CRSQLITE_FRACTINDEX_H

#include "crsqlite.h"

void crsql_fract_key_between_fn(sqlite3_context *ctx, int argc,
                                sqlite3_value **argv);
void crsql_fract_as_ordered_fn(sqlite3_context *ctx, int argc,
                               sqlite3_value **argv);
void crsql_fract_fix_conflict_return_old_key_fn(sqlite3_context *ctx, int argc,
                                                sqlite3_value **argv);
int crsql_init_fractional_index(sqlite3 *db);

#endif
