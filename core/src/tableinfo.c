#include "tableinfo.h"

#include <string.h>

#include "consts.h"
#include "pack-columns.h"
#include "util.h"

// ---------- helper: finalize and null a stmt pointer ----------
static void finalize_stmt(sqlite3_stmt **ppStmt) {
  if (*ppStmt) {
    sqlite3_finalize(*ppStmt);
    *ppStmt = 0;
  }
}

static int reset_cached_stmt(sqlite3_stmt *pStmt) {
  if (pStmt == 0) {
    return SQLITE_OK;
  }
  sqlite3_clear_bindings(pStmt);
  return sqlite3_reset(pStmt);
}

// ---------- ColumnInfo per-column stmt management ----------

static int get_col_curr_value_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                   crsql_ColumnInfo *colInfo,
                                   sqlite3_stmt **ppStmt) {
  if (colInfo->pCurrValueStmt) {
    *ppStmt = colInfo->pCurrValueStmt;
    return SQLITE_OK;
  }
  char *escaped_col = crsql_escape_ident(colInfo->name);
  char *escaped_tbl = crsql_escape_ident(tblInfo->tblName);
  char *where_list = crsql_where_list(tblInfo->pks, tblInfo->pksLen, 0);
  if (!escaped_col || !escaped_tbl || !where_list) {
    sqlite3_free(escaped_col);
    sqlite3_free(escaped_tbl);
    sqlite3_free(where_list);
    return SQLITE_NOMEM;
  }
  char *sql = sqlite3_mprintf(
      "SELECT \"%s\" FROM \"%s\" WHERE %s", escaped_col, escaped_tbl,
      where_list);
  sqlite3_free(escaped_col);
  sqlite3_free(escaped_tbl);
  sqlite3_free(where_list);
  if (!sql) return SQLITE_NOMEM;

  int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT,
                              &colInfo->pCurrValueStmt, 0);
  sqlite3_free(sql);
  *ppStmt = colInfo->pCurrValueStmt;
  return rc;
}

static int get_col_merge_insert_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                     crsql_ColumnInfo *colInfo,
                                     sqlite3_stmt **ppStmt) {
  if (colInfo->pMergeInsertStmt) {
    *ppStmt = colInfo->pMergeInsertStmt;
    return SQLITE_OK;
  }
  char *escaped_tbl = crsql_escape_ident(tblInfo->tblName);
  char *escaped_col = crsql_escape_ident(colInfo->name);
  char *pk_list = crsql_as_identifier_list(tblInfo->pks, tblInfo->pksLen, 0);
  char *pk_bindings = crsql_binding_list(tblInfo->pksLen);
  if (!escaped_tbl || !escaped_col || !pk_list || !pk_bindings) {
    sqlite3_free(escaped_tbl);
    sqlite3_free(escaped_col);
    sqlite3_free(pk_list);
    sqlite3_free(pk_bindings);
    return SQLITE_NOMEM;
  }
  char *sql = sqlite3_mprintf(
      "INSERT INTO \"%s\" (%s, \"%s\")"
      " VALUES (%s, ?)"
      " ON CONFLICT DO UPDATE"
      " SET \"%s\" = ?",
      escaped_tbl, pk_list, escaped_col, pk_bindings, escaped_col);
  sqlite3_free(escaped_tbl);
  sqlite3_free(escaped_col);
  sqlite3_free(pk_list);
  sqlite3_free(pk_bindings);
  if (!sql) return SQLITE_NOMEM;

  int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT,
                              &colInfo->pMergeInsertStmt, 0);
  sqlite3_free(sql);
  *ppStmt = colInfo->pMergeInsertStmt;
  return rc;
}

static int get_col_row_patch_data_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                       crsql_ColumnInfo *colInfo,
                                       sqlite3_stmt **ppStmt) {
  if (colInfo->pRowPatchDataStmt) {
    *ppStmt = colInfo->pRowPatchDataStmt;
    return SQLITE_OK;
  }
  char *escaped_col = crsql_escape_ident(colInfo->name);
  char *escaped_tbl = crsql_escape_ident(tblInfo->tblName);
  char *where_list = crsql_where_list(tblInfo->pks, tblInfo->pksLen, 0);
  if (!escaped_col || !escaped_tbl || !where_list) {
    sqlite3_free(escaped_col);
    sqlite3_free(escaped_tbl);
    sqlite3_free(where_list);
    return SQLITE_NOMEM;
  }
  char *sql = sqlite3_mprintf(
      "SELECT \"%s\" FROM \"%s\" WHERE %s", escaped_col, escaped_tbl,
      where_list);
  sqlite3_free(escaped_col);
  sqlite3_free(escaped_tbl);
  sqlite3_free(where_list);
  if (!sql) return SQLITE_NOMEM;

  int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT,
                              &colInfo->pRowPatchDataStmt, 0);
  sqlite3_free(sql);
  *ppStmt = colInfo->pRowPatchDataStmt;
  return rc;
}

static void clear_col_stmts(crsql_ColumnInfo *col) {
  finalize_stmt(&col->pCurrValueStmt);
  finalize_stmt(&col->pMergeInsertStmt);
  finalize_stmt(&col->pRowPatchDataStmt);
}

// ---------- TableInfo statement management ----------

