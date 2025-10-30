#include "backfill.h"
#include "str_util.h"
#include "consts.h"
#include <string.h>

// Forward declarations
static int create_clock_rows_from_stmt(sqlite3_stmt *read_stmt, sqlite3 *db,
                                       const char *table, crsql_ColumnInfo **pks,
                                       int pks_len, crsql_ColumnInfo **non_pks,
                                       int non_pks_len, int is_commit_alter);

static sqlite3_int64 get_or_create_key(sqlite3_stmt *select_stmt, sqlite3_stmt *create_stmt,
                                       crsql_ColumnInfo **pks, int pks_len,
                                       sqlite3_stmt *read_stmt);

static int backfill_missing_columns(sqlite3 *db, const char *table,
                                   crsql_ColumnInfo **pks, int pks_len,
                                   crsql_ColumnInfo **non_pks, int non_pks_len,
                                   int is_commit_alter);

static int fill_column(sqlite3 *db, const char *table, crsql_ColumnInfo **pks,
                      int pks_len, crsql_ColumnInfo *non_pk_col, int is_commit_alter);

// Main backfill function
int crsql_backfill_table(sqlite3 *db, const char *table, crsql_ColumnInfo **pks,
                         int pks_len, crsql_ColumnInfo **non_pks, int non_pks_len,
                         int is_commit_alter, int no_tx) {
  if (!no_tx) {
    char *err = NULL;
    int rc = sqlite3_exec(db, "SAVEPOINT backfill", NULL, NULL, &err);
    if (err) sqlite3_free(err);
    if (rc != SQLITE_OK) return rc;
  }

  // Find rows in table not in __crsql_pks
  char *pk_cols_list = crsql_as_identifier_list(pks, pks_len, NULL);
  char *escaped_table = crsql_escape_ident(table);

  char *sql = sqlite3_mprintf(
    "SELECT %s FROM \"%s\" AS t1 "
    "EXCEPT SELECT %s FROM \"%s__crsql_pks\" AS t2",
    pk_cols_list, escaped_table, pk_cols_list, escaped_table
  );

  sqlite3_free(pk_cols_list);
  sqlite3_free(escaped_table);

  if (!sql) {
    if (!no_tx) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return SQLITE_NOMEM;
  }

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    if (!no_tx) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return rc;
  }

  rc = create_clock_rows_from_stmt(stmt, db, table, pks, pks_len,
                                   non_pks, non_pks_len, is_commit_alter);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_OK) {
    if (!no_tx) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return rc;
  }

  rc = backfill_missing_columns(db, table, pks, pks_len, non_pks,
                               non_pks_len, is_commit_alter);

  if (rc != SQLITE_OK) {
    if (!no_tx) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return rc;
  }

  if (!no_tx) {
    char *err = NULL;
    rc = sqlite3_exec(db, "RELEASE backfill", NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc;
  }

  return SQLITE_OK;
}

