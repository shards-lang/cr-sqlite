#ifndef CRSQLITE_PACK_COLUMNS_H
#define CRSQLITE_PACK_COLUMNS_H

#include "sqlite3ext.h"

// Column value types for unpacking
typedef enum {
  CRSQL_COLUMN_BLOB = 0,
  CRSQL_COLUMN_INTEGER,
  CRSQL_COLUMN_FLOAT,
  CRSQL_COLUMN_TEXT,
  CRSQL_COLUMN_NULL
} crsql_ColumnValueType;

// Column value structure
typedef struct {
  crsql_ColumnValueType type;
  union {
    struct {
      unsigned char *data;
      int len;
    } blob;
    sqlite3_int64 i64;
    double f64;
    struct {
      char *data;
      int len;
    } text;
  } val;
} crsql_ColumnValue;

// SQLite function to pack columns into a binary blob
void crsql_pack_columns(sqlite3_context *ctx, int argc, sqlite3_value **argv);

// Unpack a binary blob into column values
// Returns array of column values, sets *num_cols to array length
// Caller must free returned array and any blob/text data
crsql_ColumnValue *crsql_unpack_columns(const unsigned char *data, int data_len,
                                        int *num_cols);

// Free unpacked column values
void crsql_free_unpacked_columns(crsql_ColumnValue *values, int num_cols);

// Bind unpacked values to a statement starting at offset
int crsql_bind_package_to_stmt(sqlite3_stmt *stmt, crsql_ColumnValue *values,
                               int num_cols, int offset);

#endif