void crsql_clear_table_info_stmts(crsql_TableInfo *tblInfo) {
  finalize_stmt(&tblInfo->pSelectKeyStmt);
  finalize_stmt(&tblInfo->pInsertKeyStmt);
  finalize_stmt(&tblInfo->pInsertOrIgnoreReturningKeyStmt);
  finalize_stmt(&tblInfo->pSetWinnerClockStmt);
  finalize_stmt(&tblInfo->pLocalClStmt);
  finalize_stmt(&tblInfo->pColVersionStmt);
  finalize_stmt(&tblInfo->pColSiteIdStmt);
  finalize_stmt(&tblInfo->pMergePkOnlyInsertStmt);
  finalize_stmt(&tblInfo->pMergeDeleteStmt);
  finalize_stmt(&tblInfo->pMergeDeleteDropClocksStmt);
  finalize_stmt(&tblInfo->pZeroClocksOnResurrectStmt);
  finalize_stmt(&tblInfo->pMarkLocallyDeletedStmt);
  finalize_stmt(&tblInfo->pMoveNonSentinelsStmt);
  finalize_stmt(&tblInfo->pMarkLocallyCreatedStmt);
  finalize_stmt(&tblInfo->pMarkLocallyUpdatedStmt);
  finalize_stmt(&tblInfo->pMaybeMarkLocallyReinsertedStmt);

  for (int i = 0; i < tblInfo->nonPksLen; i++) {
    clear_col_stmts(&tblInfo->nonPks[i]);
  }
}

// Free the contents of a TableInfo without freeing the struct itself.
// Use this for inline structs (e.g. within an array).
static void free_table_info_contents(crsql_TableInfo *tblInfo) {
  if (!tblInfo) return;
  crsql_clear_table_info_stmts(tblInfo);
  sqlite3_free(tblInfo->tblName);
  tblInfo->tblName = 0;
  for (int i = 0; i < tblInfo->pksLen; i++) {
    sqlite3_free(tblInfo->pks[i].name);
  }
  sqlite3_free(tblInfo->pks);
  tblInfo->pks = 0;
  tblInfo->pksLen = 0;
  for (int i = 0; i < tblInfo->nonPksLen; i++) {
    sqlite3_free(tblInfo->nonPks[i].name);
  }
  sqlite3_free(tblInfo->nonPks);
  tblInfo->nonPks = 0;
  tblInfo->nonPksLen = 0;
}

void crsql_free_table_info(crsql_TableInfo *tblInfo) {
  if (!tblInfo) return;
  free_table_info_contents(tblInfo);
  sqlite3_free(tblInfo);
}

// ---------- TableInfoVec ----------

void crsql_init_table_info_vec(crsql_ExtData *pExtData) {
  crsql_TableInfoVec *vec = sqlite3_malloc(sizeof(crsql_TableInfoVec));
  if (vec) {
    vec->aInfos = 0;
    vec->len = 0;
    vec->capacity = 0;
  }
  pExtData->tableInfos = vec;
}

void crsql_drop_table_info_vec(crsql_ExtData *pExtData) {
  crsql_TableInfoVec *vec = (crsql_TableInfoVec *)pExtData->tableInfos;
  if (!vec) return;
  for (int i = 0; i < vec->len; i++) {
    free_table_info_contents(&vec->aInfos[i]);
  }
  sqlite3_free(vec->aInfos);
  sqlite3_free(vec);
  pExtData->tableInfos = 0;
}

void crsql_clear_stmt_cache(crsql_ExtData *pExtData) {
  crsql_TableInfoVec *vec = (crsql_TableInfoVec *)pExtData->tableInfos;
  if (!vec) return;
  for (int i = 0; i < vec->len; i++) {
    crsql_clear_table_info_stmts(&vec->aInfos[i]);
  }
}

// ---------- pull_table_info ----------

