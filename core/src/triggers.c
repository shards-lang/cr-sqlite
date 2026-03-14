#include "triggers.h"

#include <string.h>

#include "consts.h"
#include "util.h"

static int create_insert_trigger(sqlite3 *db, crsql_TableInfo *tableInfo,
                                 char **errmsg) {
  char *escName = crsql_escape_ident(tableInfo->tblName);
  char *escNameAsVal = crsql_escape_ident_as_value(tableInfo->tblName);
  char *pkNewList =
      crsql_as_identifier_list(tableInfo->pks, tableInfo->pksLen, "NEW.");
  if (!escName || !escNameAsVal || !pkNewList) {
    sqlite3_free(escName);
    sqlite3_free(escNameAsVal);
    sqlite3_free(pkNewList);
    return SQLITE_ERROR;
  }

  char *sql = sqlite3_mprintf(
      "CREATE TRIGGER IF NOT EXISTS \"%s__crsql_itrig\""
      " AFTER INSERT ON \"%s\" WHEN crsql_internal_sync_bit() = 0"
      " BEGIN"
      "   VALUES (crsql_after_insert('%s', %s));"
      " END;",
      escName, escName, escNameAsVal, pkNewList);
  sqlite3_free(escName);
  sqlite3_free(escNameAsVal);
  sqlite3_free(pkNewList);
  if (!sql) return SQLITE_NOMEM;

  int rc = sqlite3_exec(db, sql, 0, 0, errmsg);
  sqlite3_free(sql);
  return rc;
}

static int create_update_trigger(sqlite3 *db, crsql_TableInfo *tableInfo,
                                 char **errmsg) {
  char *escName = crsql_escape_ident(tableInfo->tblName);
  char *escNameAsVal = crsql_escape_ident_as_value(tableInfo->tblName);
  char *pkNewList =
      crsql_as_identifier_list(tableInfo->pks, tableInfo->pksLen, "NEW.");
  char *pkOldList =
      crsql_as_identifier_list(tableInfo->pks, tableInfo->pksLen, "OLD.");
  if (!escName || !escNameAsVal || !pkNewList || !pkOldList) {
    sqlite3_free(escName);
    sqlite3_free(escNameAsVal);
    sqlite3_free(pkNewList);
    sqlite3_free(pkOldList);
    return SQLITE_ERROR;
  }

  char *triggerBody;
  if (tableInfo->nonPksLen == 0) {
    triggerBody = sqlite3_mprintf(
        "VALUES (crsql_after_update('%s', %s, %s))", escNameAsVal, pkNewList,
        pkOldList);
  } else {
    char *nonPkNewList = crsql_as_identifier_list(tableInfo->nonPks,
                                                  tableInfo->nonPksLen, "NEW.");
    char *nonPkOldList = crsql_as_identifier_list(tableInfo->nonPks,
                                                  tableInfo->nonPksLen, "OLD.");
    if (!nonPkNewList || !nonPkOldList) {
      sqlite3_free(escName);
      sqlite3_free(escNameAsVal);
      sqlite3_free(pkNewList);
      sqlite3_free(pkOldList);
      sqlite3_free(nonPkNewList);
      sqlite3_free(nonPkOldList);
      return SQLITE_ERROR;
    }
    triggerBody = sqlite3_mprintf(
        "VALUES (crsql_after_update('%s', %s, %s, %s, %s))", escNameAsVal,
        pkNewList, pkOldList, nonPkNewList, nonPkOldList);
    sqlite3_free(nonPkNewList);
    sqlite3_free(nonPkOldList);
  }
  sqlite3_free(escNameAsVal);

  if (!triggerBody) {
    sqlite3_free(escName);
    sqlite3_free(pkNewList);
    sqlite3_free(pkOldList);
    return SQLITE_NOMEM;
  }

  char *sql = sqlite3_mprintf(
      "CREATE TRIGGER IF NOT EXISTS \"%s__crsql_utrig\""
      " AFTER UPDATE ON \"%s\" WHEN crsql_internal_sync_bit() = 0"
      " BEGIN"
      "   %s;"
      " END;",
      escName, escName, triggerBody);
  sqlite3_free(escName);
  sqlite3_free(pkNewList);
  sqlite3_free(pkOldList);
  sqlite3_free(triggerBody);
  if (!sql) return SQLITE_NOMEM;

  int rc = sqlite3_exec(db, sql, 0, 0, errmsg);
  sqlite3_free(sql);
  return rc;
}

