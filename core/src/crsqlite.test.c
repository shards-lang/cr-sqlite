#include "crsqlite.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "consts.h"
#include "bootstrap.h"
#include "crr.h"
#include "db-version.h"
#include "tableinfo.h"

#ifndef CHECK_OK
#define CHECK_OK         \
  if (rc != SQLITE_OK) { \
    goto fail;           \
  }
#endif

#define CHANGES_SINCE_VTAB_TBL 0
#define CHANGES_SINCE_VTAB_PK 1
#define CHANGES_SINCE_VTAB_CID 2
#define CHANGES_SINCE_VTAB_CVAL 3
#define CHANGES_SINCE_VTAB_COL_VRSN 4
#define CHANGES_SINCE_VTAB_DB_VRSN 5
#define CHANGES_SINCE_VTAB_SITE_ID 6
#define CHANGES_SINCE_VTAB_SEQ 7

int crsql_close(sqlite3 *db);

/**
 * @brief selects * from db1 changes where v > since and site_id is not
 * db2_site_id then inserts those changes into db2
 *
 * @param db1
 * @param db2
 * @param since
 * @return int
 */
int syncLeftToRight(sqlite3 *db1, sqlite3 *db2, sqlite3_int64 since) {
  sqlite3_stmt *pStmtRead = 0;
  sqlite3_stmt *pStmtWrite = 0;
  sqlite3_stmt *pStmt = 0;
  int rc = SQLITE_OK;

  rc += sqlite3_prepare_v2(db2, "SELECT crsql_site_id()", -1, &pStmt, 0);
  if (sqlite3_step(pStmt) != SQLITE_ROW) {
    sqlite3_finalize(pStmt);
    return SQLITE_ERROR;
  }

  char *zSql = sqlite3_mprintf(
      "SELECT * FROM crsql_changes WHERE db_version > %lld AND site_id IS NOT "
      "?",
      since);
  rc += sqlite3_prepare_v2(db1, zSql, -1, &pStmtRead, 0);
  assert(rc == SQLITE_OK);
  sqlite3_free(zSql);
  rc += sqlite3_bind_value(pStmtRead, 1, sqlite3_column_value(pStmt, 0));
  assert(rc == SQLITE_OK);
  rc += sqlite3_prepare_v2(
      db2, "INSERT INTO crsql_changes VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", -1,
      &pStmtWrite, 0);
  assert(rc == SQLITE_OK);
  // printf("err: %s\n", err);

  while (sqlite3_step(pStmtRead) == SQLITE_ROW) {
    for (int i = 0; i < 9; ++i) {
      assert(sqlite3_bind_value(pStmtWrite, i + 1,
                                sqlite3_column_value(pStmtRead, i)) ==
             SQLITE_OK);
    }
    assert(sqlite3_step(pStmtWrite) == SQLITE_DONE);
    sqlite3_reset(pStmtWrite);
  }

  sqlite3_finalize(pStmtWrite);
  sqlite3_finalize(pStmtRead);
  sqlite3_finalize(pStmt);

  return SQLITE_OK;
}

static char *crsql_strdup(const char *s) {
  size_t l = strlen(s);
  char *d = sqlite3_malloc(l + 1);
  if (!d) return NULL;
  return memcpy(d, s, l + 1);
}