int crsql_pull_table_info(sqlite3 *db, const char *tblName,
                          crsql_TableInfo **ppTableInfo, char **errmsg) {
  sqlite3_stmt *pStmt = 0;
  int rc;

  // Count columns
  char *countSql =
      sqlite3_mprintf("SELECT count(*) FROM pragma_table_info('%q')", tblName);
  if (!countSql) return SQLITE_NOMEM;

  rc = sqlite3_prepare_v2(db, countSql, -1, &pStmt, 0);
  sqlite3_free(countSql);
  if (rc != SQLITE_OK) {
    if (errmsg) *errmsg = sqlite3_mprintf("Failed to find columns for crr -- %s", tblName);
    return rc;
  }
  sqlite3_step(pStmt);
  int columnsLen = sqlite3_column_int(pStmt, 0);
  sqlite3_finalize(pStmt);

  // Fetch column info
  char *infoSql = sqlite3_mprintf(
      "SELECT \"cid\", \"name\", \"pk\""
      " FROM pragma_table_info('%q') ORDER BY cid ASC",
      tblName);
  if (!infoSql) return SQLITE_NOMEM;

  rc = sqlite3_prepare_v2(db, infoSql, -1, &pStmt, 0);
  sqlite3_free(infoSql);
  if (rc != SQLITE_OK) {
    if (errmsg) *errmsg = sqlite3_mprintf("Failed to prepare select for crr -- %s", tblName);
    return rc;
  }

  // Temporary arrays - allocate for max possible size
  crsql_ColumnInfo *allCols = sqlite3_malloc(sizeof(crsql_ColumnInfo) * columnsLen);
  if (!allCols) {
    sqlite3_finalize(pStmt);
    return SQLITE_NOMEM;
  }
  memset(allCols, 0, sizeof(crsql_ColumnInfo) * columnsLen);

  int colIdx = 0;
  while (sqlite3_step(pStmt) == SQLITE_ROW && colIdx < columnsLen) {
    allCols[colIdx].cid = sqlite3_column_int(pStmt, 0);
    const char *name = (const char *)sqlite3_column_text(pStmt, 1);
    allCols[colIdx].name = sqlite3_mprintf("%s", name);
    allCols[colIdx].pk = sqlite3_column_int(pStmt, 2);
    allCols[colIdx].pCurrValueStmt = 0;
    allCols[colIdx].pMergeInsertStmt = 0;
    allCols[colIdx].pRowPatchDataStmt = 0;
    colIdx++;
  }
  sqlite3_finalize(pStmt);

  if (colIdx != columnsLen) {
    for (int i = 0; i < colIdx; i++) sqlite3_free(allCols[i].name);
    sqlite3_free(allCols);
    if (errmsg) *errmsg = sqlite3_mprintf("Column count mismatch for %s", tblName);
    return SQLITE_ERROR;
  }

  // Partition into PKs and non-PKs
  int pksLen = 0, nonPksLen = 0;
  for (int i = 0; i < columnsLen; i++) {
    if (allCols[i].pk > 0) pksLen++;
    else nonPksLen++;
  }

  crsql_ColumnInfo *pks = pksLen > 0 ? sqlite3_malloc(sizeof(crsql_ColumnInfo) * pksLen) : 0;
  crsql_ColumnInfo *nonPks = nonPksLen > 0 ? sqlite3_malloc(sizeof(crsql_ColumnInfo) * nonPksLen) : 0;

  int pi = 0, ni = 0;
  for (int i = 0; i < columnsLen; i++) {
    if (allCols[i].pk > 0) {
      pks[pi++] = allCols[i];
    } else {
      nonPks[ni++] = allCols[i];
    }
  }
  sqlite3_free(allCols);

  // Sort PKs by pk position
  for (int i = 0; i < pksLen - 1; i++) {
    for (int j = i + 1; j < pksLen; j++) {
      if (pks[j].pk < pks[i].pk) {
        crsql_ColumnInfo tmp = pks[i];
        pks[i] = pks[j];
        pks[j] = tmp;
      }
    }
  }

  crsql_TableInfo *tblInfo = sqlite3_malloc(sizeof(crsql_TableInfo));
  if (!tblInfo) {
    for (int i = 0; i < pksLen; i++) sqlite3_free(pks[i].name);
    for (int i = 0; i < nonPksLen; i++) sqlite3_free(nonPks[i].name);
    sqlite3_free(pks);
    sqlite3_free(nonPks);
    return SQLITE_NOMEM;
  }
  memset(tblInfo, 0, sizeof(crsql_TableInfo));
  tblInfo->tblName = sqlite3_mprintf("%s", tblName);
  tblInfo->pks = pks;
  tblInfo->pksLen = pksLen;
  tblInfo->nonPks = nonPks;
  tblInfo->nonPksLen = nonPksLen;

  *ppTableInfo = tblInfo;
  return SQLITE_OK;
}

// ---------- is_table_compatible ----------

int crsql_is_table_compatible(sqlite3 *db, const char *tblName, char **err) {
  sqlite3_stmt *pStmt = 0;
  int rc;
  int count;

  // No unique indices besides primary key
  char *sql = sqlite3_mprintf(
      "SELECT count(*) FROM pragma_index_list('%q')"
      " WHERE \"origin\" != 'pk' AND \"unique\" = 1",
      tblName);
  rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return -1;
  sqlite3_step(pStmt);
  count = sqlite3_column_int(pStmt, 0);
  sqlite3_finalize(pStmt);
  if (count != 0) {
    if (err) *err = sqlite3_mprintf(
        "Table %s has unique indices besides the primary key. "
        "This is not allowed for CRRs", tblName);
    return 0;
  }

  // Must have non-nullable primary key
  sql = sqlite3_mprintf(
      "SELECT count(*) FROM pragma_table_info('%q')"
      " WHERE \"pk\" > 0 AND \"notnull\" > 0",
      tblName);
  rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return -1;
  sqlite3_step(pStmt);
  int validPks = sqlite3_column_int(pStmt, 0);
  sqlite3_finalize(pStmt);
  if (validPks == 0) {
    if (err) *err = sqlite3_mprintf(
        "Table %s has no primary key or primary key is nullable. "
        "CRRs must have a non nullable primary key", tblName);
    return 0;
  }

  // All PKs must be non-nullable
  sql = sqlite3_mprintf(
      "SELECT count(*) FROM pragma_table_info('%q') WHERE \"pk\" > 0",
      tblName);
  rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return -1;
  sqlite3_step(pStmt);
  count = sqlite3_column_int(pStmt, 0);
  sqlite3_finalize(pStmt);
  if (count != validPks) {
    if (err) *err = sqlite3_mprintf(
        "Table %s has composite primary key part of which is nullable. "
        "CRRs must have a non nullable primary key", tblName);
    return 0;
  }

  // No auto-increment
  sql = sqlite3_mprintf(
      "SELECT 1 FROM sqlite_master WHERE name = ? AND type = 'table'"
      " AND sql LIKE '%%autoincrement%%' LIMIT 1");
  rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return -1;
  sqlite3_bind_text(pStmt, 1, tblName, -1, SQLITE_STATIC);
  rc = sqlite3_step(pStmt);
  sqlite3_finalize(pStmt);
  if (rc == SQLITE_ROW) {
    if (err) *err = sqlite3_mprintf(
        "%s has auto-increment primary keys. This is likely a mistake as two "
        "concurrent nodes will assign unrelated rows the same primary key.",
        tblName);
    return 0;
  }

  // No foreign keys
  sql = sqlite3_mprintf(
      "SELECT count(*) FROM pragma_foreign_key_list('%q')", tblName);
  rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return -1;
  sqlite3_step(pStmt);
  count = sqlite3_column_int(pStmt, 0);
  sqlite3_finalize(pStmt);
  if (count != 0) {
    if (err) *err = sqlite3_mprintf(
        "Table %s has checked foreign key constraints. "
        "CRRs may have foreign keys but must not have "
        "checked foreign key constraints.", tblName);
    return 0;
  }

  // NOT NULL columns must have DEFAULT VALUE
  sql = sqlite3_mprintf(
      "SELECT count(*) FROM pragma_table_xinfo('%q')"
      " WHERE \"notnull\" = 1 AND \"dflt_value\" IS NULL AND \"pk\" = 0",
      tblName);
  rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return -1;
  sqlite3_step(pStmt);
  count = sqlite3_column_int(pStmt, 0);
  sqlite3_finalize(pStmt);
  if (count != 0) {
    if (err) *err = sqlite3_mprintf(
        "Table %s has a NOT NULL column without a DEFAULT VALUE. "
        "This is not allowed as it prevents forwards and backwards "
        "compatibility between schema versions.", tblName);
    return 0;
  }

  return 1;
}

