#include "cl-set-vtab.h"

#include <string.h>

#include "util.h"

// Forward declarations for CRR creation
extern int crsql_create_crr(sqlite3 *db, const char *schemaName,
                            const char *tblName, int isCommitAlter, int noTx,
                            char **err);

typedef struct CLSetTab CLSetTab;
struct CLSetTab {
  sqlite3_vtab base;
  char *baseTableName;  // sqlite3_malloc'd
  char *dbName;         // sqlite3_malloc'd
  sqlite3 *db;
};

// Strip "_schema" suffix to get base table name
static const char *base_name_end(const char *vtabName) {
  int len = (int)strlen(vtabName);
  int suffixLen = (int)strlen("_schema");
  if (len > suffixLen) {
    return vtabName + (len - suffixLen);
  }
  return vtabName + len;
}

static char *get_base_name(const char *vtabName) {
  const char *end = base_name_end(vtabName);
  int baseLen = (int)(end - vtabName);
  char *result = sqlite3_malloc(baseLen + 1);
  if (result) {
    memcpy(result, vtabName, baseLen);
    result[baseLen] = '\0';
  }
  return result;
}

static int clsetConnectCreateShared(sqlite3 *db, sqlite3_vtab **ppVtab,
                                    const char *dbName,
                                    const char *tableName) {
  int rc = sqlite3_declare_vtab(
      db, "CREATE TABLE x(alteration TEXT HIDDEN, schema TEXT HIDDEN)");
  if (rc != SQLITE_OK) return rc;

  CLSetTab *pTab = sqlite3_malloc(sizeof(CLSetTab));
  if (!pTab) return SQLITE_NOMEM;
  memset(pTab, 0, sizeof(CLSetTab));
  pTab->baseTableName = get_base_name(tableName);
  pTab->dbName = sqlite3_mprintf("%s", dbName);
  pTab->db = db;
  *ppVtab = (sqlite3_vtab *)pTab;
  return SQLITE_OK;
}

static int clsetCreate(sqlite3 *db, void *pAux, int argc,
                       const char *const *argv, sqlite3_vtab **ppVtab,
                       char **pzErr) {
  if (argc < 4) {
    *pzErr = sqlite3_mprintf("clset requires column definitions");
    return SQLITE_ERROR;
  }

  // argv[0] = module name, argv[1] = db name, argv[2] = table name
  // argv[3..] = arguments (column definitions)
  const char *dbName = argv[1];
  const char *tableName = argv[2];

  // Check name ends with _schema
  int tblLen = (int)strlen(tableName);
  int suffixLen = (int)strlen("_schema");
  if (tblLen <= suffixLen ||
      strcmp(tableName + tblLen - suffixLen, "_schema") != 0) {
    *pzErr = sqlite3_mprintf(
        "%s MUST end with _schema. Two tables will be created: "
        "%s_schema for managing CRDT schemas and %s for storing data.",
        tableName, tableName, tableName);
    return SQLITE_ERROR;
  }

  int rc = clsetConnectCreateShared(db, ppVtab, dbName, tableName);
  if (rc != SQLITE_OK) return rc;

  // Build column definition from args
  // Join argv[3..argc] with commas
  char *tableDef = sqlite3_malloc(1);
  tableDef[0] = '\0';
  for (int i = 3; i < argc; i++) {
    char *newDef;
    if (i == 3) {
      newDef = sqlite3_mprintf("%s", argv[i]);
    } else {
      newDef = sqlite3_mprintf("%s,%s", tableDef, argv[i]);
    }
    sqlite3_free(tableDef);
    tableDef = newDef;
  }

  // Create the storage table
  char *baseName = get_base_name(tableName);
  char *escDb = crsql_escape_ident(dbName);
  char *escTbl = crsql_escape_ident(baseName);
  char *createSql = sqlite3_mprintf(
      "CREATE TABLE \"%s\".\"%s\" (%s)", escDb, escTbl, tableDef);
  sqlite3_free(escDb);
  sqlite3_free(escTbl);
  sqlite3_free(tableDef);

  if (!createSql) {
    sqlite3_free(baseName);
    return SQLITE_NOMEM;
  }

  rc = sqlite3_exec(db, createSql, 0, 0, 0);
  sqlite3_free(createSql);
  if (rc != SQLITE_OK) {
    sqlite3_free(baseName);
    return rc;
  }

  // Create CRR
  rc = crsql_create_crr(db, dbName, baseName, 0, 1, pzErr);
  sqlite3_free(baseName);

  if (rc != SQLITE_OK) {
    // Cleanup the vtab
    CLSetTab *pTab = (CLSetTab *)*ppVtab;
    sqlite3_free(pTab->baseTableName);
    sqlite3_free(pTab->dbName);
    sqlite3_free(pTab);
    *ppVtab = 0;
  }
  return rc;
}

