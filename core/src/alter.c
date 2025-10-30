#include "alter.h"
#include "str_util.h"
#include "consts.h"
#include "db_version.h"
#include "tableinfo.h"
#include <string.h>

// Compact clock tables after schema alterations
int crsql_compact_post_alter(sqlite3 *db, const char *tbl_name,
                             crsql_ExtData *ext_data, char **errmsg) {
  // Ensure db version is filled
  int rc = crsql_fill_db_version_if_needed(db, ext_data, errmsg);
  if (rc != SQLITE_OK) return rc;

  sqlite3_int64 current_db_version = ext_data->dbVersion;

  // Check if primary key columns changed by comparing:
  // - PKs from pragma_table_info(table)
  // - PKs from pragma_index_info(table__crsql_pks_pks)
  char *escaped_val = crsql_escape_ident_as_value(tbl_name);
  char *sql = sqlite3_mprintf(
    "SELECT count(name) FROM ("
    "  SELECT name FROM pragma_table_info('%s') "
    "    WHERE pk > 0 AND name NOT IN "
    "      (SELECT name FROM pragma_index_info('%s__crsql_pks_pks')) "
    "  UNION SELECT name FROM pragma_index_info('%s__crsql_pks_pks') "
    "    WHERE name NOT IN "
    "      (SELECT name FROM pragma_table_info('%s') WHERE pk > 0) "
    "      AND name != 'col_name'"
    ")",
    escaped_val, escaped_val, escaped_val, escaped_val
  );
  sqlite3_free(escaped_val);

  if (!sql) {
    if (errmsg) *errmsg = sqlite3_mprintf("Out of memory");
    return SQLITE_NOMEM;
  }

  sqlite3_stmt *stmt = NULL;
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    if (errmsg) *errmsg = sqlite3_mprintf("Failed to check PK changes");
    return rc;
  }

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    if (errmsg) *errmsg = sqlite3_mprintf("Failed to check PK changes");
    return SQLITE_ERROR;
  }

  int pk_diff = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if (pk_diff > 0) {
    // Primary keys changed - drop and recreate clock tables
    char *escaped_ident = crsql_escape_ident(tbl_name);
    sql = sqlite3_mprintf(
      "DROP TABLE \"%s__crsql_clock\"; "
      "DROP TABLE \"%s__crsql_pks\";",
      escaped_ident, escaped_ident
    );
    sqlite3_free(escaped_ident);

    if (!sql) {
      if (errmsg) *errmsg = sqlite3_mprintf("Out of memory");
      return SQLITE_NOMEM;
    }

    char *exec_err = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &exec_err);
    sqlite3_free(sql);

    if (rc != SQLITE_OK) {
      if (errmsg) *errmsg = sqlite3_mprintf("Failed to drop clock tables: %s",
                                            exec_err ? exec_err : "unknown");
      if (exec_err) sqlite3_free(exec_err);
      return rc;
    }
  } else {
    // Primary keys same - compact clock table

    // 1. Delete entries for columns that no longer exist
    char *escaped_ident = crsql_escape_ident(tbl_name);
    char *escaped_val2 = crsql_escape_ident_as_value(tbl_name);
    sql = sqlite3_mprintf(
      "DELETE FROM \"%s__crsql_clock\" WHERE \"col_name\" NOT IN ("
      "  SELECT name FROM pragma_table_info('%s') "
      "  UNION SELECT '%s'"
      ")",
      escaped_ident, escaped_val2, CL_SENTINEL
    );
    sqlite3_free(escaped_val2);

    if (!sql) {
      sqlite3_free(escaped_ident);
      if (errmsg) *errmsg = sqlite3_mprintf("Out of memory");
      return SQLITE_NOMEM;
    }

    char *exec_err = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &exec_err);
    sqlite3_free(sql);

    if (rc != SQLITE_OK) {
      sqlite3_free(escaped_ident);
      if (errmsg) *errmsg = sqlite3_mprintf("Failed to delete obsolete columns: %s",
                                            exec_err ? exec_err : "unknown");
      if (exec_err) sqlite3_free(exec_err);
      return rc;
    }

    // 2. Delete entries for rows that no longer exist (but keep tombstones)
    // Tombstones are sentinel entries with col_name = '-1' and even col_version

    // First, ensure table infos are up to date
    rc = crsql_ensure_table_infos_are_up_to_date(db, ext_data, errmsg);
    if (rc != SQLITE_OK) {
      sqlite3_free(escaped_ident);
      return rc;
    }

    // Find the table info for this table
    crsql_TableInfo *table_info = NULL;
    for (int i = 0; i < ext_data->tableInfosLen; i++) {
      if (strcmp(ext_data->tableInfos[i]->tbl_name, tbl_name) == 0) {
        table_info = ext_data->tableInfos[i];
        break;
      }
    }

    if (!table_info) {
      sqlite3_free(escaped_ident);
      if (errmsg) *errmsg = sqlite3_mprintf("Table info not found for %s", tbl_name);
      return SQLITE_ERROR;
    }

    // Build SQL: DELETE FROM clock WHERE (not tombstone) AND NOT EXISTS (row join)
    // Start with base query
    char *base_sql = sqlite3_mprintf(
      "DELETE FROM \"%s__crsql_clock\" "
      "WHERE (col_name != '-1' OR (col_name = '-1' AND col_version %% 2 != 0)) "
      "AND NOT EXISTS (SELECT 1 FROM \"%s\" JOIN \"%s__crsql_pks\" ON ",
      escaped_ident, escaped_ident, escaped_ident
    );

    if (!base_sql) {
      sqlite3_free(escaped_ident);
      if (errmsg) *errmsg = sqlite3_mprintf("Out of memory");
      return SQLITE_NOMEM;
    }

    // Build join conditions for each PK column
    char *join_conditions = sqlite3_malloc(1);
    join_conditions[0] = '\0';

    for (int i = 0; i < table_info->pks_len; i++) {
      char *col_escaped = crsql_escape_ident(table_info->pks[i]->name);
      char *new_cond;

      if (i == 0) {
        new_cond = sqlite3_mprintf(
          "\"%s\".\"%s\" = \"%s__crsql_pks\".\"%s\"",
          escaped_ident, col_escaped, escaped_ident, col_escaped
        );
      } else {
        new_cond = sqlite3_mprintf(
          "%s AND \"%s\".\"%s\" = \"%s__crsql_pks\".\"%s\"",
          join_conditions, escaped_ident, col_escaped, escaped_ident, col_escaped
        );
      }

      sqlite3_free(col_escaped);
      sqlite3_free(join_conditions);
      join_conditions = new_cond;

      if (!join_conditions) {
        sqlite3_free(base_sql);
        sqlite3_free(escaped_ident);
        if (errmsg) *errmsg = sqlite3_mprintf("Out of memory");
        return SQLITE_NOMEM;
      }
    }

    // Complete the SQL
    sql = sqlite3_mprintf(
      "%s%s WHERE \"%s__crsql_clock\".key = \"%s__crsql_pks\".__crsql_key LIMIT 1)",
      base_sql, join_conditions, escaped_ident, escaped_ident
    );

    sqlite3_free(base_sql);
    sqlite3_free(join_conditions);

    if (!sql) {
      sqlite3_free(escaped_ident);
      if (errmsg) *errmsg = sqlite3_mprintf("Out of memory");
      return SQLITE_NOMEM;
    }

    exec_err = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &exec_err);
    sqlite3_free(sql);

    if (rc != SQLITE_OK) {
      sqlite3_free(escaped_ident);
      if (errmsg) *errmsg = sqlite3_mprintf("Failed to delete orphaned rows: %s",
                                            exec_err ? exec_err : "unknown");
      if (exec_err) sqlite3_free(exec_err);
      return rc;
    }

    // 3. Delete orphaned pk lookasides
    sql = sqlite3_mprintf(
      "DELETE FROM \"%s__crsql_pks\" "
      "WHERE __crsql_key NOT IN (SELECT key FROM \"%s__crsql_clock\")",
      escaped_ident, escaped_ident
    );

    sqlite3_free(escaped_ident);

    if (!sql) {
      if (errmsg) *errmsg = sqlite3_mprintf("Out of memory");
      return SQLITE_NOMEM;
    }

    exec_err = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &exec_err);
    sqlite3_free(sql);

    if (rc != SQLITE_OK) {
      if (errmsg) *errmsg = sqlite3_mprintf("Failed to delete orphaned pks: %s",
                                            exec_err ? exec_err : "unknown");
      if (exec_err) sqlite3_free(exec_err);
      return rc;
    }
  }

  // Save pre_compact_dbversion
  stmt = NULL;
  rc = sqlite3_prepare_v2(db,
    "INSERT OR REPLACE INTO crsql_master (key, value) VALUES ('pre_compact_dbversion', ?)",
    -1, &stmt, NULL);

  if (rc != SQLITE_OK) {
    if (errmsg) *errmsg = sqlite3_mprintf("Failed to save pre_compact_dbversion");
    return rc;
  }

  rc = sqlite3_bind_int64(stmt, 1, current_db_version);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt);
    if (errmsg) *errmsg = sqlite3_mprintf("Failed to bind pre_compact_dbversion");
    return rc;
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    if (errmsg) *errmsg = sqlite3_mprintf("Failed to insert pre_compact_dbversion");
    return SQLITE_ERROR;
  }

  return SQLITE_OK;
}