// ---------- ensure_table_infos_are_up_to_date ----------

static int pull_all_table_infos(sqlite3 *db, crsql_ExtData *pExtData,
                                char **errmsg) {
  crsql_TableInfoVec *vec = (crsql_TableInfoVec *)pExtData->tableInfos;

  // Table info indices are about to change; drop merge memos keyed on them.
  crsql_invalidate_merge_memos(pExtData);

  // Free old table infos
  for (int i = 0; i < vec->len; i++) {
    free_table_info_contents(&vec->aInfos[i]);
  }
  vec->len = 0;

  // Get clock table names
  sqlite3_stmt *pStmt = pExtData->pSelectClockTablesStmt;
  int rc;

  // Count first pass + collect names
  int numTables = 0;
  int namesCapacity = 8;
  char **names = sqlite3_malloc(sizeof(char *) * namesCapacity);
  if (!names) return SQLITE_NOMEM;

  while ((rc = sqlite3_step(pStmt)) == SQLITE_ROW) {
    const char *clockTblName = (const char *)sqlite3_column_text(pStmt, 0);
    if (numTables >= namesCapacity) {
      namesCapacity *= 2;
      char **newNames = sqlite3_realloc(names, sizeof(char *) * namesCapacity);
      if (!newNames) {
        for (int i = 0; i < numTables; i++) sqlite3_free(names[i]);
        sqlite3_free(names);
        sqlite3_reset(pStmt);
        return SQLITE_NOMEM;
      }
      names = newNames;
    }
    // Strip "__crsql_clock" suffix to get base table name
    int clockLen = (int)strlen(clockTblName);
    int suffixLen = (int)strlen("__crsql_clock");
    int baseLen = clockLen - suffixLen;
    if (baseLen <= 0) {
      // Invalid clock table name, skip
      continue;
    }
    names[numTables] = sqlite3_malloc(baseLen + 1);
    if (!names[numTables]) {
      for (int i = 0; i < numTables; i++) sqlite3_free(names[i]);
      sqlite3_free(names);
      sqlite3_reset(pStmt);
      return SQLITE_NOMEM;
    }
    memcpy(names[numTables], clockTblName, baseLen);
    names[numTables][baseLen] = '\0';
    numTables++;
  }
  sqlite3_reset(pStmt);

  // Ensure capacity in vec
  if (numTables > vec->capacity) {
    crsql_TableInfo *newInfos =
        sqlite3_realloc(vec->aInfos, sizeof(crsql_TableInfo) * numTables);
    if (!newInfos) {
      for (int i = 0; i < numTables; i++) sqlite3_free(names[i]);
      sqlite3_free(names);
      return SQLITE_NOMEM;
    }
    vec->aInfos = newInfos;
    vec->capacity = numTables;
  }

  // Pull info for each table
  for (int i = 0; i < numTables; i++) {
    crsql_TableInfo *pInfo = 0;
    rc = crsql_pull_table_info(db, names[i], &pInfo, errmsg);
    if (rc != SQLITE_OK || !pInfo) {
      for (int j = 0; j < numTables; j++) sqlite3_free(names[j]);
      sqlite3_free(names);
      // Clean up already-added infos
      for (int j = 0; j < vec->len; j++) {
        free_table_info_contents(&vec->aInfos[j]);
      }
      vec->len = 0;
      return rc;
    }
    // Copy into vec (move ownership)
    vec->aInfos[i] = *pInfo;
    sqlite3_free(pInfo);  // free the wrapper, not the contents
    vec->len++;
  }

  for (int i = 0; i < numTables; i++) sqlite3_free(names[i]);
  sqlite3_free(names);

  return SQLITE_OK;
}

int crsql_ensure_table_infos_are_up_to_date(sqlite3 *db,
                                            crsql_ExtData *pExtData,
                                            char **errmsg) {
  if (pExtData->updatedTableInfosThisTx == 1) {
    return SQLITE_OK;
  }

  int schemaChanged =
      crsql_fetchPragmaSchemaVersion(db, pExtData, 1 /* TABLE_INFO */);
  if (schemaChanged < 0) {
    return SQLITE_ERROR;
  }

  crsql_TableInfoVec *vec = (crsql_TableInfoVec *)pExtData->tableInfos;
  if (schemaChanged > 0 || vec->len == 0) {
    int rc = pull_all_table_infos(db, pExtData, errmsg);
    if (rc != SQLITE_OK) {
      return rc;
    }
  }

  pExtData->updatedTableInfosThisTx = 1;
  return SQLITE_OK;
}

