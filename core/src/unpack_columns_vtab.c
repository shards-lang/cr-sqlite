#include "unpack_columns_vtab.h"
#include "pack_columns.h"
#include <string.h>

// Column indices
#define COLUMN_CELL 0
#define COLUMN_PACKAGE 1

// Virtual table structure (empty, no state needed)
typedef struct {
  sqlite3_vtab base;
} unpack_columns_vtab;

// Cursor structure
typedef struct {
  sqlite3_vtab_cursor base;
  crsql_ColumnValue *unpacked;  // Array of unpacked values
  int num_cols;                 // Number of unpacked columns
  int cursor;                   // Current position in array
} unpack_columns_cursor;

// Forward declarations
static int unpack_columns_connect(sqlite3 *db, void *aux, int argc,
                                  const char *const *argv, sqlite3_vtab **vtab,
                                  char **err);
static int unpack_columns_disconnect(sqlite3_vtab *vtab);
static int unpack_columns_best_index(sqlite3_vtab *vtab, sqlite3_index_info *info);
static int unpack_columns_open(sqlite3_vtab *vtab, sqlite3_vtab_cursor **cursor);
static int unpack_columns_close(sqlite3_vtab_cursor *cursor);
static int unpack_columns_filter(sqlite3_vtab_cursor *cursor, int idx_num,
                                 const char *idx_str, int argc,
                                 sqlite3_value **argv);
static int unpack_columns_next(sqlite3_vtab_cursor *cursor);
static int unpack_columns_eof(sqlite3_vtab_cursor *cursor);
static int unpack_columns_column(sqlite3_vtab_cursor *cursor, sqlite3_context *ctx,
                                 int col_num);
static int unpack_columns_rowid(sqlite3_vtab_cursor *cursor, sqlite3_int64 *row_id);

// Connect to virtual table
static int unpack_columns_connect(sqlite3 *db, void *aux, int argc,
                                  const char *const *argv, sqlite3_vtab **vtab,
                                  char **err) {
  int rc = sqlite3_declare_vtab(db, "CREATE TABLE x(cell ANY, package BLOB hidden)");
  if (rc != SQLITE_OK) return rc;

  unpack_columns_vtab *tab = sqlite3_malloc(sizeof(unpack_columns_vtab));
  if (!tab) return SQLITE_NOMEM;

  memset(tab, 0, sizeof(unpack_columns_vtab));
  *vtab = (sqlite3_vtab *)tab;

  sqlite3_vtab_config(db, SQLITE_VTAB_INNOCUOUS);

  return SQLITE_OK;
}

// Disconnect from virtual table
static int unpack_columns_disconnect(sqlite3_vtab *vtab) {
  sqlite3_free(vtab);
  return SQLITE_OK;
}

// Best index - require package column constraint
static int unpack_columns_best_index(sqlite3_vtab *vtab, sqlite3_index_info *info) {
  for (int i = 0; i < info->nConstraint; i++) {
    if (!info->aConstraint[i].usable) continue;

    if (info->aConstraint[i].iColumn != COLUMN_PACKAGE) {
      vtab->zErrMsg = sqlite3_mprintf("No package column specified");
      return SQLITE_MISUSE;
    }

    info->aConstraintUsage[i].argvIndex = 1;
    info->aConstraintUsage[i].omit = 1;
  }

  return SQLITE_OK;
}

// Open cursor
static int unpack_columns_open(sqlite3_vtab *vtab, sqlite3_vtab_cursor **cursor) {
  unpack_columns_cursor *crsr = sqlite3_malloc(sizeof(unpack_columns_cursor));
  if (!crsr) return SQLITE_NOMEM;

  memset(crsr, 0, sizeof(unpack_columns_cursor));
  crsr->unpacked = NULL;
  crsr->num_cols = 0;
  crsr->cursor = 0;

  *cursor = (sqlite3_vtab_cursor *)crsr;
  return SQLITE_OK;
}

// Close cursor
static int unpack_columns_close(sqlite3_vtab_cursor *cursor) {
  unpack_columns_cursor *crsr = (unpack_columns_cursor *)cursor;
  if (crsr->unpacked) {
    crsql_free_unpacked_columns(crsr->unpacked, crsr->num_cols);
  }
  sqlite3_free(crsr);
  return SQLITE_OK;
}

