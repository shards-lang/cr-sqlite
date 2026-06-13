#include "changes-vtab.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "consts.h"
#include "crsqlite.h"

int crsql_close(sqlite3 *db);

static void testManyPkTable() {
  printf("ManyPkTable\n");

  sqlite3 *db;
  sqlite3_stmt *pStmt;
  int rc;
  rc = sqlite3_open(":memory:", &db);

  rc = sqlite3_exec(
      db, "CREATE TABLE foo (a not null, b not null, c, primary key (a, b));",
      0, 0, 0);
  rc += sqlite3_exec(db, "SELECT crsql_as_crr('foo');", 0, 0, 0);
  assert(rc == SQLITE_OK);
  rc += sqlite3_exec(db, "INSERT INTO foo VALUES (4,5,6);", 0, 0, 0);
  assert(rc == SQLITE_OK);

  rc += sqlite3_prepare_v2(db, "SELECT [table], quote(pk) FROM crsql_changes",
                           -1, &pStmt, 0);
  assert(rc == SQLITE_OK);

  while (sqlite3_step(pStmt) == SQLITE_ROW) {
    const unsigned char *pk = sqlite3_column_text(pStmt, 1);
    // pk: 4, 5
    // X'0209040905'
    // 02 -> columns
    // 09 -> 1 byte integer
    // 04 -> 4
    // 09 -> 1 byte integer
    // 05 -> 5
    assert(strcmp("X'0209040905'", (char *)pk) == 0);
  }

  sqlite3_finalize(pStmt);
  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void assertCount(sqlite3 *db, const char *sql, int expected) {
  sqlite3_stmt *pStmt;
  int rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  assert(sqlite3_step(pStmt) == SQLITE_ROW);
  printf("expected: %d, actual: %d\n", expected, sqlite3_column_int(pStmt, 0));
  assert(sqlite3_column_int(pStmt, 0) == expected);
  sqlite3_finalize(pStmt);
}

static void testFilters() {
  printf("Filters\n");

  sqlite3 *db;
  int rc;
  rc = sqlite3_open(":memory:", &db);

  rc = sqlite3_exec(db, "CREATE TABLE foo (a primary key not null, b);", 0, 0,
                    0);
  rc += sqlite3_exec(db, "SELECT crsql_as_crr('foo');", 0, 0, 0);
  assert(rc == SQLITE_OK);
  rc += sqlite3_exec(db, "INSERT INTO foo VALUES (1,2);", 0, 0, 0);
  rc += sqlite3_exec(db, "INSERT INTO foo VALUES (2,3);", 0, 0, 0);
  rc += sqlite3_exec(db, "INSERT INTO foo VALUES (3,4);", 0, 0, 0);
  assert(rc == SQLITE_OK);

  printf("no filters\n");
  // 6 - 1 for each row creation, 1 for each b
  assertCount(db, "SELECT count(*) FROM crsql_changes", 3);

  // now test:
  // 1. site_id comparison
  // 2. db_version comparison

  printf("is null\n");
  assertCount(db, "SELECT count(*) FROM crsql_changes WHERE site_id IS NULL",
              0);

  printf("is not null\n");
  assertCount(
      db, "SELECT count(*) FROM crsql_changes WHERE site_id IS NOT NULL", 3);

  printf("equals\n");
  assertCount(
      db, "SELECT count(*) FROM crsql_changes WHERE site_id = crsql_site_id()",
      3);

  // 0 rows is actually correct ANSI sql behavior. NULLs are never equal, or not
  // equal, to anything in ANSI SQL. So users must use `IS NOT` to check rather
  // than !=.
  //
  // https://stackoverflow.com/questions/60017275/why-null-is-not-equal-to-anything-is-a-false-statement
  printf("not equals\n");
  assertCount(
      db, "SELECT count(*) FROM crsql_changes WHERE site_id != crsql_site_id()",
      0);

  printf("is not\n");
  // All rows are currently equal to site_id since all rows are currently local
  // writes
  assertCount(
      db, "SELECT count(*) FROM crsql_changes WHERE site_id IS crsql_site_id()",
      3);

  // compare on db_version _and_ site_id

  // compare upper and lower bound on db_version
  printf("double bounded version\n");
  assertCount(db,
              "SELECT count(*) FROM crsql_changes WHERE db_version >= 1 AND "
              "db_version < 2",
              1);

  printf("OR condition\n");
  assertCount(db,
              "SELECT count(*) FROM crsql_changes WHERE db_version > 2 OR "
              "site_id IS crsql_site_id()",
              3);

  // compare on pks, table name, other not perfectly supported columns

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

// test value extraction under all filter conditions

// static void testSinglePksTable()
// {
// }

/**
 * The merge path memoizes (table, pk) -> (key, causal length) for consecutive
 * changes of the same row. A savepoint rollback undoes merge writes the memo
 * may describe; the changes vtab must invalidate it (xRollbackTo) or stale
 * causal lengths would make subsequent merges no-ops.
 */
// assert() is compiled out by NDEBUG in Release builds; these helpers are
// always active.
static void checkOk(int rc, char *err, const char *what) {
  if (rc != SQLITE_OK) {
    printf("\t\e[0;31m%s failed: %d (%s)\e[0m\n", what, rc,
           err ? err : "no message");
    sqlite3_free(err);
    abort();
  }
  sqlite3_free(err);
}

static void checkCount(sqlite3 *db, const char *sql, int expected) {
  sqlite3_stmt *pStmt;
  int rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, 0);
  if (rc != SQLITE_OK || sqlite3_step(pStmt) != SQLITE_ROW) {
    printf("\t\e[0;31mfailed to run: %s\e[0m\n", sql);
    abort();
  }
  int actual = sqlite3_column_int(pStmt, 0);
  sqlite3_finalize(pStmt);
  if (actual != expected) {
    printf("\t\e[0;31m%s: expected %d, got %d\e[0m\n", sql, expected, actual);
    abort();
  }
}

static void testRowMemoSavepointRollback() {
  printf("RowMemoSavepointRollback\n");

  sqlite3 *db;
  char *err = 0;
  int rc = sqlite3_open(":memory:", &db);
  if (rc != SQLITE_OK) abort();

  rc = sqlite3_exec(
      db, "CREATE TABLE foo (a INTEGER PRIMARY KEY NOT NULL, b);", 0, 0, &err);
  checkOk(rc, err, "create table");
  err = 0;
  rc = sqlite3_exec(db, "SELECT crsql_as_crr('foo');", 0, 0, &err);
  checkOk(rc, err, "as_crr");
  err = 0;

  const char *insertChange =
      "INSERT INTO crsql_changes "
      "([table], pk, cid, val, col_version, db_version, site_id, cl, seq) "
      "VALUES ('foo', X'010901', 'b', 1, 1, 1, X'00000000000000000000000000000001', 1, 0)";
  const char *deleteChange =
      "INSERT INTO crsql_changes "
      "([table], pk, cid, val, col_version, db_version, site_id, cl, seq) "
      "VALUES ('foo', X'010901', '-1', NULL, 2, 2, X'00000000000000000000000000000001', 2, 0)";

  rc = sqlite3_exec(db, "BEGIN", 0, 0, &err);
  checkOk(rc, err, "begin");
  err = 0;
  // Create the row via a merged change -- memoizes (key, cl = 1)
  rc = sqlite3_exec(db, insertChange, 0, 0, &err);
  checkOk(rc, err, "insert change");
  err = 0;
  checkCount(db, "SELECT count(*) FROM foo", 1);

  // Delete it inside a savepoint -- memo cl becomes 2 -- then roll back
  rc = sqlite3_exec(db, "SAVEPOINT sp", 0, 0, &err);
  checkOk(rc, err, "savepoint");
  err = 0;
  rc = sqlite3_exec(db, deleteChange, 0, 0, &err);
  checkOk(rc, err, "delete change");
  err = 0;
  checkCount(db, "SELECT count(*) FROM foo", 0);
  rc = sqlite3_exec(db, "ROLLBACK TO sp", 0, 0, &err);
  checkOk(rc, err, "rollback to");
  err = 0;
  checkCount(db, "SELECT count(*) FROM foo", 1);

  // Re-apply the delete: with a stale memo (cl = 2) this would be treated as
  // already-processed and skipped; it must actually delete the row.
  rc = sqlite3_exec(db, deleteChange, 0, 0, &err);
  checkOk(rc, err, "re-applied delete change");
  err = 0;
  checkCount(db, "SELECT count(*) FROM foo", 0);

  rc = sqlite3_exec(db, "COMMIT", 0, 0, &err);
  checkOk(rc, err, "commit");
  checkCount(db, "SELECT count(*) FROM foo", 0);

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

// static void testOnlyPkTable()
// {
// }

// static void testSciNotation()
// {
// }

// static void testHex()
// {
// }

void crsqlChangesVtabTestSuite() {
  printf("\e[47m\e[1;30mSuite: crsql_changesVtab\e[0m\n");
  testManyPkTable();
  testFilters();
  testRowMemoSavepointRollback();
}