// ---------- find_table_info ----------

int crsql_find_table_info(crsql_TableInfoVec *vec, const char *tblName) {
  for (int i = 0; i < vec->len; i++) {
    if (strcmp(vec->aInfos[i].tblName, tblName) == 0) {
      return i;
    }
  }
  return -1;
}

// ---------- Lazy statement getters ----------

// Proper helper: prepare if null, always free sql
static int lazy_prepare(sqlite3 *db, sqlite3_stmt **ppCached, char *sql,
                        sqlite3_stmt **ppOut) {
  if (*ppCached) {
    *ppOut = *ppCached;
    sqlite3_free(sql);
    return SQLITE_OK;
  }
  if (!sql) {
    *ppOut = 0;
    return SQLITE_NOMEM;
  }
  int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT,
                              ppCached, 0);
  sqlite3_free(sql);
  *ppOut = *ppCached;
  return rc;
}

int crsql_get_set_winner_clock_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                    sqlite3_stmt **ppStmt) {
  if (tblInfo->pSetWinnerClockStmt) {
    *ppStmt = tblInfo->pSetWinnerClockStmt;
    return SQLITE_OK;
  }
  char *esc = crsql_escape_ident(tblInfo->tblName);
  // No RETURNING here: the caller already knows the key it binds, and a
  // RETURNING clause makes SQLite materialize an ephemeral btree (with its
  // own pager + page cache) on every execution. The db_version is computed
  // in C and bound directly rather than invoking the crsql_next_db_version
  // SQL function per row.
  char *sql = sqlite3_mprintf(
      "INSERT OR REPLACE INTO \"%s__crsql_clock\""
      " (key, col_name, col_version, db_version, seq, site_id)"
      " VALUES (?, ?, ?, ?, ?, ?)", esc);
  sqlite3_free(esc);
  return lazy_prepare(db, &tblInfo->pSetWinnerClockStmt, sql, ppStmt);
}

int crsql_get_local_cl_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                            sqlite3_stmt **ppStmt) {
  if (tblInfo->pLocalClStmt) {
    *ppStmt = tblInfo->pLocalClStmt;
    return SQLITE_OK;
  }
  char *esc = crsql_escape_ident(tblInfo->tblName);
  char *sql = sqlite3_mprintf(
      "SELECT COALESCE("
      "(SELECT col_version FROM \"%s__crsql_clock\" WHERE key = ? AND col_name = '" SENTINEL_CID "'),"
      "(SELECT 1 FROM \"%s__crsql_clock\" WHERE key = ?)"
      ")", esc, esc);
  sqlite3_free(esc);
  return lazy_prepare(db, &tblInfo->pLocalClStmt, sql, ppStmt);
}

int crsql_get_col_version_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                               sqlite3_stmt **ppStmt) {
  if (tblInfo->pColVersionStmt) {
    *ppStmt = tblInfo->pColVersionStmt;
    return SQLITE_OK;
  }
  char *esc = crsql_escape_ident(tblInfo->tblName);
  char *sql = sqlite3_mprintf(
      "SELECT col_version FROM \"%s__crsql_clock\" WHERE key = ? AND col_name = ?",
      esc);
  sqlite3_free(esc);
  return lazy_prepare(db, &tblInfo->pColVersionStmt, sql, ppStmt);
}

int crsql_get_col_site_id_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                               sqlite3_stmt **ppStmt) {
  if (tblInfo->pColSiteIdStmt) {
    *ppStmt = tblInfo->pColSiteIdStmt;
    return SQLITE_OK;
  }
  char *esc = crsql_escape_ident(tblInfo->tblName);
  char *sql = sqlite3_mprintf(
      "SELECT site_id FROM crsql_site_id WHERE ordinal = "
      "(SELECT site_id FROM \"%s__crsql_clock\" WHERE key = ? AND col_name = ?)",
      esc);
  sqlite3_free(esc);
  return lazy_prepare(db, &tblInfo->pColSiteIdStmt, sql, ppStmt);
}

int crsql_get_merge_pk_only_insert_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                        sqlite3_stmt **ppStmt) {
  if (tblInfo->pMergePkOnlyInsertStmt) {
    *ppStmt = tblInfo->pMergePkOnlyInsertStmt;
    return SQLITE_OK;
  }
  char *esc = crsql_escape_ident(tblInfo->tblName);
  char *pk_list = crsql_as_identifier_list(tblInfo->pks, tblInfo->pksLen, 0);
  char *pk_bindings = crsql_binding_list(tblInfo->pksLen);
  char *sql = sqlite3_mprintf(
      "INSERT OR IGNORE INTO \"%s\" (%s) VALUES (%s)",
      esc, pk_list, pk_bindings);
  sqlite3_free(esc);
  sqlite3_free(pk_list);
  sqlite3_free(pk_bindings);
  return lazy_prepare(db, &tblInfo->pMergePkOnlyInsertStmt, sql, ppStmt);
}

