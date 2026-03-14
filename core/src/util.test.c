#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "consts.h"
#include "crsqlite.h"
#include "util.h"

int crsql_close(sqlite3 *db);

static void testEscapeIdent() {
  printf("EscapeIdent\n");
  char *r;

  // No quotes
  r = crsql_escape_ident("foo");
  assert(strcmp(r, "foo") == 0);
  sqlite3_free(r);

  // Embedded double-quote
  r = crsql_escape_ident("foo\"bar");
  assert(strcmp(r, "foo\"\"bar") == 0);
  sqlite3_free(r);

  // All double-quotes
  r = crsql_escape_ident("\"\"");
  assert(strcmp(r, "\"\"\"\"") == 0);
  sqlite3_free(r);

  // Empty string
  r = crsql_escape_ident("");
  assert(strcmp(r, "") == 0);
  sqlite3_free(r);

  // Single quote should pass through unchanged
  r = crsql_escape_ident("it's");
  assert(strcmp(r, "it's") == 0);
  sqlite3_free(r);

  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testEscapeIdentAsValue() {
  printf("EscapeIdentAsValue\n");
  char *r;

  // No quotes
  r = crsql_escape_ident_as_value("foo");
  assert(strcmp(r, "foo") == 0);
  sqlite3_free(r);

  // Embedded single-quote
  r = crsql_escape_ident_as_value("it's");
  assert(strcmp(r, "it''s") == 0);
  sqlite3_free(r);

  // All single-quotes
  r = crsql_escape_ident_as_value("''");
  assert(strcmp(r, "''''") == 0);
  sqlite3_free(r);

  // Empty string
  r = crsql_escape_ident_as_value("");
  assert(strcmp(r, "") == 0);
  sqlite3_free(r);

  // Double quote should pass through unchanged
  r = crsql_escape_ident_as_value("foo\"bar");
  assert(strcmp(r, "foo\"bar") == 0);
  sqlite3_free(r);

  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testBindingList() {
  printf("BindingList\n");
  char *r;

  // 0 slots
  r = crsql_binding_list(0);
  assert(r != 0);
  assert(strcmp(r, "") == 0);
  sqlite3_free(r);

  // Negative slots
  r = crsql_binding_list(-1);
  assert(r != 0);
  assert(strcmp(r, "") == 0);
  sqlite3_free(r);

  // 1 slot
  r = crsql_binding_list(1);
  assert(strcmp(r, "?") == 0);
  sqlite3_free(r);

  // 3 slots
  r = crsql_binding_list(3);
  assert(strcmp(r, "?, ?, ?") == 0);
  sqlite3_free(r);

  printf("\t\e[0;32mSuccess\e[0m\n");
}

static crsql_ColumnInfo makeColInfo(const char *name) {
  crsql_ColumnInfo ci;
  memset(&ci, 0, sizeof(ci));
  ci.name = (char *)name;  // test only — not freed
  ci.cid = 0;
  ci.pk = 0;
  return ci;
}

static void testWhereList() {
  printf("WhereList\n");
  char *r;

  // Single column, no prefix
  crsql_ColumnInfo cols1[1];
  cols1[0] = makeColInfo("a");
  r = crsql_where_list(cols1, 1, NULL);
  assert(r != 0);
  assert(strcmp(r, "\"a\" IS ?") == 0);
  sqlite3_free(r);

  // Two columns, no prefix
  crsql_ColumnInfo cols2[2];
  cols2[0] = makeColInfo("a");
  cols2[1] = makeColInfo("b");
  r = crsql_where_list(cols2, 2, NULL);
  assert(r != 0);
  assert(strcmp(r, "\"a\" IS ? AND \"b\" IS ?") == 0);
  sqlite3_free(r);

  // With prefix
  r = crsql_where_list(cols1, 1, "t.");
  assert(r != 0);
  assert(strcmp(r, "t.\"a\" IS ?") == 0);
  sqlite3_free(r);

  // Column name with embedded double-quote
  crsql_ColumnInfo colQuote[1];
  colQuote[0] = makeColInfo("col\"x");
  r = crsql_where_list(colQuote, 1, NULL);
  assert(r != 0);
  assert(strcmp(r, "\"col\"\"x\" IS ?") == 0);
  sqlite3_free(r);

  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testAsIdentifierList() {
  printf("AsIdentifierList\n");
  char *r;

  // Single column
  crsql_ColumnInfo cols1[1];
  cols1[0] = makeColInfo("a");
  r = crsql_as_identifier_list(cols1, 1, NULL);
  assert(r != 0);
  assert(strcmp(r, "\"a\"") == 0);
  sqlite3_free(r);

  // Three columns
  crsql_ColumnInfo cols3[3];
  cols3[0] = makeColInfo("a");
  cols3[1] = makeColInfo("b");
  cols3[2] = makeColInfo("c");
  r = crsql_as_identifier_list(cols3, 3, NULL);
  assert(r != 0);
  assert(strcmp(r, "\"a\",\"b\",\"c\"") == 0);
  sqlite3_free(r);

  // With prefix
  r = crsql_as_identifier_list(cols3, 3, "t.");
  assert(r != 0);
  assert(strcmp(r, "t.\"a\",t.\"b\",t.\"c\"") == 0);
  sqlite3_free(r);

  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testSlabRowid() {
  printf("SlabRowid\n");

  // idx=0 should just be the rowid modulo
  assert(crsql_slab_rowid(0, 5) == 5);
  assert(crsql_slab_rowid(0, 0) == 0);

  // idx=1 should offset by ROWID_SLAB_SIZE
  assert(crsql_slab_rowid(1, 5) == ROWID_SLAB_SIZE + 5);
  assert(crsql_slab_rowid(2, 5) == 2 * ROWID_SLAB_SIZE + 5);

  // Negative idx returns -1
  assert(crsql_slab_rowid(-1, 5) == -1);

  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testGetDbVersionUnionQuery() {
  printf("GetDbVersionUnionQuery\n");
  char *r;

  // Single table
  char *tables1[] = {"foo__crsql_clock"};
  r = crsql_get_db_version_union_query(tables1, 1);
  assert(r != 0);
  assert(strstr(r, "foo__crsql_clock") != 0);
  assert(strstr(r, "UNION ALL") == 0);  // no UNION ALL with 1 table
  assert(strstr(r, "max(version)") != 0);
  assert(strstr(r, "pre_compact_dbversion") != 0);
  sqlite3_free(r);

  // Three tables
  char *tables3[] = {"a__crsql_clock", "b__crsql_clock", "c__crsql_clock"};
  r = crsql_get_db_version_union_query(tables3, 3);
  assert(r != 0);
  assert(strstr(r, "a__crsql_clock") != 0);
  assert(strstr(r, "b__crsql_clock") != 0);
  assert(strstr(r, "c__crsql_clock") != 0);
  // Should have 2 UNION ALLs for 3 tables
  char *first = strstr(r, "UNION ALL");
  assert(first != 0);
  char *second = strstr(first + 9, "UNION ALL");
  assert(second != 0);
  sqlite3_free(r);

  // Table name with embedded double-quote
  char *tablesQuote[] = {"x\"y__crsql_clock"};
  r = crsql_get_db_version_union_query(tablesQuote, 1);
  assert(r != 0);
  // The escaped name should have doubled quotes
  assert(strstr(r, "x\"\"y__crsql_clock") != 0);
  sqlite3_free(r);

  printf("\t\e[0;32mSuccess\e[0m\n");
}

static void testCompareSqliteValues() {
  printf("CompareSqliteValues\n");
  sqlite3 *db;
  sqlite3_open(":memory:", &db);

  sqlite3_stmt *pStmt;
  int rc;

  // Compare two integers
  rc = sqlite3_prepare_v2(db, "SELECT ?, ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  sqlite3_bind_int64(pStmt, 1, 10);
  sqlite3_bind_int64(pStmt, 2, 20);
  sqlite3_step(pStmt);
  assert(crsql_compare_sqlite_values(sqlite3_column_value(pStmt, 0),
                                     sqlite3_column_value(pStmt, 1)) < 0);
  assert(crsql_compare_sqlite_values(sqlite3_column_value(pStmt, 1),
                                     sqlite3_column_value(pStmt, 0)) > 0);
  assert(crsql_compare_sqlite_values(sqlite3_column_value(pStmt, 0),
                                     sqlite3_column_value(pStmt, 0)) == 0);
  sqlite3_finalize(pStmt);

  // Compare two texts
  rc = sqlite3_prepare_v2(db, "SELECT ?, ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  sqlite3_bind_text(pStmt, 1, "abc", -1, SQLITE_STATIC);
  sqlite3_bind_text(pStmt, 2, "xyz", -1, SQLITE_STATIC);
  sqlite3_step(pStmt);
  assert(crsql_compare_sqlite_values(sqlite3_column_value(pStmt, 0),
                                     sqlite3_column_value(pStmt, 1)) < 0);
  sqlite3_finalize(pStmt);

  // Cross-type: NULL vs INTEGER
  // NULL is type 5, INTEGER is type 1
  // rType - lType = 1 - 5 = -4 (NULL sorts less)
  rc = sqlite3_prepare_v2(db, "SELECT ?, ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  sqlite3_bind_null(pStmt, 1);
  sqlite3_bind_int64(pStmt, 2, 42);
  sqlite3_step(pStmt);
  assert(crsql_compare_sqlite_values(sqlite3_column_value(pStmt, 0),
                                     sqlite3_column_value(pStmt, 1)) < 0);
  assert(crsql_compare_sqlite_values(sqlite3_column_value(pStmt, 1),
                                     sqlite3_column_value(pStmt, 0)) > 0);
  sqlite3_finalize(pStmt);

  // NULL vs NULL
  rc = sqlite3_prepare_v2(db, "SELECT ?, ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  sqlite3_bind_null(pStmt, 1);
  sqlite3_bind_null(pStmt, 2);
  sqlite3_step(pStmt);
  assert(crsql_compare_sqlite_values(sqlite3_column_value(pStmt, 0),
                                     sqlite3_column_value(pStmt, 1)) == 0);
  sqlite3_finalize(pStmt);

  // Compare two floats
  rc = sqlite3_prepare_v2(db, "SELECT ?, ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  sqlite3_bind_double(pStmt, 1, 1.5);
  sqlite3_bind_double(pStmt, 2, 2.5);
  sqlite3_step(pStmt);
  assert(crsql_compare_sqlite_values(sqlite3_column_value(pStmt, 0),
                                     sqlite3_column_value(pStmt, 1)) < 0);
  sqlite3_finalize(pStmt);

  // Compare two blobs
  rc = sqlite3_prepare_v2(db, "SELECT ?, ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  sqlite3_bind_blob(pStmt, 1, "abc", 3, SQLITE_STATIC);
  sqlite3_bind_blob(pStmt, 2, "abd", 3, SQLITE_STATIC);
  sqlite3_step(pStmt);
  assert(crsql_compare_sqlite_values(sqlite3_column_value(pStmt, 0),
                                     sqlite3_column_value(pStmt, 1)) < 0);
  sqlite3_finalize(pStmt);

  // Blob length comparison: same prefix, different length
  rc = sqlite3_prepare_v2(db, "SELECT ?, ?", -1, &pStmt, 0);
  assert(rc == SQLITE_OK);
  sqlite3_bind_blob(pStmt, 1, "ab", 2, SQLITE_STATIC);
  sqlite3_bind_blob(pStmt, 2, "abc", 3, SQLITE_STATIC);
  sqlite3_step(pStmt);
  assert(crsql_compare_sqlite_values(sqlite3_column_value(pStmt, 0),
                                     sqlite3_column_value(pStmt, 1)) < 0);
  sqlite3_finalize(pStmt);

  sqlite3_close(db);
  printf("\t\e[0;32mSuccess\e[0m\n");
}

void crsqlUtilTestSuite() {
  printf("\e[47m\e[1;30mSuite: util\e[0m\n");

  testEscapeIdent();
  testEscapeIdentAsValue();
  testBindingList();
  testWhereList();
  testAsIdentifierList();
  testSlabRowid();
  testGetDbVersionUnionQuery();
  testCompareSqliteValues();
}
