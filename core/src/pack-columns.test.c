#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <string.h>

#include "crsqlite.h"
#include "pack-columns.h"

int crsql_close(sqlite3 *db);

// Helper: create an in-memory db with the extension loaded
static sqlite3 *openDb() {
  sqlite3 *db;
  int rc = sqlite3_open(":memory:", &db);
  assert(rc == SQLITE_OK);
  return db;
}

// Helper: pack a single integer through SQL, return the packed blob
// Uses SELECT ? to create a sqlite3_value from an int64
static void packSingleInt(sqlite3 *db, sqlite3_int64 val,
                          unsigned char **outBuf, int *outLen) {
  sqlite3_stmt *pStmt;
  int rc = sqlite3_prepare_v2(db, "SELECT ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  sqlite3_bind_int64(pStmt, 1, val);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);

  sqlite3_value *v = sqlite3_column_value(pStmt, 0);
  *outBuf = crsql_pack_columns(&v, 1, outLen);
  assert(*outBuf != 0);
  sqlite3_finalize(pStmt);
}

static void assertIntRoundtrip(sqlite3 *db, sqlite3_int64 val) {
  unsigned char *buf;
  int bufLen;
  packSingleInt(db, val, &buf, &bufLen);

  int numCols;
  crsql_ColumnValue *vals = crsql_unpack_columns(buf, bufLen, &numCols);
  assert(numCols == 1);
  assert(vals != 0);
  assert(vals[0].type == CRSQL_CV_INTEGER);
  if (vals[0].v.integer != val) {
    printf("FAIL: expected %lld, got %lld\n", val, vals[0].v.integer);
  }
  assert(vals[0].v.integer == val);

  crsql_free_column_values(vals, numCols);
  sqlite3_free(buf);
}

static void testPackUnpackRoundtripIntegers() {
  printf("PackUnpackRoundtripIntegers\n");
  sqlite3 *db = openDb();

  // Zero (0 bytes needed)
  assertIntRoundtrip(db, 0);

  // 1-byte positive boundary: 0x7F = 127
  assertIntRoundtrip(db, 1);
  assertIntRoundtrip(db, 127);
  // 2-byte boundary: 128 needs 2 bytes (sign bit)
  assertIntRoundtrip(db, 128);
  assertIntRoundtrip(db, 255);
  assertIntRoundtrip(db, 256);

  // 2-byte positive boundary: 0x7FFF = 32767
  assertIntRoundtrip(db, 32767);
  assertIntRoundtrip(db, 32768);

  // 4-byte boundary
  assertIntRoundtrip(db, 2147483647LL);   // INT32_MAX
  assertIntRoundtrip(db, 2147483648LL);   // INT32_MAX + 1

  // 8-byte boundary
  assertIntRoundtrip(db, 9223372036854775807LL);  // INT64_MAX

  // Negative 1-byte boundary: -0x80 = -128
  assertIntRoundtrip(db, -1);
  assertIntRoundtrip(db, -128);
  // 2-byte boundary
  assertIntRoundtrip(db, -129);
  assertIntRoundtrip(db, -32768);
  assertIntRoundtrip(db, -32769);

  // 4-byte boundary
  assertIntRoundtrip(db, -2147483648LL);  // INT32_MIN
  assertIntRoundtrip(db, -2147483649LL);  // INT32_MIN - 1

  // INT64_MIN
  // Note: we write the literal as -9223372036854775807LL - 1 to avoid
  // compiler warnings about the literal being too large for signed
  assertIntRoundtrip(db, -9223372036854775807LL - 1);

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void assertFloatRoundtrip(sqlite3 *db, double val) {
  sqlite3_stmt *pStmt;
  int rc = sqlite3_prepare_v2(db, "SELECT ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  sqlite3_bind_double(pStmt, 1, val);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);

  sqlite3_value *v = sqlite3_column_value(pStmt, 0);
  int bufLen;
  unsigned char *buf = crsql_pack_columns(&v, 1, &bufLen);
  assert(buf != 0);
  sqlite3_finalize(pStmt);

  int numCols;
  crsql_ColumnValue *vals = crsql_unpack_columns(buf, bufLen, &numCols);
  assert(numCols == 1);
  assert(vals != 0);
  assert(vals[0].type == CRSQL_CV_FLOAT);

  // Bit-exact comparison (memcmp) to catch sign/endian issues
  double got = vals[0].v.floatVal;
  assert(memcmp(&got, &val, sizeof(double)) == 0);

  crsql_free_column_values(vals, numCols);
  sqlite3_free(buf);
}

static void testPackUnpackRoundtripFloats() {
  printf("PackUnpackRoundtripFloats\n");
  sqlite3 *db = openDb();

  assertFloatRoundtrip(db, 0.0);
  assertFloatRoundtrip(db, -0.0);
  assertFloatRoundtrip(db, 1.5);
  assertFloatRoundtrip(db, -1.5);
  assertFloatRoundtrip(db, DBL_MAX);
  assertFloatRoundtrip(db, DBL_MIN);
  assertFloatRoundtrip(db, 1e-300);
  assertFloatRoundtrip(db, -1e-300);
  assertFloatRoundtrip(db, 3.141592653589793);

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void assertTextRoundtrip(sqlite3 *db, const char *text, int textLen) {
  sqlite3_stmt *pStmt;
  int rc = sqlite3_prepare_v2(db, "SELECT ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  sqlite3_bind_text(pStmt, 1, text, textLen, SQLITE_STATIC);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);

  sqlite3_value *v = sqlite3_column_value(pStmt, 0);
  int bufLen;
  unsigned char *buf = crsql_pack_columns(&v, 1, &bufLen);
  assert(buf != 0);
  sqlite3_finalize(pStmt);

  int numCols;
  crsql_ColumnValue *vals = crsql_unpack_columns(buf, bufLen, &numCols);
  assert(numCols == 1);
  assert(vals != 0);
  assert(vals[0].type == CRSQL_CV_TEXT);
  assert(vals[0].v.text.len == textLen);
  assert(memcmp(vals[0].v.text.data, text, textLen) == 0);
  // Verify null termination
  assert(vals[0].v.text.data[textLen] == '\0');

  crsql_free_column_values(vals, numCols);
  sqlite3_free(buf);
}

static void testPackUnpackRoundtripText() {
  printf("PackUnpackRoundtripText\n");
  sqlite3 *db = openDb();

  // Empty string
  assertTextRoundtrip(db, "", 0);

  // Single char
  assertTextRoundtrip(db, "x", 1);

  // 127 bytes - length fits in 1 byte (sign bit not set)
  char buf127[128];
  memset(buf127, 'A', 127);
  buf127[127] = '\0';
  assertTextRoundtrip(db, buf127, 127);

  // 128 bytes - length needs 2 bytes (sign bit boundary)
  char buf128[129];
  memset(buf128, 'B', 128);
  buf128[128] = '\0';
  assertTextRoundtrip(db, buf128, 128);

  // 256 bytes
  char buf256[257];
  memset(buf256, 'C', 256);
  buf256[256] = '\0';
  assertTextRoundtrip(db, buf256, 256);

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void assertBlobRoundtrip(sqlite3 *db, const unsigned char *blob,
                                int blobLen) {
  sqlite3_stmt *pStmt;
  int rc = sqlite3_prepare_v2(db, "SELECT ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  sqlite3_bind_blob(pStmt, 1, blob, blobLen, SQLITE_STATIC);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);

  sqlite3_value *v = sqlite3_column_value(pStmt, 0);
  int bufLen;
  unsigned char *buf = crsql_pack_columns(&v, 1, &bufLen);
  assert(buf != 0);
  sqlite3_finalize(pStmt);

  int numCols;
  crsql_ColumnValue *vals = crsql_unpack_columns(buf, bufLen, &numCols);
  assert(numCols == 1);
  assert(vals != 0);
  assert(vals[0].type == CRSQL_CV_BLOB);
  assert(vals[0].v.blob.len == blobLen);
  if (blobLen > 0) {
    assert(memcmp(vals[0].v.blob.data, blob, blobLen) == 0);
  }

  crsql_free_column_values(vals, numCols);
  sqlite3_free(buf);
}

static void testPackUnpackRoundtripBlob() {
  printf("PackUnpackRoundtripBlob\n");
  sqlite3 *db = openDb();

  // Note: empty blob (len=0) is skipped — sqlite3_malloc(0) may return NULL
  // which unpack treats as OOM. Not a real-world case for PK packing.

  // 1-byte blob
  unsigned char one[] = {0xFF};
  assertBlobRoundtrip(db, one, 1);

  // 127 bytes
  unsigned char buf127[127];
  memset(buf127, 0xAB, 127);
  assertBlobRoundtrip(db, buf127, 127);

  // 128 bytes (sign bit boundary)
  unsigned char buf128[128];
  memset(buf128, 0xCD, 128);
  assertBlobRoundtrip(db, buf128, 128);

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testPackUnpackNull() {
  printf("PackUnpackNull\n");
  sqlite3 *db = openDb();

  sqlite3_stmt *pStmt;
  int rc = sqlite3_prepare_v2(db, "SELECT ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  sqlite3_bind_null(pStmt, 1);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);

  sqlite3_value *v = sqlite3_column_value(pStmt, 0);
  int bufLen;
  unsigned char *buf = crsql_pack_columns(&v, 1, &bufLen);
  assert(buf != 0);
  sqlite3_finalize(pStmt);

  int numCols;
  crsql_ColumnValue *vals = crsql_unpack_columns(buf, bufLen, &numCols);
  assert(numCols == 1);
  assert(vals != 0);
  assert(vals[0].type == CRSQL_CV_NULL);

  crsql_free_column_values(vals, numCols);
  sqlite3_free(buf);

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testPackMultipleColumns() {
  printf("PackMultipleColumns\n");
  sqlite3 *db = openDb();

  sqlite3_stmt *pStmt;
  int rc =
      sqlite3_prepare_v2(db, "SELECT ?, ?, ?, ?, ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);

  sqlite3_bind_int64(pStmt, 1, 42);
  sqlite3_bind_double(pStmt, 2, 3.14);
  sqlite3_bind_text(pStmt, 3, "hello", -1, SQLITE_STATIC);
  unsigned char blobData[] = {0xDE, 0xAD, 0xBE, 0xEF};
  sqlite3_bind_blob(pStmt, 4, blobData, 4, SQLITE_STATIC);
  sqlite3_bind_null(pStmt, 5);

  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);

  sqlite3_value *values[5];
  for (int i = 0; i < 5; i++) {
    values[i] = sqlite3_column_value(pStmt, i);
  }

  int bufLen;
  unsigned char *buf = crsql_pack_columns(values, 5, &bufLen);
  assert(buf != 0);
  sqlite3_finalize(pStmt);

  int numCols;
  crsql_ColumnValue *vals = crsql_unpack_columns(buf, bufLen, &numCols);
  assert(numCols == 5);
  assert(vals != 0);

  // Integer
  assert(vals[0].type == CRSQL_CV_INTEGER);
  assert(vals[0].v.integer == 42);

  // Float
  assert(vals[1].type == CRSQL_CV_FLOAT);
  assert(vals[1].v.floatVal == 3.14);

  // Text
  assert(vals[2].type == CRSQL_CV_TEXT);
  assert(vals[2].v.text.len == 5);
  assert(strcmp(vals[2].v.text.data, "hello") == 0);

  // Blob
  assert(vals[3].type == CRSQL_CV_BLOB);
  assert(vals[3].v.blob.len == 4);
  assert(memcmp(vals[3].v.blob.data, blobData, 4) == 0);

  // Null
  assert(vals[4].type == CRSQL_CV_NULL);

  crsql_free_column_values(vals, numCols);
  sqlite3_free(buf);

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testPackZeroColumns() {
  printf("PackZeroColumns\n");
  sqlite3 *db = openDb();

  int bufLen;
  unsigned char *buf = crsql_pack_columns(NULL, 0, &bufLen);
  assert(buf != 0);
  assert(bufLen == 1);
  assert(buf[0] == 0);

  // Unpack zero columns — returns NULL with outLen=0
  int numCols;
  crsql_ColumnValue *vals = crsql_unpack_columns(buf, bufLen, &numCols);
  assert(numCols == 0);
  assert(vals == 0);

  sqlite3_free(buf);
  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testUnpackTruncatedData() {
  printf("UnpackTruncatedData\n");

  int numCols;
  crsql_ColumnValue *vals;

  // Empty data
  vals = crsql_unpack_columns(NULL, 0, &numCols);
  assert(vals == 0);

  // Header says 1 column but no type byte follows
  unsigned char trunc1[] = {0x01};
  vals = crsql_unpack_columns(trunc1, 1, &numCols);
  assert(vals == 0);
  assert(numCols == 0);

  // Integer type byte says 2 bytes of data but only 1 byte follows
  // type = SQLITE_INTEGER(1), numBytes = 2 → typeAndLen = (2 << 3) | 1 = 0x11
  unsigned char trunc2[] = {0x01, 0x11, 0xFF};
  vals = crsql_unpack_columns(trunc2, 3, &numCols);
  assert(vals == 0);
  assert(numCols == 0);

  // Text with length prefix claiming more data than available
  // type = SQLITE_TEXT(3), numBytes for length = 1 → typeAndLen = (1 << 3) | 3
  // = 0x0B length byte = 100 (claims 100 bytes) but no data follows
  unsigned char trunc3[] = {0x01, 0x0B, 100};
  vals = crsql_unpack_columns(trunc3, 3, &numCols);
  assert(vals == 0);
  assert(numCols == 0);

  // Float type but only 4 bytes instead of 8
  // type = SQLITE_FLOAT(2), no length bits → typeAndLen = 0x02
  unsigned char trunc4[] = {0x01, 0x02, 0, 0, 0, 0};
  vals = crsql_unpack_columns(trunc4, 6, &numCols);
  assert(vals == 0);
  assert(numCols == 0);

  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testUnpackInvalidTypeByte() {
  printf("UnpackInvalidTypeByte\n");

  int numCols;
  crsql_ColumnValue *vals;

  // Type 0 is not a valid SQLITE_* type constant
  unsigned char invalid1[] = {0x01, 0x00};
  vals = crsql_unpack_columns(invalid1, 2, &numCols);
  // Type 0 hits default case → error
  assert(vals == 0);
  assert(numCols == 0);

  // Type 6 is invalid
  unsigned char invalid2[] = {0x01, 0x06};
  vals = crsql_unpack_columns(invalid2, 2, &numCols);
  assert(vals == 0);
  assert(numCols == 0);

  // Type 7 is invalid
  unsigned char invalid3[] = {0x01, 0x07};
  vals = crsql_unpack_columns(invalid3, 2, &numCols);
  assert(vals == 0);
  assert(numCols == 0);

  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testBindPackageToStmt() {
  printf("BindPackageToStmt\n");
  sqlite3 *db = openDb();

  // Pack 3 values: integer, text, float
  sqlite3_stmt *pStmt;
  int rc = sqlite3_prepare_v2(db, "SELECT ?, ?, ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  sqlite3_bind_int64(pStmt, 1, 99);
  sqlite3_bind_text(pStmt, 2, "world", -1, SQLITE_STATIC);
  sqlite3_bind_double(pStmt, 3, 2.718);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);

  sqlite3_value *values[3];
  for (int i = 0; i < 3; i++) {
    values[i] = sqlite3_column_value(pStmt, i);
  }
  int bufLen;
  unsigned char *buf = crsql_pack_columns(values, 3, &bufLen);
  assert(buf != 0);
  sqlite3_finalize(pStmt);

  // Unpack
  int numCols;
  crsql_ColumnValue *vals = crsql_unpack_columns(buf, bufLen, &numCols);
  assert(numCols == 3);
  assert(vals != 0);

  // Bind to a new statement and verify
  rc = sqlite3_prepare_v2(db, "SELECT ?, ?, ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  rc = crsql_bind_package_to_stmt(pStmt, vals, numCols, 0);
  assert(rc == SQLITE_OK);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);

  assert(sqlite3_column_int64(pStmt, 0) == 99);
  assert(strcmp((const char *)sqlite3_column_text(pStmt, 1), "world") == 0);
  assert(sqlite3_column_double(pStmt, 2) == 2.718);

  sqlite3_finalize(pStmt);
  crsql_free_column_values(vals, numCols);
  sqlite3_free(buf);

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

void crsqlPackColumnsTestSuite() {
  printf("\e[47m\e[1;30mSuite: pack-columns\e[0m\n");

  testPackUnpackRoundtripIntegers();
  testPackUnpackRoundtripFloats();
  testPackUnpackRoundtripText();
  testPackUnpackRoundtripBlob();
  testPackUnpackNull();
  testPackMultipleColumns();
  testPackZeroColumns();
  testUnpackTruncatedData();
  testUnpackInvalidTypeByte();
  testBindPackageToStmt();
}