int crsql_get_merge_delete_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                sqlite3_stmt **ppStmt) {
  if (tblInfo->pMergeDeleteStmt) {
    *ppStmt = tblInfo->pMergeDeleteStmt;
    return SQLITE_OK;
  }
  char *esc = crsql_escape_ident(tblInfo->tblName);
  char *where_list = crsql_where_list(tblInfo->pks, tblInfo->pksLen, 0);
  char *sql = sqlite3_mprintf("DELETE FROM \"%s\" WHERE %s", esc, where_list);
  sqlite3_free(esc);
  sqlite3_free(where_list);
  return lazy_prepare(db, &tblInfo->pMergeDeleteStmt, sql, ppStmt);
}

int crsql_get_merge_delete_drop_clocks_stmt(sqlite3 *db,
                                            crsql_TableInfo *tblInfo,
                                            sqlite3_stmt **ppStmt) {
  if (tblInfo->pMergeDeleteDropClocksStmt) {
    *ppStmt = tblInfo->pMergeDeleteDropClocksStmt;
    return SQLITE_OK;
  }
  char *esc = crsql_escape_ident(tblInfo->tblName);
  char *sql = sqlite3_mprintf(
      "DELETE FROM \"%s__crsql_clock\" WHERE key = ? AND col_name IS NOT '" SENTINEL_CID "'",
      esc);
  sqlite3_free(esc);
  return lazy_prepare(db, &tblInfo->pMergeDeleteDropClocksStmt, sql, ppStmt);
}

int crsql_get_zero_clocks_on_resurrect_stmt(sqlite3 *db,
                                            crsql_TableInfo *tblInfo,
                                            sqlite3_stmt **ppStmt) {
  if (tblInfo->pZeroClocksOnResurrectStmt) {
    *ppStmt = tblInfo->pZeroClocksOnResurrectStmt;
    return SQLITE_OK;
  }
  char *esc = crsql_escape_ident(tblInfo->tblName);
  // db_version computed in C and bound directly (see set_winner_clock).
  char *sql = sqlite3_mprintf(
      "UPDATE \"%s__crsql_clock\" SET col_version = 0, db_version = ?"
      " WHERE key = ? AND col_name IS NOT '" SENTINEL_CID "'",
      esc);
  sqlite3_free(esc);
  return lazy_prepare(db, &tblInfo->pZeroClocksOnResurrectStmt, sql, ppStmt);
}

int crsql_get_mark_locally_deleted_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                        sqlite3_stmt **ppStmt) {
  if (tblInfo->pMarkLocallyDeletedStmt) {
    *ppStmt = tblInfo->pMarkLocallyDeletedStmt;
    return SQLITE_OK;
  }
  char *esc = crsql_escape_ident(tblInfo->tblName);
  char *sql = sqlite3_mprintf(
      "INSERT INTO \"%s__crsql_clock\" ("
      "key, col_name, col_version, db_version, seq, site_id"
      ") SELECT ?, '" SENTINEL_CID "', 2, ?, ?, 0 WHERE true"
      " ON CONFLICT DO UPDATE SET"
      " col_version = 1 + col_version,"
      " db_version = ?,"
      " seq = ?,"
      " site_id = 0",
      esc);
  sqlite3_free(esc);
  return lazy_prepare(db, &tblInfo->pMarkLocallyDeletedStmt, sql, ppStmt);
}

int crsql_get_move_non_sentinels_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                      sqlite3_stmt **ppStmt) {
  if (tblInfo->pMoveNonSentinelsStmt) {
    *ppStmt = tblInfo->pMoveNonSentinelsStmt;
    return SQLITE_OK;
  }
  char *esc = crsql_escape_ident(tblInfo->tblName);
  char *sql = sqlite3_mprintf(
      "UPDATE OR REPLACE \"%s__crsql_clock\" SET key = ? WHERE key = ?"
      " AND col_name != '" SENTINEL_CID "'",
      esc);
  sqlite3_free(esc);
  return lazy_prepare(db, &tblInfo->pMoveNonSentinelsStmt, sql, ppStmt);
}

int crsql_get_mark_locally_created_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                        sqlite3_stmt **ppStmt) {
  if (tblInfo->pMarkLocallyCreatedStmt) {
    *ppStmt = tblInfo->pMarkLocallyCreatedStmt;
    return SQLITE_OK;
  }
  char *esc = crsql_escape_ident(tblInfo->tblName);
  char *sql = sqlite3_mprintf(
      "INSERT INTO \"%s__crsql_clock\" ("
      "key, col_name, col_version, db_version, seq, site_id"
      ") SELECT ?, '" SENTINEL_CID "', 1, ?, ?, 0 WHERE true"
      " ON CONFLICT DO UPDATE SET"
      " col_version = CASE col_version %% 2 WHEN 0 THEN col_version + 1 ELSE col_version + 2 END,"
      " db_version = ?,"
      " seq = ?,"
      " site_id = 0",
      esc);
  sqlite3_free(esc);
  return lazy_prepare(db, &tblInfo->pMarkLocallyCreatedStmt, sql, ppStmt);
}

int crsql_get_mark_locally_updated_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                        sqlite3_stmt **ppStmt) {
  if (tblInfo->pMarkLocallyUpdatedStmt) {
    *ppStmt = tblInfo->pMarkLocallyUpdatedStmt;
    return SQLITE_OK;
  }
  char *esc = crsql_escape_ident(tblInfo->tblName);
  char *sql = sqlite3_mprintf(
      "INSERT INTO \"%s__crsql_clock\" ("
      "key, col_name, col_version, db_version, seq, site_id"
      ") SELECT ?, ?, 1, ?, ?, 0 WHERE true"
      " ON CONFLICT DO UPDATE SET"
      " col_version = col_version + 1,"
      " db_version = ?,"
      " seq = ?,"
      " site_id = 0;",
      esc);
  sqlite3_free(esc);
  return lazy_prepare(db, &tblInfo->pMarkLocallyUpdatedStmt, sql, ppStmt);
}

