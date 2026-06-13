#include "crr.h"

#include <string.h>

#include "consts.h"
#include "tableinfo.h"
#include "triggers.h"
#include "util.h"

// Forward declarations from bootstrap.h
int crsql_is_crr(sqlite3 *db, const char *table);

/* ------------------------------------------------------------------ */
/*  create_clock_table                                                 */
/* ------------------------------------------------------------------ */

int crsql_create_clock_table(sqlite3 *db, crsql_TableInfo *tableInfo,
                             char **errmsg) {
  char *escName = crsql_escape_ident(tableInfo->tblName);
  char *pkList =
      crsql_as_identifier_list(tableInfo->pks, tableInfo->pksLen, NULL);
  if (!escName || !pkList) {
    sqlite3_free(escName);
    sqlite3_free(pkList);
    return SQLITE_ERROR;
  }

  char *sql;
  int rc;

  // Create clock table
  sql = sqlite3_mprintf(
      "CREATE TABLE IF NOT EXISTS \"%s__crsql_clock\" ("
      "key INTEGER NOT NULL,"
      "col_name TEXT NOT NULL,"
      "col_version INTEGER NOT NULL,"
      "db_version INTEGER NOT NULL,"
      "site_id INTEGER NOT NULL DEFAULT 0,"
      "seq INTEGER NOT NULL,"
      "PRIMARY KEY (key, col_name)"
      ") WITHOUT ROWID, STRICT",
      escName);
  if (!sql) {
    sqlite3_free(escName);
    sqlite3_free(pkList);
    return SQLITE_NOMEM;
  }
  rc = sqlite3_exec(db, sql, 0, 0, errmsg);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    sqlite3_free(escName);
    sqlite3_free(pkList);
    return rc;
  }

  // Create db_version index
  sql = sqlite3_mprintf(
      "CREATE INDEX IF NOT EXISTS \"%s__crsql_clock_dbv_idx\""
      " ON \"%s__crsql_clock\" (\"db_version\")",
      escName, escName);
  if (!sql) {
    sqlite3_free(escName);
    sqlite3_free(pkList);
    return SQLITE_NOMEM;
  }
  rc = sqlite3_exec(db, sql, 0, 0, errmsg);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    sqlite3_free(escName);
    sqlite3_free(pkList);
    return rc;
  }

  // Create pks table
  sql = sqlite3_mprintf(
      "CREATE TABLE IF NOT EXISTS \"%w__crsql_pks\""
      " (__crsql_key INTEGER PRIMARY KEY, %s)",
      tableInfo->tblName, pkList);
  if (!sql) {
    sqlite3_free(escName);
    sqlite3_free(pkList);
    return SQLITE_NOMEM;
  }
  rc = sqlite3_exec(db, sql, 0, 0, errmsg);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    sqlite3_free(escName);
    sqlite3_free(pkList);
    return rc;
  }

  // Create unique index on pks
  sql = sqlite3_mprintf(
      "CREATE UNIQUE INDEX IF NOT EXISTS \"%w__crsql_pks_pks\""
      " ON \"%w__crsql_pks\" (%s)",
      tableInfo->tblName, tableInfo->tblName, pkList);
  sqlite3_free(escName);
  sqlite3_free(pkList);
  if (!sql) return SQLITE_NOMEM;
  rc = sqlite3_exec(db, sql, 0, 0, errmsg);
  sqlite3_free(sql);
  return rc;
}

/* ------------------------------------------------------------------ */
/*  backfill helpers                                                    */
/* ------------------------------------------------------------------ */

/**
 * Get or create a key in __crsql_pks for the given primary key values
 * read from read_stmt columns [0..numPks).
 * Returns key >= 0 on success, < 0 on error.
 */