static char *getQuotedSiteId(sqlite3 *db) {
  sqlite3_stmt *pStmt = 0;
  int rc = SQLITE_OK;

  rc += sqlite3_prepare_v2(db, "SELECT quote(crsql_site_id())", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  if (sqlite3_step(pStmt) != SQLITE_ROW) {
    sqlite3_finalize(pStmt);
    return 0;
  }

  char *ret = crsql_strdup((const char *)sqlite3_column_text(pStmt, 0));
  sqlite3_finalize(pStmt);
  return ret;
}

static int createSimpleSchema(sqlite3 *db, char **err) {
  int rc = SQLITE_OK;
  rc += sqlite3_exec(db, "create table foo (a primary key not null, b);", 0, 0,
                     err);
  rc += sqlite3_exec(db, "select crsql_as_crr('foo');", 0, 0, err);

  return rc;
}

static int columnsAreSame(sqlite3_stmt *pStmt1, sqlite3_stmt *pStmt2, int c) {
  int type1 = sqlite3_column_type(pStmt1, c);
  int type2 = sqlite3_column_type(pStmt2, c);
  int len1 = 0;
  int len2 = 0;

  if (type1 != type2) {
    return 0;
  }

  switch (type1) {
    case SQLITE_NULL:
      return 1;
    case SQLITE_INTEGER:
      return sqlite3_column_int64(pStmt1, c) == sqlite3_column_int64(pStmt2, c);
    case SQLITE_FLOAT:
      return sqlite3_column_double(pStmt1, c) ==
             sqlite3_column_double(pStmt2, c);
    case SQLITE_BLOB:
      len1 = sqlite3_column_bytes(pStmt1, c);
      len2 = sqlite3_column_bytes(pStmt2, c);
      if (len1 != len2) {
        return 0;
      }
      return memcmp(sqlite3_column_blob(pStmt1, c),
                    sqlite3_column_blob(pStmt2, c), len1) == 0;
    case SQLITE_TEXT:
      return strcmp((const char *)sqlite3_column_text(pStmt1, c),
                    (const char *)sqlite3_column_text(pStmt2, c)) == 0;
  }

  // should be unreachable
  assert(0);
  return 0;
}

static int stmtsReturnSameResults(sqlite3_stmt *pStmt1, sqlite3_stmt *pStmt2) {
  int rc1 = SQLITE_OK;
  int rc2 = SQLITE_OK;
  while (sqlite3_step(pStmt1) == SQLITE_ROW) {
    rc2 = sqlite3_step(pStmt2);

    int columns = sqlite3_column_count(pStmt1);
    for (int c = 0; c < columns; ++c) {
      if (columnsAreSame(pStmt1, pStmt2, c) == 0) {
        return 0;
      }
    }
  }

  if (rc1 == SQLITE_DONE && rc2 != SQLITE_DONE) {
    rc2 = sqlite3_step(pStmt2);
    if (rc2 != SQLITE_DONE) {
      return 0;
    }
  }

  return 1;
}

// TODO: add many more cases here.
// 1. Many pk tables
// 2. Only pk tables
// 3. blobs, floats, text, bools, sci notation
// 4. deletes
// 5. pk value changes
static void teste2e() {
  printf("e2e\n");

  int rc = SQLITE_OK;
  sqlite3 *db1;
  sqlite3 *db2;
  sqlite3 *db3;
  sqlite3_stmt *pStmt1;
  sqlite3_stmt *pStmt2;
  sqlite3_stmt *pStmt3;
  char *err = 0;
  char *db1siteid;
  char *db2siteid;
  char *db3siteid;
  rc += sqlite3_open(":memory:", &db1);
  rc += sqlite3_open(":memory:", &db2);
  rc += sqlite3_open(":memory:", &db3);

  rc += createSimpleSchema(db1, &err);
  rc += createSimpleSchema(db2, &err);
  rc += createSimpleSchema(db3, &err);

  db1siteid = getQuotedSiteId(db1);
  db2siteid = getQuotedSiteId(db2);
  db3siteid = getQuotedSiteId(db3);

  rc += sqlite3_exec(db1, "insert into foo values (1, 2.0e2);", 0, 0, &err);
  rc += sqlite3_exec(db1, "insert into foo values (2, X'1232');", 0, 0, &err);
  assert(rc == SQLITE_OK);

  syncLeftToRight(db1, db2, 0);

  rc += sqlite3_prepare_v2(db1, "SELECT * FROM foo ORDER BY a ASC", -1, &pStmt1,
                           0);
  rc += sqlite3_prepare_v2(db2, "SELECT * FROM foo ORDER BY a ASC", -1, &pStmt2,
                           0);
  assert(rc == SQLITE_OK);

  assert(stmtsReturnSameResults(pStmt1, pStmt2) == 1);
  sqlite3_finalize(pStmt1);
  sqlite3_finalize(pStmt2);

  syncLeftToRight(db2, db3, 0);
  rc += sqlite3_prepare_v2(
      db3, "SELECT quote(site_id) FROM crsql_changes ORDER BY pk ASC", -1,
      &pStmt3, 0);
  assert(rc == SQLITE_OK);
  // now compare site ids are what we expect
  rc = sqlite3_step(pStmt3);
  assert(rc == SQLITE_ROW);

  const char *tmpSiteid = (const char *)sqlite3_column_text(pStmt3, 0);
  // printf("db1sid: %s\n", db1siteid);
  // printf("db2sid: %s\n", db2siteid);
  // printf("db3sid: %s\n", db3siteid);
  // printf("tempsid: %s\n", tmpSiteid);
  assert(strcmp(tmpSiteid, db1siteid) == 0);

  rc = sqlite3_step(pStmt3);
  assert(rc == SQLITE_ROW);
  assert(strcmp((const char *)sqlite3_column_text(pStmt3, 0), db1siteid) == 0);
  sqlite3_finalize(pStmt3);

  rc = sqlite3_prepare_v2(db2, "SELECT * FROM foo ORDER BY a ASC", -1, &pStmt2,
                          0);
  rc += sqlite3_prepare_v2(db3, "SELECT * FROM foo ORDER BY a ASC", -1, &pStmt3,
                           0);
  assert(rc == SQLITE_OK);
  assert(stmtsReturnSameResults(pStmt2, pStmt3) == 1);
  sqlite3_finalize(pStmt2);
  sqlite3_finalize(pStmt3);

  // now modify 3 and sync back from 2 to 1
  rc += sqlite3_exec(db3, "insert into foo values (3, 'str');", 0, 0, &err);
  syncLeftToRight(db3, db2, 0);
  syncLeftToRight(db2, db1, 0);

  rc = sqlite3_prepare_v2(db1, "SELECT * FROM foo ORDER BY a ASC", -1, &pStmt1,
                          0);
  rc += sqlite3_prepare_v2(db3, "SELECT * FROM foo ORDER BY a ASC", -1, &pStmt3,
                           0);
  assert(rc == SQLITE_OK);
  assert(stmtsReturnSameResults(pStmt1, pStmt3) == 1);
  sqlite3_finalize(pStmt1);
  sqlite3_finalize(pStmt3);

  // test modification cases -- although these are handled under
  // `testLamportCondition`

  crsql_close(db1);
  crsql_close(db2);
  crsql_close(db3);
  sqlite3_free(db1siteid);
  sqlite3_free(db2siteid);
  sqlite3_free(db3siteid);
  printf("\t\e[0;32mSuccess\e[0m\n");
  return;
}

static void testSelectChangesAfterChangingColumnName() {
  printf("SelectAfterChangingColumnName\n");

  int rc = SQLITE_OK;
  char *err = 0;
  sqlite3 *db;
  sqlite3_stmt *pStmt = 0;
  rc = sqlite3_open(":memory:", &db);

  rc +=
      sqlite3_exec(db, "CREATE TABLE foo(a primary key not null, b);", 0, 0, 0);
  rc += sqlite3_exec(db, "SELECT crsql_as_crr('foo')", 0, 0, 0);
  assert(rc == SQLITE_OK);

  // insert some rows so we have changes
  rc += sqlite3_exec(db, "INSERT INTO foo VALUES (1, 2);", 0, 0, 0);
  assert(rc == SQLITE_OK);

  rc = sqlite3_exec(db, "SELECT crsql_begin_alter('foo')", 0, 0, &err);
  rc += sqlite3_exec(db, "ALTER TABLE foo DROP COLUMN b", 0, 0, 0);
  rc += sqlite3_exec(db, "ALTER TABLE foo ADD COLUMN c", 0, 0, 0);
  rc += sqlite3_exec(db, "SELECT crsql_commit_alter('foo')", 0, 0, 0);
  assert(rc == SQLITE_OK);

  rc += sqlite3_prepare_v2(db, "SELECT cid, val FROM crsql_changes", -1, &pStmt,
                           0);
  assert(rc == SQLITE_OK);
  int numRows = 0;
  // clock records should now be for column `c` with a `null` value.
  // nit: test if a default value is set for the column
  while ((rc = sqlite3_step(pStmt)) == SQLITE_ROW) {
    assert(strcmp((const char *)sqlite3_column_text(pStmt, 0), "c") == 0);
    assert(sqlite3_column_type(pStmt, 1) == SQLITE_NULL);
    ++numRows;
  }
  sqlite3_finalize(pStmt);
  // we should still have a change given we never dropped the row
  assert(numRows == 1);
  assert(rc == SQLITE_DONE);

  // insert some rows post schema change
  rc = sqlite3_exec(db, "INSERT INTO foo VALUES (2, 3);", 0, 0, 0);
  rc += sqlite3_prepare_v2(
      db, "SELECT * FROM crsql_changes WHERE db_version >= 1", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  numRows = 0;
  // Columns that no long exist post-alter should not
  // be retained for replication
  while ((rc = sqlite3_step(pStmt)) == SQLITE_ROW) {
    assert(strcmp("foo", (const char *)sqlite3_column_text(
                             pStmt, CHANGES_SINCE_VTAB_TBL)) == 0);
    const unsigned char *pkBlob = (const unsigned char *)sqlite3_column_blob(
        pStmt, CHANGES_SINCE_VTAB_PK);

    if (numRows == 0) {
      assert(pkBlob[0] == 0x01);
      assert(pkBlob[1] == 0x09);
      assert(pkBlob[2] == 0x01);
    } else {
      assert(pkBlob[0] == 0x01);
      assert(pkBlob[1] == 0x09);
      assert(pkBlob[2] == 0x02);
    }

    if (numRows == 0) {
      assert(strcmp("c", (const char *)sqlite3_column_text(
                             pStmt, CHANGES_SINCE_VTAB_CID)) == 0);
    }
    if (numRows == 1) {
      assert(strcmp("c", (const char *)sqlite3_column_text(
                             pStmt, CHANGES_SINCE_VTAB_CID)) == 0);
      assert(3 == sqlite3_column_int(pStmt, CHANGES_SINCE_VTAB_CVAL));
    }

    ++numRows;
  }
  sqlite3_finalize(pStmt);
  assert(numRows == 2);
  assert(rc == SQLITE_DONE);

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

// We no longer support this given we fixup metadata on migration.
// Maybe we should support it though?
// static void testInsertChangesWithUnkownColumnNames() {
//   printf("InsertChangesWithUnknownColumnName\n");

//   int rc = SQLITE_OK;
//   sqlite3 *db1;
//   sqlite3 *db2;
//   rc = sqlite3_open(":memory:", &db1);
//   rc += sqlite3_open(":memory:", &db2);

//   rc += sqlite3_exec(db1, "CREATE TABLE foo(a primary key not null, b);", 0,
//   0, 0); rc += sqlite3_exec(db1, "SELECT crsql_as_crr('foo')", 0, 0, 0); rc
//   += sqlite3_exec(db2, "CREATE TABLE foo(a primary key not null, c);", 0, 0,
//   0); rc += sqlite3_exec(db2, "SELECT crsql_as_crr('foo')", 0, 0, 0);
//   assert(rc == SQLITE_OK);

//   rc += sqlite3_exec(db1, "INSERT INTO foo VALUES (1, 2);", 0, 0, 0);
//   rc += sqlite3_exec(db2, "INSERT INTO foo VALUES (2, 3);", 0, 0, 0);
//   assert(rc == SQLITE_OK);

//   sqlite3_stmt *pStmtRead = 0;
//   sqlite3_stmt *pStmtWrite = 0;
//   rc +=
//       sqlite3_prepare_v2(db1, "SELECT * FROM crsql_changes", -1, &pStmtRead,
//       0);
//   rc += sqlite3_prepare_v2(
//       db2, "INSERT INTO crsql_changes VALUES (?, ?, ?, ?, ?, ?, ?, ?)", -1,
//       &pStmtWrite, 0);
//   assert(rc == SQLITE_OK);

//   while (sqlite3_step(pStmtRead) == SQLITE_ROW) {
//     for (int i = 0; i < 7; ++i) {
//       sqlite3_bind_value(pStmtWrite, i + 1, sqlite3_column_value(pStmtRead,
//       i));
//     }

//     sqlite3_step(pStmtWrite);
//     sqlite3_reset(pStmtWrite);
//   }
//   sqlite3_finalize(pStmtWrite);
//   sqlite3_finalize(pStmtRead);

//   // select all from db2.
//   // it should have a row for pk 1.
//   sqlite3_prepare_v2(db2, "SELECT * FROM foo ORDER BY a ASC", -1, &pStmtRead,
//                      0);
//   int comparisons = 0;
//   while (sqlite3_step(pStmtRead) == SQLITE_ROW) {
//     if (comparisons == 0) {
//       assert(sqlite3_column_int(pStmtRead, 0) == 1);
//       assert(sqlite3_column_type(pStmtRead, 1) == SQLITE_NULL);
//     } else {
//       assert(sqlite3_column_int(pStmtRead, 0) == 2);
//       assert(sqlite3_column_int(pStmtRead, 1) == 3);
//     }
//     comparisons += 1;
//   }
//   sqlite3_finalize(pStmtRead);

//   assert(comparisons == 2);
//   crsql_close(db1);
//   crsql_close(db2);
//   printf("\t\e[0;32mSuccess\e[0m\n");
// }

static sqlite3_int64 getDbVersion(sqlite3 *db) {
  sqlite3_stmt *pStmt = 0;
  int rc = sqlite3_prepare_v2(db, "SELECT crsql_db_version()", -1, &pStmt, 0);
  if (rc != SQLITE_OK) {
    return -1;
  }

  sqlite3_step(pStmt);
  sqlite3_int64 db2v = sqlite3_column_int64(pStmt, 0);
  sqlite3_finalize(pStmt);

  return db2v;
}

static void testLamportCondition() {
  printf("LamportCondition\n");
  // syncing from A -> B, while no changes happen on B, moves up
  // B's clock still.

  sqlite3 *db1;
  sqlite3 *db2;
  int rc = SQLITE_OK;

  rc += sqlite3_open(":memory:", &db1);
  rc += sqlite3_open(":memory:", &db2);

  rc += sqlite3_exec(
      db1, "CREATE TABLE \"hoot\" (\"a\", \"b\" primary key not null, \"c\")",
      0, 0, 0);
  rc += sqlite3_exec(
      db2, "CREATE TABLE \"hoot\" (\"a\", \"b\" primary key not null, \"c\")",
      0, 0, 0);
  rc += sqlite3_exec(db1, "SELECT crsql_as_crr('hoot');", 0, 0, 0);
  rc += sqlite3_exec(db2, "SELECT crsql_as_crr('hoot');", 0, 0, 0);
  assert(rc == SQLITE_OK);

  rc += sqlite3_exec(db1, "INSERT INTO hoot VALUES (1, 1, 1);", 0, 0, 0);
  rc += sqlite3_exec(db1, "UPDATE hoot SET a = 1 WHERE b = 1;", 0, 0, 0);
  rc += sqlite3_exec(db1, "UPDATE hoot SET a = 2 WHERE b = 1;", 0, 0, 0);
  rc += sqlite3_exec(db1, "UPDATE hoot SET a = 3 WHERE b = 1;", 0, 0, 0);
  assert(rc == SQLITE_OK);

  rc += syncLeftToRight(db1, db2, 0);
  assert(rc == SQLITE_OK);

  sqlite3_int64 db1v = getDbVersion(db1);
  sqlite3_int64 db2v = getDbVersion(db2);

  assert(db1v > 0);
  assert(db1v == db2v);

  // now update col c on db2
  // and sync right to left
  // change should be taken
  rc += sqlite3_exec(db2, "UPDATE hoot SET c = 33 WHERE b = 1", 0, 0, 0);
  rc += syncLeftToRight(db2, db1, db2v);

  sqlite3_stmt *pStmt = 0;
  sqlite3_prepare_v2(db1, "SELECT c FROM hoot WHERE b = 1", -1, &pStmt, 0);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);
  assert(sqlite3_column_int64(pStmt, 0) == 33);
  sqlite3_finalize(pStmt);

  rc = crsql_close(db1);
  assert(rc == SQLITE_OK);
  rc += crsql_close(db2);
  assert(rc == SQLITE_OK);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

// Setting a value to the same value it is already?
// No change should happen unless the versions are different.
static void noopsDoNotMoveClocks() {
  printf("NoopsDoNotMoveClocks\n");
  // syncing from A -> B, while no changes happen on B, moves up
  // B's clock still.

  sqlite3 *db1;
  sqlite3 *db2;
  int rc = SQLITE_OK;

  rc += sqlite3_open(":memory:", &db1);
  rc += sqlite3_open(":memory:", &db2);

  // swap dbs based on site id compare to make it a noop.

  rc += sqlite3_exec(
      db1, "CREATE TABLE \"hoot\" (\"a\", \"b\" primary key not null, \"c\")",
      0, 0, 0);
  rc += sqlite3_exec(
      db2, "CREATE TABLE \"hoot\" (\"a\", \"b\" primary key not null, \"c\")",
      0, 0, 0);
  rc += sqlite3_exec(db1, "SELECT crsql_as_crr('hoot');", 0, 0, 0);
  rc += sqlite3_exec(db2, "SELECT crsql_as_crr('hoot');", 0, 0, 0);
  assert(rc == SQLITE_OK);

  rc += sqlite3_exec(db1, "INSERT INTO hoot VALUES (1, 1, 1);", 0, 0, 0);
  rc += sqlite3_exec(db1, "UPDATE hoot SET a = 1 WHERE b = 1;", 0, 0, 0);
  rc += sqlite3_exec(db1, "UPDATE hoot SET a = 2 WHERE b = 1;", 0, 0, 0);
  rc += sqlite3_exec(db1, "UPDATE hoot SET a = 3 WHERE b = 1;", 0, 0, 0);
  assert(rc == SQLITE_OK);

  rc += sqlite3_exec(db2, "INSERT INTO hoot VALUES (1, 1, 1);", 0, 0, 0);
  rc += sqlite3_exec(db2, "UPDATE hoot SET a = 1 WHERE b = 1;", 0, 0, 0);
  rc += sqlite3_exec(db2, "UPDATE hoot SET a = 2 WHERE b = 1;", 0, 0, 0);
  rc += sqlite3_exec(db2, "UPDATE hoot SET a = 3 WHERE b = 1;", 0, 0, 0);
  assert(rc == SQLITE_OK);

  sqlite3_int64 db1vPre = getDbVersion(db1);
  sqlite3_int64 db2vPre = getDbVersion(db2);

  // identical
  assert(db1vPre == db2vPre);

  rc += syncLeftToRight(db1, db2, 0);
  assert(rc == SQLITE_OK);

  sqlite3_int64 db1vPost = getDbVersion(db1);
  sqlite3_int64 db2vPost = getDbVersion(db2);

  assert(db1vPre == db2vPost);
  assert(db1vPre == db1vPost);

  rc = crsql_close(db1);
  assert(rc == SQLITE_OK);
  rc += crsql_close(db2);
  assert(rc == SQLITE_OK);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testPullingOnlyLocalChanges() {
  /**
   * site_id IS NULL would be local changes.
   */
  printf("PullingOnlyLocalChanges\n");
  sqlite3 *db;
  int rc = SQLITE_OK;

  rc = sqlite3_open(":memory:", &db);
  rc += sqlite3_exec(db, "CREATE TABLE node (id primary key not null, content)",
                     0, 0, 0);
  rc += sqlite3_exec(db, "SELECT crsql_as_crr('node')", 0, 0, 0);
  rc += sqlite3_exec(db, "INSERT INTO node VALUES (1, 'some str')", 0, 0, 0);
  rc += sqlite3_exec(db, "INSERT INTO node VALUES (2, 'other str')", 0, 0, 0);
  assert(rc == SQLITE_OK);

  sqlite3_stmt *pStmt;
  // TODO: why does `IS NULL` not work in the vtab???
  // `IS NOT NULL` also fails to call the virtual table bestIndex function with
  // any constraints p pIdxInfo->nConstraint
  sqlite3_prepare_v2(
      db, "SELECT count(*) FROM crsql_changes WHERE site_id IS crsql_site_id()",
      -1, &pStmt, 0);

  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);

  int count = sqlite3_column_int(pStmt, 0);
  // we created 2 local changes, we should get 2 changes back. Well 4 really
  // since row creation is an event.
  printf("count: %d\n", count);
  assert(count == 2);
  sqlite3_finalize(pStmt);

  sqlite3_prepare_v2(
      db,
      "SELECT count(*) FROM crsql_changes WHERE site_id IS NOT crsql_site_id()",
      -1, &pStmt, 0);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);
  count = sqlite3_column_int(pStmt, 0);
  // we asked for changes that were not local
  assert(count == 0);
  sqlite3_finalize(pStmt);

  // now sync in some chnages from elsewhere
  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testSyncBit() {
  printf("SyncBit\n");
  int rc = SQLITE_OK;
  sqlite3 *db;
  sqlite3_stmt *pStmt = 0;

  rc = sqlite3_open(":memory:", &db);
  rc += sqlite3_exec(
      db, "CREATE TABLE foo (a primary key not null, b)", 0, 0, 0);
  rc += sqlite3_exec(db, "SELECT crsql_as_crr('foo')", 0, 0, 0);
  assert(rc == SQLITE_OK);

  // Set sync bit — triggers should NOT fire
  rc = sqlite3_exec(db, "SELECT crsql_internal_sync_bit(1)", 0, 0, 0);
  assert(rc == SQLITE_OK);
  rc = sqlite3_exec(db, "INSERT INTO foo VALUES (1, 'hello')", 0, 0, 0);
  assert(rc == SQLITE_OK);

  // Check clock table — should have NO entries because sync bit suppresses
  rc = sqlite3_prepare_v2(
      db, "SELECT count(*) FROM foo__crsql_clock", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);
  assert(sqlite3_column_int(pStmt, 0) == 0);
  sqlite3_finalize(pStmt);

  // Clear sync bit — triggers should fire
  rc = sqlite3_exec(db, "SELECT crsql_internal_sync_bit(0)", 0, 0, 0);
  assert(rc == SQLITE_OK);
  rc = sqlite3_exec(db, "INSERT INTO foo VALUES (2, 'world')", 0, 0, 0);
  assert(rc == SQLITE_OK);

  // Clock table should now have entries for row 2
  rc = sqlite3_prepare_v2(
      db, "SELECT count(*) FROM foo__crsql_clock", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);
  assert(sqlite3_column_int(pStmt, 0) > 0);
  sqlite3_finalize(pStmt);

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testDbVersion() {
  printf("DbVersion\n");
  int rc = SQLITE_OK;
  sqlite3 *db;

  rc = sqlite3_open(":memory:", &db);
  rc += sqlite3_exec(
      db, "CREATE TABLE foo (a primary key not null, b)", 0, 0, 0);
  rc += sqlite3_exec(db, "SELECT crsql_as_crr('foo')", 0, 0, 0);
  assert(rc == SQLITE_OK);

  sqlite3_int64 v0 = getDbVersion(db);
  // Fresh db with CRR but no data — version should be 0
  assert(v0 == 0);

  // Insert in first transaction
  rc = sqlite3_exec(db, "INSERT INTO foo VALUES (1, 'a')", 0, 0, 0);
  assert(rc == SQLITE_OK);
  sqlite3_int64 v1 = getDbVersion(db);
  assert(v1 > v0);

  // Insert in second transaction
  rc = sqlite3_exec(db, "INSERT INTO foo VALUES (2, 'b')", 0, 0, 0);
  assert(rc == SQLITE_OK);
  sqlite3_int64 v2 = getDbVersion(db);
  assert(v2 > v1);

  // Insert in third transaction
  rc = sqlite3_exec(db, "INSERT INTO foo VALUES (3, 'c')", 0, 0, 0);
  assert(rc == SQLITE_OK);
  sqlite3_int64 v3 = getDbVersion(db);
  assert(v3 > v2);

  // Monotonically increasing
  assert(v3 > v2 && v2 > v1 && v1 > v0);

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testSiteId() {
  printf("SiteId\n");
  int rc = SQLITE_OK;
  sqlite3 *db1;
  sqlite3 *db2;
  sqlite3_stmt *pStmt1 = 0;
  sqlite3_stmt *pStmt2 = 0;

  rc = sqlite3_open(":memory:", &db1);
  rc += sqlite3_open(":memory:", &db2);
  rc += sqlite3_exec(
      db1, "CREATE TABLE foo (a primary key not null, b)", 0, 0, 0);
  rc += sqlite3_exec(db1, "SELECT crsql_as_crr('foo')", 0, 0, 0);
  rc += sqlite3_exec(
      db2, "CREATE TABLE foo (a primary key not null, b)", 0, 0, 0);
  rc += sqlite3_exec(db2, "SELECT crsql_as_crr('foo')", 0, 0, 0);
  assert(rc == SQLITE_OK);

  // Site ID should be a 16-byte blob
  rc = sqlite3_prepare_v2(db1, "SELECT crsql_site_id()", -1, &pStmt1, 0);
  assert(rc == SQLITE_OK);
  rc = sqlite3_step(pStmt1);
  assert(rc == SQLITE_ROW);
  assert(sqlite3_column_type(pStmt1, 0) == SQLITE_BLOB);
  assert(sqlite3_column_bytes(pStmt1, 0) == SITE_ID_LEN);
  const void *sid1 = sqlite3_column_blob(pStmt1, 0);

  // Should be stable across calls within same connection
  sqlite3_stmt *pStmt1b = 0;
  rc = sqlite3_prepare_v2(db1, "SELECT crsql_site_id()", -1, &pStmt1b, 0);
  assert(rc == SQLITE_OK);
  rc = sqlite3_step(pStmt1b);
  assert(rc == SQLITE_ROW);
  assert(memcmp(sid1, sqlite3_column_blob(pStmt1b, 0), SITE_ID_LEN) == 0);
  sqlite3_finalize(pStmt1b);

  // Different DB should have different site ID
  rc = sqlite3_prepare_v2(db2, "SELECT crsql_site_id()", -1, &pStmt2, 0);
  assert(rc == SQLITE_OK);
  rc = sqlite3_step(pStmt2);
  assert(rc == SQLITE_ROW);
  assert(sqlite3_column_bytes(pStmt2, 0) == SITE_ID_LEN);
  assert(memcmp(sid1, sqlite3_column_blob(pStmt2, 0), SITE_ID_LEN) != 0);

  sqlite3_finalize(pStmt1);
  sqlite3_finalize(pStmt2);
  crsql_close(db1);
  crsql_close(db2);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testRequiredPrimaryKey() {
  printf("RequiredPrimaryKey\n");
  int rc = SQLITE_OK;
  sqlite3 *db;
  char *err = 0;

  rc = sqlite3_open(":memory:", &db);
  // Table with no explicit primary key (rowid only)
  rc += sqlite3_exec(db, "CREATE TABLE nopk (a, b)", 0, 0, 0);
  assert(rc == SQLITE_OK);

  // crsql_as_crr should fail on a table without an explicit PK
  rc = sqlite3_exec(db, "SELECT crsql_as_crr('nopk')", 0, 0, &err);
  assert(rc != SQLITE_OK);
  if (err) {
    sqlite3_free(err);
  }

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testModifySinglePK() {
  printf("ModifySinglePK\n");
  int rc = SQLITE_OK;
  sqlite3 *db;
  sqlite3_stmt *pStmt = 0;

  rc = sqlite3_open(":memory:", &db);
  rc += sqlite3_exec(
      db, "CREATE TABLE foo (a primary key not null, b)", 0, 0, 0);
  rc += sqlite3_exec(db, "SELECT crsql_as_crr('foo')", 0, 0, 0);
  assert(rc == SQLITE_OK);

  // Insert a row
  rc = sqlite3_exec(db, "INSERT INTO foo VALUES (1, 'hello')", 0, 0, 0);
  assert(rc == SQLITE_OK);
  sqlite3_int64 v1 = getDbVersion(db);

  // Update PK: old row should be deleted, new row created
  rc = sqlite3_exec(db, "UPDATE foo SET a = 2 WHERE a = 1", 0, 0, 0);
  assert(rc == SQLITE_OK);
  sqlite3_int64 v2 = getDbVersion(db);
  assert(v2 > v1);

  // Old row should not exist
  rc = sqlite3_prepare_v2(
      db, "SELECT count(*) FROM foo WHERE a = 1", -1, &pStmt, 0);
  sqlite3_step(pStmt);
  assert(sqlite3_column_int(pStmt, 0) == 0);
  sqlite3_finalize(pStmt);

  // New row should exist
  rc = sqlite3_prepare_v2(
      db, "SELECT b FROM foo WHERE a = 2", -1, &pStmt, 0);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);
  assert(strcmp((const char *)sqlite3_column_text(pStmt, 0), "hello") == 0);
  sqlite3_finalize(pStmt);

  // Old key should have a delete sentinel (even CL) in clock table
  rc = sqlite3_prepare_v2(
      db,
      "SELECT col_version FROM foo__crsql_clock WHERE key = 1 AND "
      "col_name = '" SENTINEL_CID "'",
      -1, &pStmt, 0);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);
  // CL should be even (deleted)
  sqlite3_int64 cl = sqlite3_column_int64(pStmt, 0);
  assert(cl % 2 == 0);
  sqlite3_finalize(pStmt);

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testModifyCompoundPK() {
  printf("ModifyCompoundPK\n");
  int rc = SQLITE_OK;
  sqlite3 *db;
  sqlite3_stmt *pStmt = 0;

  rc = sqlite3_open(":memory:", &db);
  rc += sqlite3_exec(
      db, "CREATE TABLE bar (a not null, b not null, c, PRIMARY KEY (a, b))",
      0, 0, 0);
  rc += sqlite3_exec(db, "SELECT crsql_as_crr('bar')", 0, 0, 0);
  assert(rc == SQLITE_OK);

  // Insert a row
  rc = sqlite3_exec(db, "INSERT INTO bar VALUES (1, 2, 'data')", 0, 0, 0);
  assert(rc == SQLITE_OK);
  sqlite3_int64 v1 = getDbVersion(db);

  // Modify one PK column
  rc = sqlite3_exec(db, "UPDATE bar SET b = 3 WHERE a = 1 AND b = 2", 0, 0, 0);
  assert(rc == SQLITE_OK);
  sqlite3_int64 v2 = getDbVersion(db);
  assert(v2 > v1);

  // Old row gone, new row exists
  rc = sqlite3_prepare_v2(
      db, "SELECT count(*) FROM bar WHERE a = 1 AND b = 2", -1, &pStmt, 0);
  sqlite3_step(pStmt);
  assert(sqlite3_column_int(pStmt, 0) == 0);
  sqlite3_finalize(pStmt);

  rc = sqlite3_prepare_v2(
      db, "SELECT c FROM bar WHERE a = 1 AND b = 3", -1, &pStmt, 0);
  rc = sqlite3_step(pStmt);
  assert(rc == SQLITE_ROW);
  assert(strcmp((const char *)sqlite3_column_text(pStmt, 0), "data") == 0);
  sqlite3_finalize(pStmt);

  crsql_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

// Hard check that survives -DNDEBUG (assert() is a no-op in release).
// Existing tests in this file embed sqlite3_step inside assert(), which
// makes them vacuous in release; the test suite as a whole inherits that
// limitation but this new test must not.
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,     \
              #cond);                                                        \
      abort();                                                               \
    }                                                                        \
  } while (0)

// Local sync helper that does not depend on assert(), unlike the file-wide
// syncLeftToRight which is a no-op under -DNDEBUG.
static int checkedSyncLeftToRight(sqlite3 *src, sqlite3 *dst) {
  sqlite3_stmt *pSid = 0;
  if (sqlite3_prepare_v2(dst, "SELECT crsql_site_id()", -1, &pSid, 0) !=
      SQLITE_OK)
    return SQLITE_ERROR;
  if (sqlite3_step(pSid) != SQLITE_ROW) {
    sqlite3_finalize(pSid);
    return SQLITE_ERROR;
  }
  sqlite3_stmt *pRead = 0;
  if (sqlite3_prepare_v2(
          src,
          "SELECT * FROM crsql_changes WHERE site_id IS NOT ?",
          -1, &pRead, 0) != SQLITE_OK) {
    sqlite3_finalize(pSid);
    return SQLITE_ERROR;
  }
  sqlite3_bind_value(pRead, 1, sqlite3_column_value(pSid, 0));

  sqlite3_stmt *pWrite = 0;
  if (sqlite3_prepare_v2(
          dst,
          "INSERT INTO crsql_changes VALUES (?,?,?,?,?,?,?,?,?)", -1,
          &pWrite, 0) != SQLITE_OK) {
    sqlite3_finalize(pRead);
    sqlite3_finalize(pSid);
    return SQLITE_ERROR;
  }

  int rc;
  while ((rc = sqlite3_step(pRead)) == SQLITE_ROW) {
    for (int i = 0; i < 9; i++) {
      if (sqlite3_bind_value(pWrite, i + 1, sqlite3_column_value(pRead, i)) !=
          SQLITE_OK) {
        rc = SQLITE_ERROR;
        goto done;
      }
    }
    if (sqlite3_step(pWrite) != SQLITE_DONE) {
      fprintf(stderr, "merge step failed: %s\n", sqlite3_errmsg(dst));
      rc = SQLITE_ERROR;
      goto done;
    }
    sqlite3_reset(pWrite);
  }
  if (rc == SQLITE_DONE) rc = SQLITE_OK;
done:
  sqlite3_finalize(pWrite);
  sqlite3_finalize(pRead);
  sqlite3_finalize(pSid);
  return rc;
}

// Verify the auto-tracking behavior of crsql_tracked_peers and that
// crsql_set_tracked_peer's monotonic upsert never rolls a watermark
// backwards. After db1 -> db2 sync, db2.crsql_tracked_peers must have
// exactly one row whose site_id matches db1, event = 0 (RECEIVED), and
// (version, seq) match the max db_version / seq actually merged.
static void testTrackedPeers() {
  printf("trackedPeers\n");

  sqlite3 *db1 = 0;
  sqlite3 *db2 = 0;
  sqlite3_stmt *pStmt = 0;
  char *err = 0;
  int rc;

  CHECK(sqlite3_open(":memory:", &db1) == SQLITE_OK);
  CHECK(sqlite3_open(":memory:", &db2) == SQLITE_OK);
  CHECK(createSimpleSchema(db1, &err) == SQLITE_OK);
  CHECK(createSimpleSchema(db2, &err) == SQLITE_OK);

  // Sanity: each fresh db sees an empty peer-tracking table.
  CHECK(sqlite3_prepare_v2(db2, "SELECT count(*) FROM crsql_tracked_peers",
                            -1, &pStmt, 0) == SQLITE_OK);
  CHECK(sqlite3_step(pStmt) == SQLITE_ROW);
  CHECK(sqlite3_column_int(pStmt, 0) == 0);
  sqlite3_finalize(pStmt);

  CHECK(sqlite3_exec(db1, "insert into foo values (1, 'a');", 0, 0, &err) ==
        SQLITE_OK);
  CHECK(sqlite3_exec(db1, "insert into foo values (2, 'b');", 0, 0, &err) ==
        SQLITE_OK);

  // Capture the max (db_version, seq) db1 produced; db2 must end up
  // tracking exactly this watermark for db1 after the sync.
  sqlite3_int64 expectedVersion = 0;
  sqlite3_int64 expectedSeq = 0;
  CHECK(sqlite3_prepare_v2(
            db1,
            "SELECT max(db_version), max(seq) FROM crsql_changes",
            -1, &pStmt, 0) == SQLITE_OK);
  CHECK(sqlite3_step(pStmt) == SQLITE_ROW);
  expectedVersion = sqlite3_column_int64(pStmt, 0);
  expectedSeq = sqlite3_column_int64(pStmt, 1);
  sqlite3_finalize(pStmt);
  CHECK(expectedVersion > 0);

  CHECK(checkedSyncLeftToRight(db1, db2) == SQLITE_OK);

  // db2 should now have exactly one peer row, for db1, with event=RECEIVED
  // and the watermark we captured above.
  CHECK(sqlite3_prepare_v2(
            db2,
            "SELECT site_id, version, seq, tag, event "
            "FROM crsql_tracked_peers",
            -1, &pStmt, 0) == SQLITE_OK);
  CHECK(sqlite3_step(pStmt) == SQLITE_ROW);

  unsigned char trackedSiteId[16];
  CHECK(sqlite3_column_bytes(pStmt, 0) == 16);
  memcpy(trackedSiteId, sqlite3_column_blob(pStmt, 0), 16);
  CHECK(sqlite3_column_int64(pStmt, 1) == expectedVersion);
  CHECK(sqlite3_column_int64(pStmt, 2) == expectedSeq);
  CHECK(sqlite3_column_int(pStmt, 3) == 0);   // tag
  CHECK(sqlite3_column_int(pStmt, 4) == 0);   // RECEIVED

  // No second row from this single sync.
  CHECK(sqlite3_step(pStmt) == SQLITE_DONE);
  sqlite3_finalize(pStmt);

  // Recorded site_id must be db1's, not db2's own.
  CHECK(sqlite3_prepare_v2(db1, "SELECT crsql_site_id()", -1, &pStmt, 0) ==
        SQLITE_OK);
  CHECK(sqlite3_step(pStmt) == SQLITE_ROW);
  CHECK(sqlite3_column_bytes(pStmt, 0) == 16);
  CHECK(memcmp(trackedSiteId, sqlite3_column_blob(pStmt, 0), 16) == 0);
  sqlite3_finalize(pStmt);

  // Re-syncing the same changes must not advance the watermark (idempotent
  // upsert with strict-greater guard).
  CHECK(checkedSyncLeftToRight(db1, db2) == SQLITE_OK);
  CHECK(sqlite3_prepare_v2(db2,
                            "SELECT version, seq FROM crsql_tracked_peers",
                            -1, &pStmt, 0) == SQLITE_OK);
  CHECK(sqlite3_step(pStmt) == SQLITE_ROW);
  CHECK(sqlite3_column_int64(pStmt, 0) == expectedVersion);
  CHECK(sqlite3_column_int64(pStmt, 1) == expectedSeq);
  sqlite3_finalize(pStmt);

  // Local writes on db2 must not produce a self-row.
  CHECK(sqlite3_exec(db2, "insert into foo values (3, 'c');", 0, 0, &err) ==
        SQLITE_OK);
  CHECK(sqlite3_prepare_v2(
            db2, "SELECT count(*) FROM crsql_tracked_peers", -1, &pStmt, 0) ==
        SQLITE_OK);
  CHECK(sqlite3_step(pStmt) == SQLITE_ROW);
  CHECK(sqlite3_column_int(pStmt, 0) == 1);
  sqlite3_finalize(pStmt);

  // crsql_set_tracked_peer writes a SENT watermark, then a backwards
  // attempt is silently ignored (monotonic), then a forwards attempt wins.
  rc = sqlite3_exec(
      db2,
      "SELECT crsql_set_tracked_peer("
      "  (SELECT site_id FROM crsql_tracked_peers LIMIT 1), 100, 5, 0, 1)",
      0, 0, &err);
  CHECK(rc == SQLITE_OK);

  CHECK(sqlite3_prepare_v2(
            db2,
            "SELECT version, seq FROM crsql_tracked_peers WHERE event = 1",
            -1, &pStmt, 0) == SQLITE_OK);
  CHECK(sqlite3_step(pStmt) == SQLITE_ROW);
  CHECK(sqlite3_column_int64(pStmt, 0) == 100);
  CHECK(sqlite3_column_int64(pStmt, 1) == 5);
  sqlite3_finalize(pStmt);

  // Backwards write — should be a no-op.
  rc = sqlite3_exec(
      db2,
      "SELECT crsql_set_tracked_peer("
      "  (SELECT site_id FROM crsql_tracked_peers WHERE event = 1), "
      "  50, 99, 0, 1)",
      0, 0, &err);
  CHECK(rc == SQLITE_OK);
  CHECK(sqlite3_prepare_v2(
            db2,
            "SELECT version, seq FROM crsql_tracked_peers WHERE event = 1",
            -1, &pStmt, 0) == SQLITE_OK);
  CHECK(sqlite3_step(pStmt) == SQLITE_ROW);
  CHECK(sqlite3_column_int64(pStmt, 0) == 100);
  CHECK(sqlite3_column_int64(pStmt, 1) == 5);
  sqlite3_finalize(pStmt);

  // Forwards write — wins.
  rc = sqlite3_exec(
      db2,
      "SELECT crsql_set_tracked_peer("
      "  (SELECT site_id FROM crsql_tracked_peers WHERE event = 1), "
      "  100, 6, 0, 1)",
      0, 0, &err);
  CHECK(rc == SQLITE_OK);
  CHECK(sqlite3_prepare_v2(
            db2,
            "SELECT version, seq FROM crsql_tracked_peers WHERE event = 1",
            -1, &pStmt, 0) == SQLITE_OK);
  CHECK(sqlite3_step(pStmt) == SQLITE_ROW);
  CHECK(sqlite3_column_int64(pStmt, 0) == 100);
  CHECK(sqlite3_column_int64(pStmt, 1) == 6);
  sqlite3_finalize(pStmt);

  crsql_close(db1);
  crsql_close(db2);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

void crsqlTestSuite() {
  printf("\e[47m\e[1;30mSuite: crsql\e[0m\n");

  teste2e();
  testSelectChangesAfterChangingColumnName();
  testLamportCondition();
  noopsDoNotMoveClocks();
  testPullingOnlyLocalChanges();
  testSyncBit();
  testDbVersion();
  testSiteId();
  testRequiredPrimaryKey();
  testModifySinglePK();
  testModifyCompoundPK();
  testTrackedPeers();
}