int crsql_get_maybe_mark_locally_reinserted_stmt(sqlite3 *db,
                                                 crsql_TableInfo *tblInfo,
                                                 sqlite3_stmt **ppStmt) {
  if (tblInfo->pMaybeMarkLocallyReinsertedStmt) {
    *ppStmt = tblInfo->pMaybeMarkLocallyReinsertedStmt;
    return SQLITE_OK;
  }
  char *esc = crsql_escape_ident(tblInfo->tblName);
  char *sql = sqlite3_mprintf(
      "UPDATE \"%s__crsql_clock\" SET"
      " col_version = CASE col_version %% 2 WHEN 0 THEN col_version + 1 ELSE col_version + 2 END,"
      " db_version = ?,"
      " seq = ?,"
      " site_id = 0"
      " WHERE key = ? AND col_name = ?",
      esc);
  sqlite3_free(esc);
  return lazy_prepare(db, &tblInfo->pMaybeMarkLocallyReinsertedStmt, sql,
                      ppStmt);
}

// ---------- Key management ----------

static int ensure_select_key_stmt(sqlite3 *db, crsql_TableInfo *tblInfo) {
  if (tblInfo->pSelectKeyStmt) return SQLITE_OK;
  char *esc = crsql_escape_ident(tblInfo->tblName);
  char *where_list = crsql_where_list(tblInfo->pks, tblInfo->pksLen, 0);
  char *sql = sqlite3_mprintf(
      "SELECT __crsql_key FROM \"%s__crsql_pks\" WHERE %s", esc, where_list);
  sqlite3_free(esc);
  sqlite3_free(where_list);
  if (!sql) return SQLITE_NOMEM;
  int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT,
                              &tblInfo->pSelectKeyStmt, 0);
  sqlite3_free(sql);
  return rc;
}

static int ensure_insert_key_stmt(sqlite3 *db, crsql_TableInfo *tblInfo) {
  if (tblInfo->pInsertKeyStmt) return SQLITE_OK;
  char *esc = crsql_escape_ident(tblInfo->tblName);
  char *pk_list = crsql_as_identifier_list(tblInfo->pks, tblInfo->pksLen, 0);
  char *pk_bindings = crsql_binding_list(tblInfo->pksLen);
  // __crsql_key is an INTEGER PRIMARY KEY (rowid alias): the assigned key is
  // read via sqlite3_last_insert_rowid. A RETURNING clause would force an
  // ephemeral btree (own pager + page cache) on every execution.
  char *sql = sqlite3_mprintf(
      "INSERT INTO \"%s__crsql_pks\" (%s) VALUES (%s)",
      esc, pk_list, pk_bindings);
  sqlite3_free(esc);
  sqlite3_free(pk_list);
  sqlite3_free(pk_bindings);
  if (!sql) return SQLITE_NOMEM;
  int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT,
                              &tblInfo->pInsertKeyStmt, 0);
  sqlite3_free(sql);
  return rc;
}

// Step a select-key stmt with bindings already applied.
static sqlite3_int64 step_key_stmt(sqlite3_stmt *pStmt) {
  int rc = sqlite3_step(pStmt);
  sqlite3_int64 key = -1;
  if (rc == SQLITE_ROW) {
    key = sqlite3_column_int64(pStmt, 0);
  }
  reset_cached_stmt(pStmt);
  return key;
}

// Step an insert-key stmt with bindings already applied; the new key is the
// rowid assigned to the inserted row.
static sqlite3_int64 step_insert_key_stmt(sqlite3 *db, sqlite3_stmt *pStmt) {
  int rc = sqlite3_step(pStmt);
  sqlite3_int64 key = -1;
  if (rc == SQLITE_DONE) {
    key = sqlite3_last_insert_rowid(db);
  }
  reset_cached_stmt(pStmt);
  return key;
}

sqlite3_int64 crsql_get_key(sqlite3 *db, crsql_TableInfo *tblInfo,
                            sqlite3_value **pks, int numPks) {
  if (ensure_select_key_stmt(db, tblInfo) != SQLITE_OK) return -1;

  for (int i = 0; i < numPks; i++) {
    sqlite3_bind_value(tblInfo->pSelectKeyStmt, i + 1, pks[i]);
  }
  return step_key_stmt(tblInfo->pSelectKeyStmt);
}

static sqlite3_int64 create_key_raw(sqlite3 *db, crsql_TableInfo *tblInfo,
                                    sqlite3_value **pks, int numPks) {
  if (ensure_insert_key_stmt(db, tblInfo) != SQLITE_OK) return -1;

  for (int i = 0; i < numPks; i++) {
    sqlite3_bind_value(tblInfo->pInsertKeyStmt, i + 1, pks[i]);
  }
  return step_insert_key_stmt(db, tblInfo->pInsertKeyStmt);
}

sqlite3_int64 crsql_get_or_create_key(sqlite3 *db, crsql_TableInfo *tblInfo,
                                      sqlite3_value **pks, int numPks,
                                      char **errmsg) {
  sqlite3_int64 key = crsql_get_key(db, tblInfo, pks, numPks);
  if (key >= 0) {
    return key;
  }
  key = create_key_raw(db, tblInfo, pks, numPks);
  if (key < 0 && errmsg) {
    *errmsg = sqlite3_mprintf("Failed to create key for table %s",
                              tblInfo->tblName);
  }
  return key;
}