static sqlite3_int64 backfill_get_or_create_key(sqlite3 *db,
                                                sqlite3_stmt *selectKeyStmt,
                                                sqlite3_stmt *createKeyStmt,
                                                int numPks,
                                                sqlite3_stmt *readStmt) {
  for (int i = 0; i < numPks; i++) {
    sqlite3_value *val = sqlite3_column_value(readStmt, i);
    sqlite3_bind_value(selectKeyStmt, i + 1, val);
    sqlite3_bind_value(createKeyStmt, i + 1, val);
  }

  sqlite3_int64 key = -1;
  if (sqlite3_step(selectKeyStmt) == SQLITE_ROW) {
    key = sqlite3_column_int64(selectKeyStmt, 0);
    sqlite3_clear_bindings(createKeyStmt);
    sqlite3_reset(selectKeyStmt);
    return key;
  }
  sqlite3_reset(selectKeyStmt);

  // __crsql_key is an INTEGER PRIMARY KEY (rowid alias)
  if (sqlite3_step(createKeyStmt) == SQLITE_DONE) {
    key = sqlite3_last_insert_rowid(db);
    sqlite3_reset(createKeyStmt);
    return key;
  }
  sqlite3_reset(createKeyStmt);

  return -1;
}

/**
 * Given a statement that returns rows in the source table not present
 * in the clock table, create clock table rows for them.
 */
static int create_clock_rows_from_stmt(sqlite3_stmt *readStmt, sqlite3 *db,
                                       const char *table,
                                       crsql_ColumnInfo *pkCols, int numPks,
                                       crsql_ColumnInfo *nonPkCols,
                                       int numNonPks, int isCommitAlter) {
  char *escTable = crsql_escape_ident(table);
  char *whereList = crsql_where_list(pkCols, numPks, NULL);
  char *pkColList = crsql_as_identifier_list(pkCols, numPks, NULL);
  char *pkBindings = crsql_binding_list(numPks);
  if (!escTable || !whereList || !pkColList || !pkBindings) {
    sqlite3_free(escTable);
    sqlite3_free(whereList);
    sqlite3_free(pkColList);
    sqlite3_free(pkBindings);
    return SQLITE_ERROR;
  }

  // Prepare select key stmt
  char *selectKeySql = sqlite3_mprintf(
      "SELECT __crsql_key FROM \"%s__crsql_pks\" WHERE %s", escTable,
      whereList);
  sqlite3_free(whereList);
  if (!selectKeySql) {
    sqlite3_free(escTable);
    sqlite3_free(pkColList);
    sqlite3_free(pkBindings);
    return SQLITE_NOMEM;
  }
  sqlite3_stmt *selectKeyStmt = 0;
  int rc = sqlite3_prepare_v2(db, selectKeySql, -1, &selectKeyStmt, 0);
  sqlite3_free(selectKeySql);
  if (rc != SQLITE_OK) {
    sqlite3_free(escTable);
    sqlite3_free(pkColList);
    sqlite3_free(pkBindings);
    return rc;
  }

  // Prepare create key stmt
  char *createKeySql = sqlite3_mprintf(
      "INSERT INTO \"%s__crsql_pks\" (%s) VALUES (%s)",
      escTable, pkColList, pkBindings);
  sqlite3_free(pkColList);
  sqlite3_free(pkBindings);
  if (!createKeySql) {
    sqlite3_free(escTable);
    sqlite3_finalize(selectKeyStmt);
    return SQLITE_NOMEM;
  }
  sqlite3_stmt *createKeyStmt = 0;
  rc = sqlite3_prepare_v2(db, createKeySql, -1, &createKeyStmt, 0);
  sqlite3_free(createKeySql);
  if (rc != SQLITE_OK) {
    sqlite3_free(escTable);
    sqlite3_finalize(selectKeyStmt);
    return rc;
  }

  // Prepare write stmt
  const char *dbversionGetter =
      isCommitAlter ? "crsql_db_version()" : "crsql_next_db_version()";
  char *writeSql = sqlite3_mprintf(
      "INSERT OR IGNORE INTO \"%s__crsql_clock\""
      " (key, col_name, col_version, db_version, seq)"
      " VALUES (?, ?, 1, %s, crsql_increment_and_get_seq())",
      escTable, dbversionGetter);
  sqlite3_free(escTable);
  if (!writeSql) {
    sqlite3_finalize(selectKeyStmt);
    sqlite3_finalize(createKeyStmt);
    return SQLITE_NOMEM;
  }
  sqlite3_stmt *writeStmt = 0;
  rc = sqlite3_prepare_v2(db, writeSql, -1, &writeStmt, 0);
  sqlite3_free(writeSql);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(selectKeyStmt);
    sqlite3_finalize(createKeyStmt);
    return rc;
  }

  // Iterate rows
  while (sqlite3_step(readStmt) == SQLITE_ROW) {
    sqlite3_int64 key = backfill_get_or_create_key(db, selectKeyStmt,
                                                   createKeyStmt, numPks,
                                                   readStmt);
    if (key < 0) {
      rc = SQLITE_ERROR;
      break;
    }
    sqlite3_bind_int64(writeStmt, 1, key);

    if (numNonPks > 0) {
      for (int i = 0; i < numNonPks; i++) {
        sqlite3_bind_text(writeStmt, 2, nonPkCols[i].name, -1, SQLITE_STATIC);
        sqlite3_step(writeStmt);
        sqlite3_reset(writeStmt);
      }
    } else {
      sqlite3_bind_text(writeStmt, 2, SENTINEL_CID, -1, SQLITE_STATIC);
      sqlite3_step(writeStmt);
      sqlite3_reset(writeStmt);
    }
  }

  sqlite3_finalize(selectKeyStmt);
  sqlite3_finalize(createKeyStmt);
  sqlite3_finalize(writeStmt);
  return rc;
}