// Create clock rows from statement results
static int create_clock_rows_from_stmt(sqlite3_stmt *read_stmt, sqlite3 *db,
                                       const char *table, crsql_ColumnInfo **pks,
                                       int pks_len, crsql_ColumnInfo **non_pks,
                                       int non_pks_len, int is_commit_alter) {
  // Prepare select key statement
  char *where_list = crsql_where_list(pks, pks_len, NULL);
  char *escaped_table = crsql_escape_ident(table);

  char *sql = sqlite3_mprintf(
    "SELECT __crsql_key FROM \"%s__crsql_pks\" WHERE %s",
    escaped_table, where_list
  );

  sqlite3_free(where_list);

  sqlite3_stmt *select_key_stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &select_key_stmt, NULL);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    sqlite3_free(escaped_table);
    return rc;
  }

  // Prepare create key statement
  char *pk_list = crsql_as_identifier_list(pks, pks_len, NULL);
  char *binding_list = crsql_binding_list(pks_len);

  sql = sqlite3_mprintf(
    "INSERT INTO \"%s__crsql_pks\" (%s) VALUES (%s) RETURNING __crsql_key",
    escaped_table, pk_list, binding_list
  );

  sqlite3_free(pk_list);
  sqlite3_free(binding_list);

  sqlite3_stmt *create_key_stmt = NULL;
  rc = sqlite3_prepare_v2(db, sql, -1, &create_key_stmt, NULL);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    sqlite3_finalize(select_key_stmt);
    sqlite3_free(escaped_table);
    return rc;
  }

  // Prepare write statement
  const char *dbversion_getter = is_commit_alter ? "crsql_db_version()" : "crsql_next_db_version()";
  sql = sqlite3_mprintf(
    "INSERT OR IGNORE INTO \"%s__crsql_clock\" "
    "(key, col_name, col_version, db_version, seq) VALUES "
    "(?, ?, 1, %s, crsql_increment_and_get_seq())",
    escaped_table, dbversion_getter
  );

  sqlite3_free(escaped_table);

  sqlite3_stmt *write_stmt = NULL;
  rc = sqlite3_prepare_v2(db, sql, -1, &write_stmt, NULL);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    sqlite3_finalize(select_key_stmt);
    sqlite3_finalize(create_key_stmt);
    return rc;
  }

  // Process each row
  while ((rc = sqlite3_step(read_stmt)) == SQLITE_ROW) {
    sqlite3_int64 key = get_or_create_key(select_key_stmt, create_key_stmt,
                                          pks, pks_len, read_stmt);
    if (key < 0) {
      sqlite3_finalize(select_key_stmt);
      sqlite3_finalize(create_key_stmt);
      sqlite3_finalize(write_stmt);
      return SQLITE_ERROR;
    }

    sqlite3_bind_int64(write_stmt, 1, key);

    if (non_pks_len == 0) {
      // No non-pk columns, insert sentinel
      sqlite3_bind_text(write_stmt, 2, "-1", -1, SQLITE_STATIC);
      sqlite3_step(write_stmt);
      sqlite3_reset(write_stmt);
    } else {
      // Insert clock entry for each non-pk column
      for (int i = 0; i < non_pks_len; i++) {
        sqlite3_bind_text(write_stmt, 2, non_pks[i]->name, -1, SQLITE_STATIC);
        sqlite3_step(write_stmt);
        sqlite3_reset(write_stmt);
      }
    }
  }

  sqlite3_finalize(select_key_stmt);
  sqlite3_finalize(create_key_stmt);
  sqlite3_finalize(write_stmt);

  return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

// Get or create key in __crsql_pks
static sqlite3_int64 get_or_create_key(sqlite3_stmt *select_stmt, sqlite3_stmt *create_stmt,
                                       crsql_ColumnInfo **pks, int pks_len,
                                       sqlite3_stmt *read_stmt) {
  // Bind PK values to both statements
  for (int i = 0; i < pks_len; i++) {
    sqlite3_value *value = sqlite3_column_value(read_stmt, i);
    sqlite3_bind_value(select_stmt, i + 1, value);
    sqlite3_bind_value(create_stmt, i + 1, value);
  }

  // Try to select existing key
  if (sqlite3_step(select_stmt) == SQLITE_ROW) {
    sqlite3_int64 key = sqlite3_column_int64(select_stmt, 0);
    sqlite3_clear_bindings(create_stmt);
    sqlite3_reset(select_stmt);
    return key;
  }

  sqlite3_reset(select_stmt);

  // Create new key
  if (sqlite3_step(create_stmt) == SQLITE_ROW) {
    sqlite3_int64 key = sqlite3_column_int64(create_stmt, 0);
    sqlite3_reset(create_stmt);
    return key;
  }

  sqlite3_reset(create_stmt);
  return -1;
}