/**
 * Same as crsql_get_or_create_key but takes unpacked ColumnValues directly,
 * avoiding the temporary `SELECT ?,?,...` statement previously needed to
 * convert them into sqlite3_value pointers. This is the merge hot path: it
 * runs once per imported change.
 */
sqlite3_int64 crsql_get_or_create_key_packed(sqlite3 *db,
                                             crsql_TableInfo *tblInfo,
                                             crsql_ColumnValue *pks,
                                             int numPks, char **errmsg) {
  if (ensure_select_key_stmt(db, tblInfo) != SQLITE_OK) return -1;

  int rc = crsql_bind_package_to_stmt(tblInfo->pSelectKeyStmt, pks, numPks, 0);
  if (rc != SQLITE_OK) {
    reset_cached_stmt(tblInfo->pSelectKeyStmt);
    return -1;
  }
  sqlite3_int64 key = step_key_stmt(tblInfo->pSelectKeyStmt);
  if (key >= 0) {
    return key;
  }

  if (ensure_insert_key_stmt(db, tblInfo) != SQLITE_OK) return -1;
  rc = crsql_bind_package_to_stmt(tblInfo->pInsertKeyStmt, pks, numPks, 0);
  if (rc != SQLITE_OK) {
    reset_cached_stmt(tblInfo->pInsertKeyStmt);
    return -1;
  }
  key = step_insert_key_stmt(db, tblInfo->pInsertKeyStmt);
  if (key < 0 && errmsg) {
    *errmsg = sqlite3_mprintf("Failed to create key for table %s",
                              tblInfo->tblName);
  }
  return key;
}

sqlite3_int64 crsql_get_or_create_key_for_insert(sqlite3 *db,
                                                  crsql_TableInfo *tblInfo,
                                                  sqlite3_value **pks,
                                                  int numPks,
                                                  char **errmsg) {
  // Try INSERT OR IGNORE first
  if (!tblInfo->pInsertOrIgnoreReturningKeyStmt) {
    char *esc = crsql_escape_ident(tblInfo->tblName);
    char *pk_list = crsql_as_identifier_list(tblInfo->pks, tblInfo->pksLen, 0);
    char *pk_bindings = crsql_binding_list(tblInfo->pksLen);
    // No RETURNING: whether the insert happened is read via
    // sqlite3_changes64 and the new key via sqlite3_last_insert_rowid
    // (__crsql_key is a rowid alias). RETURNING would force an ephemeral
    // btree per execution.
    char *sql = sqlite3_mprintf(
        "INSERT OR IGNORE INTO \"%s__crsql_pks\" (%s) VALUES (%s)",
        esc, pk_list, pk_bindings);
    sqlite3_free(esc);
    sqlite3_free(pk_list);
    sqlite3_free(pk_bindings);
    if (!sql) return -1;
    int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT,
                                &tblInfo->pInsertOrIgnoreReturningKeyStmt, 0);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return -1;
  }

  for (int i = 0; i < numPks; i++) {
    sqlite3_bind_value(tblInfo->pInsertOrIgnoreReturningKeyStmt, i + 1,
                       pks[i]);
  }

  int rc = sqlite3_step(tblInfo->pInsertOrIgnoreReturningKeyStmt);
  if (rc == SQLITE_DONE && sqlite3_changes64(db) > 0) {
    // Newly inserted
    sqlite3_int64 key = sqlite3_last_insert_rowid(db);
    reset_cached_stmt(tblInfo->pInsertOrIgnoreReturningKeyStmt);
    return key;
  }

  // Already existed (insert was ignored), fall back to select
  reset_cached_stmt(tblInfo->pInsertOrIgnoreReturningKeyStmt);
  sqlite3_int64 key = crsql_get_key(db, tblInfo, pks, numPks);
  if (key < 0 && errmsg) {
    *errmsg = sqlite3_mprintf("Failed to get key for table %s",
                              tblInfo->tblName);
  }
  return key;
}

// Public accessors for per-column stmts (used by changes_vtab_write)
int crsql_get_col_value_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                             const char *colName, sqlite3_stmt **ppStmt) {
  for (int i = 0; i < tblInfo->nonPksLen; i++) {
    if (strcmp(tblInfo->nonPks[i].name, colName) == 0) {
      return get_col_curr_value_stmt(db, tblInfo, &tblInfo->nonPks[i], ppStmt);
    }
  }
  *ppStmt = 0;
  return SQLITE_ERROR;
}

int crsql_get_merge_insert_col_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                    const char *colName,
                                    sqlite3_stmt **ppStmt) {
  for (int i = 0; i < tblInfo->nonPksLen; i++) {
    if (strcmp(tblInfo->nonPks[i].name, colName) == 0) {
      return get_col_merge_insert_stmt(db, tblInfo, &tblInfo->nonPks[i],
                                       ppStmt);
    }
  }
  *ppStmt = 0;
  return SQLITE_ERROR;
}

int crsql_get_row_patch_data_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                  const char *colName,
                                  sqlite3_stmt **ppStmt) {
  for (int i = 0; i < tblInfo->nonPksLen; i++) {
    if (strcmp(tblInfo->nonPks[i].name, colName) == 0) {
      return get_col_row_patch_data_stmt(db, tblInfo, &tblInfo->nonPks[i],
                                         ppStmt);
    }
  }
  *ppStmt = 0;
  return SQLITE_ERROR;
}