static int create_delete_trigger(sqlite3 *db, crsql_TableInfo *tableInfo,
                                 char **errmsg) {
  char *escName = crsql_escape_ident(tableInfo->tblName);
  char *escNameAsVal = crsql_escape_ident_as_value(tableInfo->tblName);
  char *pkOldList =
      crsql_as_identifier_list(tableInfo->pks, tableInfo->pksLen, "OLD.");
  if (!escName || !escNameAsVal || !pkOldList) {
    sqlite3_free(escName);
    sqlite3_free(escNameAsVal);
    sqlite3_free(pkOldList);
    return SQLITE_ERROR;
  }

  char *sql = sqlite3_mprintf(
      "CREATE TRIGGER IF NOT EXISTS \"%s__crsql_dtrig\""
      " AFTER DELETE ON \"%s\" WHEN crsql_internal_sync_bit() = 0"
      " BEGIN"
      "   VALUES (crsql_after_delete('%s', %s));"
      " END;",
      escName, escName, escNameAsVal, pkOldList);
  sqlite3_free(escName);
  sqlite3_free(escNameAsVal);
  sqlite3_free(pkOldList);
  if (!sql) return SQLITE_NOMEM;

  int rc = sqlite3_exec(db, sql, 0, 0, errmsg);
  sqlite3_free(sql);
  return rc;
}

int crsql_create_triggers(sqlite3 *db, crsql_TableInfo *tableInfo,
                          char **errmsg) {
  int rc = create_insert_trigger(db, tableInfo, errmsg);
  if (rc != SQLITE_OK) return rc;
  rc = create_update_trigger(db, tableInfo, errmsg);
  if (rc != SQLITE_OK) return rc;
  return create_delete_trigger(db, tableInfo, errmsg);
}

int crsql_remove_crr_triggers_if_exist(sqlite3 *db, const char *table) {
  char *escTable = crsql_escape_ident(table);
  if (!escTable) return SQLITE_ERROR;

  char *sql;
  int rc;

  // Drop insert trigger
  sql = sqlite3_mprintf("DROP TRIGGER IF EXISTS \"%s__crsql_itrig\"",
                         escTable);
  if (!sql) {
    sqlite3_free(escTable);
    return SQLITE_NOMEM;
  }
  rc = sqlite3_exec(db, sql, 0, 0, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    sqlite3_free(escTable);
    return rc;
  }

  // Drop update trigger
  sql = sqlite3_mprintf("DROP TRIGGER IF EXISTS \"%s__crsql_utrig\"",
                         escTable);
  if (!sql) {
    sqlite3_free(escTable);
    return SQLITE_NOMEM;
  }
  rc = sqlite3_exec(db, sql, 0, 0, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    sqlite3_free(escTable);
    return rc;
  }

  // Drop per-PK-column update triggers (legacy)
  sqlite3_stmt *pStmt = 0;
  rc = sqlite3_prepare_v2(
      db, "SELECT name FROM pragma_table_info(?) WHERE pk > 0", -1, &pStmt, 0);
  if (rc != SQLITE_OK) {
    sqlite3_free(escTable);
    return rc;
  }
  sqlite3_bind_text(pStmt, 1, table, -1, SQLITE_STATIC);
  while (sqlite3_step(pStmt) == SQLITE_ROW) {
    const char *colName = (const char *)sqlite3_column_text(pStmt, 0);
    char *escCol = crsql_escape_ident(colName);
    if (!escCol) {
      sqlite3_finalize(pStmt);
      sqlite3_free(escTable);
      return SQLITE_ERROR;
    }
    char *escTableOrig = crsql_escape_ident(table);
    sql = sqlite3_mprintf("DROP TRIGGER IF EXISTS \"%s_%s__crsql_utrig\"",
                           escTableOrig, escCol);
    sqlite3_free(escTableOrig);
    sqlite3_free(escCol);
    if (!sql) {
      sqlite3_finalize(pStmt);
      sqlite3_free(escTable);
      return SQLITE_NOMEM;
    }
    rc = sqlite3_exec(db, sql, 0, 0, 0);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) {
      sqlite3_finalize(pStmt);
      sqlite3_free(escTable);
      return rc;
    }
  }
  sqlite3_finalize(pStmt);

  // Drop delete trigger
  sql = sqlite3_mprintf("DROP TRIGGER IF EXISTS \"%s__crsql_dtrig\"",
                         escTable);
  sqlite3_free(escTable);
  if (!sql) return SQLITE_NOMEM;
  rc = sqlite3_exec(db, sql, 0, 0, 0);
  sqlite3_free(sql);
  return rc;
}

int crsql_remove_crr_clock_table_if_exists(sqlite3 *db, const char *table) {
  char *escTable = crsql_escape_ident(table);
  if (!escTable) return SQLITE_ERROR;

  char *sql;
  int rc;

  sql = sqlite3_mprintf("DROP TABLE IF EXISTS \"%s__crsql_clock\"", escTable);
  if (!sql) {
    sqlite3_free(escTable);
    return SQLITE_NOMEM;
  }
  rc = sqlite3_exec(db, sql, 0, 0, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    sqlite3_free(escTable);
    return rc;
  }

  sql = sqlite3_mprintf("DROP TABLE IF EXISTS \"%s__crsql_pks\"", escTable);
  sqlite3_free(escTable);
  if (!sql) return SQLITE_NOMEM;
  rc = sqlite3_exec(db, sql, 0, 0, 0);
  sqlite3_free(sql);
  return rc;
}