// Backfill missing columns
static int backfill_missing_columns(sqlite3 *db, const char *table,
                                   crsql_ColumnInfo **pks, int pks_len,
                                   crsql_ColumnInfo **non_pks, int non_pks_len,
                                   int is_commit_alter) {
  for (int i = 0; i < non_pks_len; i++) {
    int rc = fill_column(db, table, pks, pks_len, non_pks[i], is_commit_alter);
    if (rc != SQLITE_OK) return rc;
  }

  return SQLITE_OK;
}

// Fill a specific column
static int fill_column(sqlite3 *db, const char *table, crsql_ColumnInfo **pks,
                      int pks_len, crsql_ColumnInfo *non_pk_col, int is_commit_alter) {
  char *dflt_value = crsql_get_dflt_value(db, table, non_pk_col->name);
  char *escaped_table = crsql_escape_ident(table);

  // Build pk columns list with t1 prefix
  char **pk_cols_t1 = sqlite3_malloc(sizeof(char*) * pks_len);
  for (int i = 0; i < pks_len; i++) {
    char *escaped = crsql_escape_ident(pks[i]->name);
    pk_cols_t1[i] = sqlite3_mprintf("t1.\"%s\"", escaped);
    sqlite3_free(escaped);
  }

  // Join pk columns
  char *pk_list = sqlite3_malloc(1);
  pk_list[0] = '\0';
  for (int i = 0; i < pks_len; i++) {
    char *new_list;
    if (i == 0) {
      new_list = sqlite3_mprintf("%s", pk_cols_t1[i]);
    } else {
      new_list = sqlite3_mprintf("%s, %s", pk_list, pk_cols_t1[i]);
    }
    sqlite3_free(pk_list);
    pk_list = new_list;
  }

  // Build ON conditions
  char *on_conditions = sqlite3_malloc(1);
  on_conditions[0] = '\0';
  for (int i = 0; i < pks_len; i++) {
    char *escaped = crsql_escape_ident(pks[i]->name);
    char *new_cond;
    if (i == 0) {
      new_cond = sqlite3_mprintf("t1.\"%s\" = t2.\"%s\"", escaped, escaped);
    } else {
      new_cond = sqlite3_mprintf("%s AND t1.\"%s\" = t2.\"%s\"", on_conditions, escaped, escaped);
    }
    sqlite3_free(escaped);
    sqlite3_free(on_conditions);
    on_conditions = new_cond;
  }

  // Build default value condition
  char *dflt_condition = "";
  if (dflt_value) {
    char *escaped_col = crsql_escape_ident(non_pk_col->name);
    dflt_condition = sqlite3_mprintf(" AND t1.\"%s\" IS NOT %s", escaped_col, dflt_value);
    sqlite3_free(escaped_col);
    sqlite3_free(dflt_value);
  }

  char *sql = sqlite3_mprintf(
    "SELECT %s FROM \"%s\" as t1 "
    "JOIN \"%s__crsql_pks\" as t2 ON %s "
    "LEFT JOIN \"%s__crsql_clock\" as t3 ON t3.key = t2.__crsql_key AND t3.col_name = ? "
    "WHERE t3.key IS NULL%s",
    pk_list, escaped_table, escaped_table, on_conditions, escaped_table, dflt_condition
  );

  // Free temp arrays
  for (int i = 0; i < pks_len; i++) sqlite3_free(pk_cols_t1[i]);
  sqlite3_free(pk_cols_t1);
  sqlite3_free(pk_list);
  sqlite3_free(on_conditions);
  sqlite3_free(escaped_table);
  if (dflt_value) sqlite3_free(dflt_condition);

  sqlite3_stmt *read_stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &read_stmt, NULL);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) return rc;

  rc = sqlite3_bind_text(read_stmt, 1, non_pk_col->name, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(read_stmt);
    return rc;
  }

  // Create clock rows for this column
  crsql_ColumnInfo *cols_array[1] = {non_pk_col};
  rc = create_clock_rows_from_stmt(read_stmt, db, table, pks, pks_len,
                                   cols_array, 1, is_commit_alter);
  sqlite3_finalize(read_stmt);

  return rc;
}