/**
 * For a single column, fill in missing clock entries for rows that
 * have values differing from the default.
 */
static int fill_column(sqlite3 *db, const char *table, crsql_ColumnInfo *pkCols,
                       int numPks, crsql_ColumnInfo *nonPkCol,
                       int isCommitAlter) {
  char *escTable = crsql_escape_ident(table);
  if (!escTable) return SQLITE_ERROR;

  // Build pk column list with t1 prefix
  char *pkSelectList = 0;
  {
    int totalLen = 0;
    for (int i = 0; i < numPks; i++) {
      char *esc = crsql_escape_ident(pkCols[i].name);
      if (!esc) {
        sqlite3_free(escTable);
        return SQLITE_ERROR;
      }
      // t1."col" + comma + space
      totalLen += (int)strlen(esc) + 6;  // t1."" + ,_
      sqlite3_free(esc);
    }
    pkSelectList = sqlite3_malloc(totalLen + 1);
    if (!pkSelectList) {
      sqlite3_free(escTable);
      return SQLITE_NOMEM;
    }
    pkSelectList[0] = 0;
    for (int i = 0; i < numPks; i++) {
      char *esc = crsql_escape_ident(pkCols[i].name);
      if (i > 0) strcat(pkSelectList, ", ");
      strcat(pkSelectList, "t1.\"");
      strcat(pkSelectList, esc);
      strcat(pkSelectList, "\"");
      sqlite3_free(esc);
    }
  }

  // Build pk ON conditions
  char *pkOnConditions = 0;
  {
    int totalLen = 0;
    for (int i = 0; i < numPks; i++) {
      char *esc = crsql_escape_ident(pkCols[i].name);
      if (!esc) {
        sqlite3_free(escTable);
        sqlite3_free(pkSelectList);
        return SQLITE_ERROR;
      }
      // t1."col" = t2."col" AND
      totalLen += (int)strlen(esc) * 2 + 20;
      sqlite3_free(esc);
    }
    pkOnConditions = sqlite3_malloc(totalLen + 1);
    if (!pkOnConditions) {
      sqlite3_free(escTable);
      sqlite3_free(pkSelectList);
      return SQLITE_NOMEM;
    }
    pkOnConditions[0] = 0;
    for (int i = 0; i < numPks; i++) {
      char *esc = crsql_escape_ident(pkCols[i].name);
      if (i > 0) strcat(pkOnConditions, " AND ");
      strcat(pkOnConditions, "t1.\"");
      strcat(pkOnConditions, esc);
      strcat(pkOnConditions, "\" = t2.\"");
      strcat(pkOnConditions, esc);
      strcat(pkOnConditions, "\"");
      sqlite3_free(esc);
    }
  }

  // Get default value for this column
  char *dfltValue = 0;
  int rc = crsql_get_dflt_value(db, table, nonPkCol->name, &dfltValue);
  if (rc != SQLITE_OK) {
    sqlite3_free(escTable);
    sqlite3_free(pkSelectList);
    sqlite3_free(pkOnConditions);
    return rc;
  }

  char *dfltCondition;
  if (dfltValue) {
    dfltCondition =
        sqlite3_mprintf("AND t1.\"%s\" IS NOT %s", nonPkCol->name, dfltValue);
    sqlite3_free(dfltValue);
  } else {
    dfltCondition = sqlite3_mprintf("%s", "");
  }
  if (!dfltCondition) {
    sqlite3_free(escTable);
    sqlite3_free(pkSelectList);
    sqlite3_free(pkOnConditions);
    return SQLITE_NOMEM;
  }

  char *sql = sqlite3_mprintf(
      "SELECT %s FROM \"%s\" as t1"
      " JOIN \"%s__crsql_pks\" as t2 ON %s"
      " LEFT JOIN \"%s__crsql_clock\" as t3"
      " ON t3.key = t2.__crsql_key AND t3.col_name = ?"
      " WHERE t3.key IS NULL %s",
      pkSelectList, escTable, escTable, pkOnConditions, escTable,
      dfltCondition);
  sqlite3_free(pkSelectList);
  sqlite3_free(pkOnConditions);
  sqlite3_free(dfltCondition);
  if (!sql) {
    sqlite3_free(escTable);
    return SQLITE_NOMEM;
  }

  sqlite3_stmt *readStmt = 0;
  rc = sqlite3_prepare_v2(db, sql, -1, &readStmt, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    sqlite3_free(escTable);
    return rc;
  }
  sqlite3_bind_text(readStmt, 1, nonPkCol->name, -1, SQLITE_STATIC);

  sqlite3_free(escTable);
  rc = create_clock_rows_from_stmt(readStmt, db, table, pkCols, numPks,
                                   nonPkCol, 1, isCommitAlter);
  sqlite3_finalize(readStmt);
  return rc;
}

