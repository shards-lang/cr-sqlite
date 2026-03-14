#include "unpack-columns-vtab.h"

#include <string.h>

#include "pack-columns.h"

#define UNPACK_COL_CELL 0
#define UNPACK_COL_PACKAGE 1

typedef struct unpack_cursor unpack_cursor;
struct unpack_cursor {
  sqlite3_vtab_cursor base;
  int crsr;
  crsql_ColumnValue *unpacked;
  int numCols;
};

static int unpackConnect(sqlite3 *db, void *pAux, int argc,
                         const char *const *argv, sqlite3_vtab **ppVtab,
                         char **pzErr) {
  int rc = sqlite3_declare_vtab(
      db, "CREATE TABLE x(cell ANY, package BLOB hidden)");
  if (rc != SQLITE_OK) return rc;

  sqlite3_vtab *pNew = sqlite3_malloc(sizeof(sqlite3_vtab));
  if (!pNew) return SQLITE_NOMEM;
  memset(pNew, 0, sizeof(sqlite3_vtab));
  *ppVtab = pNew;
  return SQLITE_OK;
}

static int unpackDisconnect(sqlite3_vtab *pVtab) {
  sqlite3_free(pVtab);
  return SQLITE_OK;
}

static int unpackBestIndex(sqlite3_vtab *pVtab,
                           sqlite3_index_info *pIdxInfo) {
  for (int i = 0; i < pIdxInfo->nConstraint; i++) {
    if (!pIdxInfo->aConstraint[i].usable) continue;
    if (pIdxInfo->aConstraint[i].iColumn != UNPACK_COL_PACKAGE) {
      pVtab->zErrMsg = sqlite3_mprintf("must filter on package column");
      return SQLITE_MISUSE;
    }
    pIdxInfo->aConstraintUsage[i].argvIndex = 1;
    pIdxInfo->aConstraintUsage[i].omit = 1;
  }
  return SQLITE_OK;
}

static int unpackOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor) {
  unpack_cursor *pCur = sqlite3_malloc(sizeof(unpack_cursor));
  if (!pCur) return SQLITE_NOMEM;
  memset(pCur, 0, sizeof(unpack_cursor));
  *ppCursor = &pCur->base;
  return SQLITE_OK;
}

static int unpackClose(sqlite3_vtab_cursor *cur) {
  unpack_cursor *pCur = (unpack_cursor *)cur;
  crsql_free_column_values(pCur->unpacked, pCur->numCols);
  sqlite3_free(pCur);
  return SQLITE_OK;
}

static int unpackFilter(sqlite3_vtab_cursor *cur, int idxNum,
                        const char *idxStr, int argc, sqlite3_value **argv) {
  unpack_cursor *pCur = (unpack_cursor *)cur;

  // Free any previous unpacked data
  crsql_free_column_values(pCur->unpacked, pCur->numCols);
  pCur->unpacked = 0;
  pCur->numCols = 0;
  pCur->crsr = 0;

  if (argc < 1) {
    cur->pVtab->zErrMsg = sqlite3_mprintf("Zero args passed to filter");
    return SQLITE_MISUSE;
  }

  const unsigned char *blob =
      (const unsigned char *)sqlite3_value_blob(argv[0]);
  int blobLen = sqlite3_value_bytes(argv[0]);
  if (!blob || blobLen == 0) {
    return SQLITE_OK;  // empty result
  }

  int numCols = 0;
  pCur->unpacked = crsql_unpack_columns(blob, blobLen, &numCols);
  pCur->numCols = numCols;
  if (!pCur->unpacked && numCols > 0) {
    return SQLITE_ERROR;
  }

  return SQLITE_OK;
}

static int unpackNext(sqlite3_vtab_cursor *cur) {
  unpack_cursor *pCur = (unpack_cursor *)cur;
  pCur->crsr++;
  return SQLITE_OK;
}

static int unpackEof(sqlite3_vtab_cursor *cur) {
  unpack_cursor *pCur = (unpack_cursor *)cur;
  return pCur->crsr >= pCur->numCols ? 1 : 0;
}

static int unpackColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx,
                        int col) {
  unpack_cursor *pCur = (unpack_cursor *)cur;
  if (col != UNPACK_COL_CELL) {
    cur->pVtab->zErrMsg =
        sqlite3_mprintf("Selected a column besides cell! %d", col);
    return SQLITE_MISUSE;
  }

  if (!pCur->unpacked || pCur->crsr >= pCur->numCols) {
    cur->pVtab->zErrMsg = sqlite3_mprintf("No columns to unpack!");
    return SQLITE_ABORT;
  }

  crsql_ColumnValue *val = &pCur->unpacked[pCur->crsr];
  switch (val->type) {
    case CRSQL_CV_BLOB:
      sqlite3_result_blob(ctx, val->v.blob.data, val->v.blob.len,
                          SQLITE_TRANSIENT);
      break;
    case CRSQL_CV_FLOAT:
      sqlite3_result_double(ctx, val->v.floatVal);
      break;
    case CRSQL_CV_INTEGER:
      sqlite3_result_int64(ctx, val->v.integer);
      break;
    case CRSQL_CV_NULL:
      sqlite3_result_null(ctx);
      break;
    case CRSQL_CV_TEXT:
      sqlite3_result_text(ctx, val->v.text.data, val->v.text.len,
                          SQLITE_TRANSIENT);
      break;
  }
  return SQLITE_OK;
}

static int unpackRowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *pRowid) {
  unpack_cursor *pCur = (unpack_cursor *)cur;
  *pRowid = pCur->crsr;
  return SQLITE_OK;
}

static sqlite3_module crsql_unpackColumnsModule = {
    /* iVersion    */ 0,
    /* xCreate     */ 0,
    /* xConnect    */ unpackConnect,
    /* xBestIndex  */ unpackBestIndex,
    /* xDisconnect */ unpackDisconnect,
    /* xDestroy    */ 0,
    /* xOpen       */ unpackOpen,
    /* xClose      */ unpackClose,
    /* xFilter     */ unpackFilter,
    /* xNext       */ unpackNext,
    /* xEof        */ unpackEof,
    /* xColumn     */ unpackColumn,
    /* xRowid      */ unpackRowid,
    /* xUpdate     */ 0,
    /* xBegin      */ 0,
    /* xSync       */ 0,
    /* xCommit     */ 0,
    /* xRollback   */ 0,
    /* xFindMethod */ 0,
    /* xRename     */ 0,
    /* xSavepoint  */ 0,
    /* xRelease    */ 0,
    /* xRollbackTo */ 0,
    /* xShadowName */ 0,
};

int crsql_create_unpack_columns_module(sqlite3 *db) {
  return sqlite3_create_module_v2(db, "crsql_unpack_columns",
                                  &crsql_unpackColumnsModule, 0, 0);
}
