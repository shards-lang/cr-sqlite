#include "teardown.h"
#include "str_util.h"

// Remove clock tables
int crsql_remove_crr_clock_table_if_exists(sqlite3 *db, const char *table) {
  char *escaped = crsql_escape_ident(table);
  if (!escaped) return SQLITE_NOMEM;

  char *sql = sqlite3_mprintf("DROP TABLE IF EXISTS \"%s__crsql_clock\"", escaped);
  sqlite3_free(escaped);

  if (!sql) return SQLITE_NOMEM;

  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  sqlite3_free(sql);
  if (err) sqlite3_free(err);

  if (rc != SQLITE_OK) return rc;

  escaped = crsql_escape_ident(table);
  if (!escaped) return SQLITE_NOMEM;

  sql = sqlite3_mprintf("DROP TABLE IF EXISTS \"%s__crsql_pks\"", escaped);
  sqlite3_free(escaped);

  if (!sql) return SQLITE_NOMEM;

  rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  sqlite3_free(sql);
  if (err) sqlite3_free(err);

  return rc;
}

// Remove triggers
int crsql_remove_crr_triggers_if_exist(sqlite3 *db, const char *table) {
  char *escaped = crsql_escape_ident(table);
  if (!escaped) return SQLITE_NOMEM;

  // Drop insert trigger
  char *sql = sqlite3_mprintf("DROP TRIGGER IF EXISTS \"%s__crsql_itrig\"", escaped);
  if (!sql) {
    sqlite3_free(escaped);
    return SQLITE_NOMEM;
  }

  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  sqlite3_free(sql);
  if (err) sqlite3_free(err);
  if (rc != SQLITE_OK) {
    sqlite3_free(escaped);
    return rc;
  }

  // Drop update trigger
  sql = sqlite3_mprintf("DROP TRIGGER IF EXISTS \"%s__crsql_utrig\"", escaped);
  if (!sql) {
    sqlite3_free(escaped);
    return SQLITE_NOMEM;
  }

  rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  sqlite3_free(sql);
  if (err) sqlite3_free(err);
  if (rc != SQLITE_OK) {
    sqlite3_free(escaped);
    return rc;
  }

  // Drop per-PK-column triggers
  sqlite3_stmt *stmt = NULL;
  rc = sqlite3_prepare_v2(db,
    "SELECT name FROM pragma_table_info(?) WHERE pk > 0",
    -1, &stmt, NULL);

  if (rc != SQLITE_OK) {
    sqlite3_free(escaped);
    return rc;
  }

  rc = sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt);
    sqlite3_free(escaped);
    return rc;
  }

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *col_name = (const char *)sqlite3_column_text(stmt, 0);
    char *escaped_col = crsql_escape_ident(col_name);

    if (escaped_col) {
      char *trigger_sql = sqlite3_mprintf(
        "DROP TRIGGER IF EXISTS \"%s_%s__crsql_utrig\"",
        escaped, escaped_col
      );

      if (trigger_sql) {
        sqlite3_exec(db, trigger_sql, NULL, NULL, NULL);
        sqlite3_free(trigger_sql);
      }

      sqlite3_free(escaped_col);
    }
  }

  sqlite3_finalize(stmt);

  // Drop delete trigger
  sql = sqlite3_mprintf("DROP TRIGGER IF EXISTS \"%s__crsql_dtrig\"", escaped);
  sqlite3_free(escaped);

  if (!sql) return SQLITE_NOMEM;

  rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  sqlite3_free(sql);
  if (err) sqlite3_free(err);

  return rc;
}