static int backfill_missing_columns(sqlite3 *db, const char *table,
                                    crsql_ColumnInfo *pkCols, int numPks,
                                    crsql_ColumnInfo *nonPkCols, int numNonPks,
                                    int isCommitAlter) {
  for (int i = 0; i < numNonPks; i++) {
    int rc = fill_column(db, table, pkCols, numPks, &nonPkCols[i],
                         isCommitAlter);
    if (rc != SQLITE_OK) return rc;
  }
  return SQLITE_OK;
}

/* ------------------------------------------------------------------ */
/*  backfill_table                                                     */
/* ------------------------------------------------------------------ */

int crsql_backfill_table(sqlite3 *db, const char *tblName,
                         crsql_ColumnInfo *pkCols, int numPks,
                         crsql_ColumnInfo *nonPkCols, int numNonPks,
                         int isCommitAlter, int noTx) {
  int rc;

  if (!noTx) {
    rc = sqlite3_exec(db, "SAVEPOINT backfill", 0, 0, 0);
    if (rc != SQLITE_OK) return rc;
  }

  // Build pk column list for the SELECT...EXCEPT query
  char *pkColList = crsql_as_identifier_list(pkCols, numPks, NULL);
  char *escTable = crsql_escape_ident(tblName);
  if (!pkColList || !escTable) {
    sqlite3_free(pkColList);
    sqlite3_free(escTable);
    if (!noTx) sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    return SQLITE_ERROR;
  }

  char *sql = sqlite3_mprintf(
      "SELECT %s FROM \"%s\" AS t1"
      " EXCEPT SELECT %s FROM \"%s__crsql_pks\" AS t2",
      pkColList, escTable, pkColList, escTable);
  sqlite3_free(pkColList);
  sqlite3_free(escTable);
  if (!sql) {
    if (!noTx) sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    return SQLITE_NOMEM;
  }

  sqlite3_stmt *readStmt = 0;
  rc = sqlite3_prepare_v2(db, sql, -1, &readStmt, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    if (!noTx) sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    return rc;
  }

  rc = create_clock_rows_from_stmt(readStmt, db, tblName, pkCols, numPks,
                                   nonPkCols, numNonPks, isCommitAlter);
  sqlite3_finalize(readStmt);
  if (rc != SQLITE_OK) {
    if (!noTx) sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    return rc;
  }

  rc = backfill_missing_columns(db, tblName, pkCols, numPks, nonPkCols,
                                numNonPks, isCommitAlter);
  if (rc != SQLITE_OK) {
    if (!noTx) sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    return rc;
  }

  if (!noTx) {
    return sqlite3_exec(db, "RELEASE backfill", 0, 0, 0);
  }
  return SQLITE_OK;
}

/* ------------------------------------------------------------------ */
/*  create_crr                                                         */
/* ------------------------------------------------------------------ */

