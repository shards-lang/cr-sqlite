#ifndef CRSQLITE_PACK_COLUMNS_H
#define CRSQLITE_PACK_COLUMNS_H

#include "crsqlite.h"

typedef enum {
  CRSQL_CV_BLOB = 0,
  CRSQL_CV_INTEGER = 1,
  CRSQL_CV_FLOAT = 2,
  CRSQL_CV_NULL = 3,
  CRSQL_CV_TEXT = 4,
} crsql_ColValType;

typedef struct crsql_ColumnValue crsql_ColumnValue;
struct crsql_ColumnValue {
  crsql_ColValType type;
  union {
    struct {
      unsigned char *data;
      int len;
    } blob;
    sqlite3_int64 integer;
    double floatVal;
    struct {
      char *data;
      int len;
    } text;
  } v;
};

/**
 * SQL function: crsql_pack_columns(val1, val2, ...)
 * Packs column values into a compact binary blob.
 */
void crsql_pack_columns_fn(sqlite3_context *ctx, int argc,
                            sqlite3_value **argv);

/**
 * Pack an array of sqlite3_value into a binary blob.
 * Returns sqlite3_malloc'd blob. Caller must sqlite3_free.
 * Sets *outLen to the length of the returned blob.
 * Returns NULL on error.
 */
unsigned char *crsql_pack_columns(sqlite3_value **values, int numValues,
                                  int *outLen);

/**
 * Unpack a binary blob into an array of ColumnValues.
 * Returns sqlite3_malloc'd array. Caller must free with
 * crsql_free_column_values.
 * Sets *outLen to the number of values.
 * Returns NULL on error.
 */
crsql_ColumnValue *crsql_unpack_columns(const unsigned char *data, int dataLen,
                                        int *outLen);

/**
 * Free an array of ColumnValues returned by crsql_unpack_columns.
 */
void crsql_free_column_values(crsql_ColumnValue *values, int numValues);

/**
 * Bind unpacked column values to a prepared statement.
 * Binds starting at parameter index (offset + 1).
 */
int crsql_bind_package_to_stmt(sqlite3_stmt *pStmt, crsql_ColumnValue *values,
                               int numValues, int offset);

#endif