// Filter - unpack the package blob
static int unpack_columns_filter(sqlite3_vtab_cursor *cursor, int idx_num,
                                 const char *idx_str, int argc,
                                 sqlite3_value **argv) {
  unpack_columns_cursor *crsr = (unpack_columns_cursor *)cursor;

  if (argc < 1) {
    cursor->pVtab->zErrMsg = sqlite3_mprintf("Zero args passed to filter");
    return SQLITE_MISUSE;
  }

  // Free any existing unpacked data
  if (crsr->unpacked) {
    crsql_free_unpacked_columns(crsr->unpacked, crsr->num_cols);
    crsr->unpacked = NULL;
    crsr->num_cols = 0;
  }

  // Unpack the blob
  const unsigned char *blob = sqlite3_value_blob(argv[0]);
  int blob_len = sqlite3_value_bytes(argv[0]);

  crsr->unpacked = crsql_unpack_columns(blob, blob_len, &crsr->num_cols);
  if (!crsr->unpacked) {
    return SQLITE_ERROR;
  }

  crsr->cursor = 0;
  return SQLITE_OK;
}

// Next row
static int unpack_columns_next(sqlite3_vtab_cursor *cursor) {
  unpack_columns_cursor *crsr = (unpack_columns_cursor *)cursor;
  crsr->cursor++;
  return SQLITE_OK;
}

// Check if at end of data
static int unpack_columns_eof(sqlite3_vtab_cursor *cursor) {
  unpack_columns_cursor *crsr = (unpack_columns_cursor *)cursor;
  if (!crsr->unpacked) return 1;
  return (crsr->cursor >= crsr->num_cols) ? 1 : 0;
}

// Get column value
static int unpack_columns_column(sqlite3_vtab_cursor *cursor, sqlite3_context *ctx,
                                 int col_num) {
  unpack_columns_cursor *crsr = (unpack_columns_cursor *)cursor;

  if (col_num != COLUMN_CELL) {
    cursor->pVtab->zErrMsg = sqlite3_mprintf("Selected a column besides cell: %d",
                                            col_num);
    return SQLITE_MISUSE;
  }

  if (!crsr->unpacked) {
    cursor->pVtab->zErrMsg = sqlite3_mprintf("No columns to unpack");
    return SQLITE_ABORT;
  }

  crsql_ColumnValue *val = &crsr->unpacked[crsr->cursor];

  switch (val->type) {
    case CRSQL_COLUMN_BLOB:
      sqlite3_result_blob(ctx, val->val.blob.data, val->val.blob.len, SQLITE_TRANSIENT);
      break;

    case CRSQL_COLUMN_FLOAT:
      sqlite3_result_double(ctx, val->val.f64);
      break;

    case CRSQL_COLUMN_INTEGER:
      sqlite3_result_int64(ctx, val->val.i64);
      break;

    case CRSQL_COLUMN_NULL:
      sqlite3_result_null(ctx);
      break;

    case CRSQL_COLUMN_TEXT:
      sqlite3_result_text(ctx, val->val.text.data, val->val.text.len, SQLITE_TRANSIENT);
      break;

    default:
      return SQLITE_ERROR;
  }

  return SQLITE_OK;
}

// Get rowid
static int unpack_columns_rowid(sqlite3_vtab_cursor *cursor, sqlite3_int64 *row_id) {
  unpack_columns_cursor *crsr = (unpack_columns_cursor *)cursor;
  *row_id = crsr->cursor;
  return SQLITE_OK;
}

// Module definition
static sqlite3_module unpack_columns_module = {
  .iVersion = 0,
  .xCreate = NULL,
  .xConnect = unpack_columns_connect,
  .xBestIndex = unpack_columns_best_index,
  .xDisconnect = unpack_columns_disconnect,
  .xDestroy = NULL,
  .xOpen = unpack_columns_open,
  .xClose = unpack_columns_close,
  .xFilter = unpack_columns_filter,
  .xNext = unpack_columns_next,
  .xEof = unpack_columns_eof,
  .xColumn = unpack_columns_column,
  .xRowid = unpack_columns_rowid,
  .xUpdate = NULL,
  .xBegin = NULL,
  .xSync = NULL,
  .xCommit = NULL,
  .xRollback = NULL,
  .xFindFunction = NULL,
  .xRename = NULL,
  .xSavepoint = NULL,
  .xRelease = NULL,
  .xRollbackTo = NULL,
  .xShadowName = NULL,
};

// Create the module
int crsql_create_unpack_columns_module(sqlite3 *db) {
  return sqlite3_create_module_v2(db, "crsql_unpack_columns",
                                  &unpack_columns_module, NULL, NULL);
}