int crsql_create_crr(sqlite3 *db, const char *schemaName, const char *tblName,
                     int isCommitAlter, int noTx, char **errmsg) {
  int rc;

  rc = crsql_is_table_compatible(db, tblName, errmsg);
  if (rc != 1) {
    // 0 = not compatible, -1 = error
    return SQLITE_ERROR;
  }

  rc = crsql_is_crr(db, tblName);
  if (rc == 1) {
    return SQLITE_OK;  // Already a CRR
  }
  if (rc < 0) {
    return SQLITE_ERROR;
  }

  crsql_TableInfo *tableInfo = 0;
  rc = crsql_pull_table_info(db, tblName, &tableInfo, errmsg);
  if (rc != SQLITE_OK) return rc;

  rc = crsql_create_clock_table(db, tableInfo, errmsg);
  if (rc != SQLITE_OK) {
    crsql_free_table_info(tableInfo);
    return rc;
  }

  rc = crsql_remove_crr_triggers_if_exist(db, tblName);
  if (rc != SQLITE_OK) {
    crsql_free_table_info(tableInfo);
    return rc;
  }

  rc = crsql_create_triggers(db, tableInfo, errmsg);
  if (rc != SQLITE_OK) {
    crsql_free_table_info(tableInfo);
    return rc;
  }

  rc = crsql_backfill_table(db, tblName, tableInfo->pks, tableInfo->pksLen,
                            tableInfo->nonPks, tableInfo->nonPksLen,
                            isCommitAlter, noTx);
  crsql_free_table_info(tableInfo);
  return rc;
}

/* ------------------------------------------------------------------ */
/*  config_get / config_set                                            */
/* ------------------------------------------------------------------ */

#define MERGE_EQUAL_VALUES "merge-equal-values"

void crsql_config_set(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  if (argc < 2) {
    sqlite3_result_error(ctx, "config_set requires 2 arguments", -1);
    return;
  }

  const char *name = (const char *)sqlite3_value_text(argv[0]);
  if (!name) {
    sqlite3_result_error(ctx, "config_set: name must not be null", -1);
    return;
  }

  if (strcmp(name, MERGE_EQUAL_VALUES) == 0) {
    crsql_ExtData *extData = (crsql_ExtData *)sqlite3_user_data(ctx);
    extData->mergeEqualValues = sqlite3_value_int(argv[1]);
  } else {
    sqlite3_result_error(ctx, "Unknown setting name", -1);
    sqlite3_result_error_code(ctx, SQLITE_ERROR);
    return;
  }

  // Persist to crsql_master
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  char *configKey = sqlite3_mprintf("config.%s", name);
  if (!configKey) {
    sqlite3_result_error_nomem(ctx);
    return;
  }

  sqlite3_stmt *pStmt = 0;
  int rc = sqlite3_prepare_v2(
      db, "INSERT OR REPLACE INTO crsql_master VALUES (?, ?) RETURNING value",
      -1, &pStmt, 0);
  if (rc != SQLITE_OK) {
    sqlite3_free(configKey);
    sqlite3_result_error(ctx, "Could not persist config in database", -1);
    sqlite3_result_error_code(ctx, rc);
    return;
  }

  sqlite3_bind_text(pStmt, 1, configKey, -1, sqlite3_free);
  sqlite3_bind_value(pStmt, 2, argv[1]);

  if (sqlite3_step(pStmt) == SQLITE_ROW) {
    sqlite3_result_value(ctx, sqlite3_column_value(pStmt, 0));
  } else {
    sqlite3_result_error(ctx, "Could not persist config in database", -1);
    sqlite3_result_error_code(ctx, SQLITE_ERROR);
  }
  sqlite3_finalize(pStmt);
}

void crsql_config_get(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  if (argc < 1) {
    sqlite3_result_error(ctx, "config_get requires 1 argument", -1);
    return;
  }

  const char *name = (const char *)sqlite3_value_text(argv[0]);
  if (!name) {
    sqlite3_result_error(ctx, "config_get: name must not be null", -1);
    return;
  }

  if (strcmp(name, MERGE_EQUAL_VALUES) == 0) {
    crsql_ExtData *extData = (crsql_ExtData *)sqlite3_user_data(ctx);
    sqlite3_result_int(ctx, extData->mergeEqualValues);
  } else {
    sqlite3_result_error(ctx, "Unknown setting name", -1);
    sqlite3_result_error_code(ctx, SQLITE_ERROR);
  }
}