static int clsetConnect(sqlite3 *db, void *pAux, int argc,
                        const char *const *argv, sqlite3_vtab **ppVtab,
                        char **pzErr) {
  if (argc < 3) return SQLITE_ERROR;
  return clsetConnectCreateShared(db, ppVtab, argv[1], argv[2]);
}

static int clsetDisconnect(sqlite3_vtab *pVtab) {
  CLSetTab *pTab = (CLSetTab *)pVtab;
  sqlite3_free(pTab->baseTableName);
  sqlite3_free(pTab->dbName);
  sqlite3_free(pTab);
  return SQLITE_OK;
}

static int clsetDestroy(sqlite3_vtab *pVtab) {
  CLSetTab *pTab = (CLSetTab *)pVtab;
  char *escTbl = crsql_escape_ident(pTab->baseTableName);
  char *escDb = crsql_escape_ident(pTab->dbName);
  char *sql = sqlite3_mprintf(
      "DROP TABLE \"%s\".\"%s\";"
      "DROP TABLE \"%s\".\"%s__crsql_clock\";"
      "DROP TABLE \"%s\".\"%s__crsql_pks\";",
      escDb, escTbl, escDb, escTbl, escDb, escTbl);
  sqlite3_free(escTbl);
  sqlite3_free(escDb);
  if (sql) {
    sqlite3_exec(pTab->db, sql, 0, 0, 0);
    sqlite3_free(sql);
  }
  sqlite3_free(pTab->baseTableName);
  sqlite3_free(pTab->dbName);
  sqlite3_free(pTab);
  return SQLITE_OK;
}

static int clsetBestIndex(sqlite3_vtab *pVtab,
                          sqlite3_index_info *pIdxInfo) {
  return SQLITE_OK;
}

static int clsetOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor) {
  sqlite3_vtab_cursor *pCur = sqlite3_malloc(sizeof(sqlite3_vtab_cursor));
  if (!pCur) return SQLITE_NOMEM;
  memset(pCur, 0, sizeof(sqlite3_vtab_cursor));
  *ppCursor = pCur;
  return SQLITE_OK;
}

static int clsetClose(sqlite3_vtab_cursor *cur) {
  sqlite3_free(cur);
  return SQLITE_OK;
}

static int clsetFilter(sqlite3_vtab_cursor *cur, int idxNum,
                       const char *idxStr, int argc, sqlite3_value **argv) {
  return SQLITE_OK;
}

static int clsetNext(sqlite3_vtab_cursor *cur) { return SQLITE_OK; }
static int clsetEof(sqlite3_vtab_cursor *cur) { return 1; }
static int clsetColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx,
                       int col) {
  return SQLITE_OK;
}
static int clsetRowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *pRowid) {
  return SQLITE_OK;
}
static int clsetBegin(sqlite3_vtab *pVtab) { return SQLITE_OK; }
static int clsetCommit(sqlite3_vtab *pVtab) { return SQLITE_OK; }
static int clsetRollback(sqlite3_vtab *pVtab) { return SQLITE_OK; }

static sqlite3_module crsql_clsetModule = {
    /* iVersion    */ 0,
    /* xCreate     */ clsetCreate,
    /* xConnect    */ clsetConnect,
    /* xBestIndex  */ clsetBestIndex,
    /* xDisconnect */ clsetDisconnect,
    /* xDestroy    */ clsetDestroy,
    /* xOpen       */ clsetOpen,
    /* xClose      */ clsetClose,
    /* xFilter     */ clsetFilter,
    /* xNext       */ clsetNext,
    /* xEof        */ clsetEof,
    /* xColumn     */ clsetColumn,
    /* xRowid      */ clsetRowid,
    /* xUpdate     */ 0,
    /* xBegin      */ clsetBegin,
    /* xSync       */ 0,
    /* xCommit     */ clsetCommit,
    /* xRollback   */ clsetRollback,
    /* xFindMethod */ 0,
    /* xRename     */ 0,
    /* xSavepoint  */ 0,
    /* xRelease    */ 0,
    /* xRollbackTo */ 0,
    /* xShadowName */ 0,
};

int crsql_create_cl_set_module(sqlite3 *db) {
  return sqlite3_create_module_v2(db, "clset", &crsql_clsetModule, 0, 0);
}
