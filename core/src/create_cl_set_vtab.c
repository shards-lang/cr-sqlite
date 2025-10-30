#include "create_cl_set_vtab.h"
#include "create_crr.h"
#include "str_util.h"
#include <string.h>

// Virtual table structure
typedef struct {
  sqlite3_vtab base;
  char *base_table_name;
  char *db_name;
  sqlite3 *db;
} cl_set_vtab;

// Forward declarations
static int cl_set_create(sqlite3 *db, void *aux, int argc,
                        const char *const *argv, sqlite3_vtab **vtab,
                        char **err);
static int cl_set_connect(sqlite3 *db, void *aux, int argc,
                         const char *const *argv, sqlite3_vtab **vtab,
                         char **err);
static int cl_set_disconnect(sqlite3_vtab *vtab);
static int cl_set_destroy(sqlite3_vtab *vtab);
static int cl_set_best_index(sqlite3_vtab *vtab, sqlite3_index_info *info);
static int cl_set_open(sqlite3_vtab *vtab, sqlite3_vtab_cursor **cursor);
static int cl_set_close(sqlite3_vtab_cursor *cursor);
static int cl_set_filter(sqlite3_vtab_cursor *cursor, int idx_num,
                        const char *idx_str, int argc, sqlite3_value **argv);
static int cl_set_next(sqlite3_vtab_cursor *cursor);
static int cl_set_eof(sqlite3_vtab_cursor *cursor);
static int cl_set_column(sqlite3_vtab_cursor *cursor, sqlite3_context *ctx,
                        int col_num);
static int cl_set_rowid(sqlite3_vtab_cursor *cursor, sqlite3_int64 *row_id);
static int cl_set_begin(sqlite3_vtab *vtab);
static int cl_set_commit(sqlite3_vtab *vtab);
static int cl_set_rollback(sqlite3_vtab *vtab);

// Helper: Get base table name from virtual table name (remove _schema suffix)
static char *base_name_from_virtual_name(const char *virtual_name) {
  const char *suffix = "_schema";
  int suffix_len = strlen(suffix);
  int name_len = strlen(virtual_name);

  if (name_len < suffix_len || strcmp(virtual_name + name_len - suffix_len, suffix) != 0) {
    return NULL;
  }

  int base_len = name_len - suffix_len;
  char *base_name = sqlite3_malloc(base_len + 1);
  if (!base_name) return NULL;

  memcpy(base_name, virtual_name, base_len);
  base_name[base_len] = '\0';
  return base_name;
}

// Helper: Create the base storage table
static int create_clset_storage(sqlite3 *db, const char *db_name,
                                const char *base_table_name,
                                int argc, const char *const *argv,
                                char **err) {
  // Build table definition from arguments (skip module name, db name, table name)
  char *table_def = sqlite3_malloc(1);
  table_def[0] = '\0';

  for (int i = 3; i < argc; i++) {
    char *new_def;
    if (i == 3) {
      new_def = sqlite3_mprintf("%s", argv[i]);
    } else {
      new_def = sqlite3_mprintf("%s,%s", table_def, argv[i]);
    }
    sqlite3_free(table_def);
    table_def = new_def;
    if (!table_def) {
      if (err) *err = sqlite3_mprintf("Out of memory");
      return SQLITE_NOMEM;
    }
  }

  char *escaped_db = crsql_escape_ident(db_name);
  char *escaped_table = crsql_escape_ident(base_table_name);
  char *sql = sqlite3_mprintf(
    "CREATE TABLE \"%s\".\"%s\" (%s)",
    escaped_db, escaped_table, table_def
  );
  sqlite3_free(escaped_db);
  sqlite3_free(escaped_table);
  sqlite3_free(table_def);

  if (!sql) {
    if (err) *err = sqlite3_mprintf("Out of memory");
    return SQLITE_NOMEM;
  }

  char *exec_err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &exec_err);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    if (err && exec_err) {
      *err = exec_err;
    } else if (exec_err) {
      sqlite3_free(exec_err);
    }
  }

  return rc;
}

// Helper: Shared connect/create logic
static int connect_create_shared(sqlite3 *db, sqlite3_vtab **vtab,
                                 const char *db_name, const char *table_name,
                                 char **err) {
  int rc = sqlite3_declare_vtab(db,
    "CREATE TABLE x(alteration TEXT HIDDEN, schema TEXT HIDDEN)");
  if (rc != SQLITE_OK) return rc;

  char *base_table_name = base_name_from_virtual_name(table_name);
  if (!base_table_name) {
    if (err) *err = sqlite3_mprintf("Table name must end with _schema");
    return SQLITE_MISUSE;
  }

  cl_set_vtab *tab = sqlite3_malloc(sizeof(cl_set_vtab));
  if (!tab) {
    sqlite3_free(base_table_name);
    return SQLITE_NOMEM;
  }

  memset(tab, 0, sizeof(cl_set_vtab));
  tab->base_table_name = base_table_name;
  tab->db_name = sqlite3_mprintf("%s", db_name);
  tab->db = db;

  if (!tab->db_name) {
    sqlite3_free(tab->base_table_name);
    sqlite3_free(tab);
    return SQLITE_NOMEM;
  }

  *vtab = (sqlite3_vtab *)tab;
  return SQLITE_OK;
}

// Create virtual table
static int cl_set_create(sqlite3 *db, void *aux, int argc,
                        const char *const *argv, sqlite3_vtab **vtab,
                        char **err) {
  if (argc < 3) {
    if (err) *err = sqlite3_mprintf("Not enough arguments");
    return SQLITE_ERROR;
  }

  const char *db_name = argv[1];
  const char *table_name = argv[2];

  // Verify table name ends with _schema
  int name_len = strlen(table_name);
  if (name_len < 7 || strcmp(table_name + name_len - 7, "_schema") != 0) {
    if (err) {
      *err = sqlite3_mprintf(
        "%s MUST end with _schema. Two tables will be created: "
        "%s_schema for managing CRDT schemas and %.*s for storing data.",
        table_name, table_name, (int)(name_len - 7), table_name
      );
    }
    return SQLITE_ERROR;
  }

  int rc = connect_create_shared(db, vtab, db_name, table_name, err);
  if (rc != SQLITE_OK) {
    if (*vtab) {
      cl_set_vtab *tab = (cl_set_vtab *)*vtab;
      sqlite3_free(tab->base_table_name);
      sqlite3_free(tab->db_name);
      sqlite3_free(tab);
      *vtab = NULL;
    }
    return rc;
  }

  cl_set_vtab *tab = (cl_set_vtab *)*vtab;

  // Create base table
  rc = create_clset_storage(db, db_name, tab->base_table_name, argc, argv, err);
  if (rc != SQLITE_OK) {
    sqlite3_free(tab->base_table_name);
    sqlite3_free(tab->db_name);
    sqlite3_free(tab);
    *vtab = NULL;
    return rc;
  }

  // Convert to CRR
  rc = crsql_create_crr(db, db_name, tab->base_table_name, 0, 1, err);
  if (rc != SQLITE_OK) {
    sqlite3_free(tab->base_table_name);
    sqlite3_free(tab->db_name);
    sqlite3_free(tab);
    *vtab = NULL;
    return rc;
  }

  return SQLITE_OK;
}

// Connect to existing virtual table
static int cl_set_connect(sqlite3 *db, void *aux, int argc,
                         const char *const *argv, sqlite3_vtab **vtab,
                         char **err) {
  if (argc < 3) {
    if (err) *err = sqlite3_mprintf("Not enough arguments");
    return SQLITE_ERROR;
  }

  const char *db_name = argv[1];
  const char *table_name = argv[2];

  int rc = connect_create_shared(db, vtab, db_name, table_name, err);
  if (rc != SQLITE_OK && *vtab) {
    cl_set_vtab *tab = (cl_set_vtab *)*vtab;
    sqlite3_free(tab->base_table_name);
    sqlite3_free(tab->db_name);
    sqlite3_free(tab);
    *vtab = NULL;
  }

  return rc;
}

// Disconnect from virtual table
static int cl_set_disconnect(sqlite3_vtab *vtab) {
  if (!vtab) return SQLITE_OK;

  cl_set_vtab *tab = (cl_set_vtab *)vtab;
  sqlite3_free(tab->base_table_name);
  sqlite3_free(tab->db_name);
  sqlite3_free(tab);

  return SQLITE_OK;
}

// Destroy virtual table
static int cl_set_destroy(sqlite3_vtab *vtab) {
  cl_set_vtab *tab = (cl_set_vtab *)vtab;

  char *escaped_db = crsql_escape_ident(tab->db_name);
  char *escaped_table = crsql_escape_ident(tab->base_table_name);
  char *sql = sqlite3_mprintf(
    "DROP TABLE \"%s\".\"%s\"; "
    "DROP TABLE \"%s\".\"%s__crsql_clock\"; "
    "DROP TABLE \"%s\".\"%s__crsql_pks\";",
    escaped_db, escaped_table,
    escaped_db, escaped_table,
    escaped_db, escaped_table
  );
  sqlite3_free(escaped_db);
  sqlite3_free(escaped_table);

  if (!sql) {
    cl_set_disconnect(vtab);
    return SQLITE_NOMEM;
  }

  char *err = NULL;
  int rc = sqlite3_exec(tab->db, sql, NULL, NULL, &err);
  sqlite3_free(sql);
  if (err) sqlite3_free(err);

  cl_set_disconnect(vtab);
  return rc;
}

// Best index (no-op)
static int cl_set_best_index(sqlite3_vtab *vtab, sqlite3_index_info *info) {
  return SQLITE_OK;
}

// Open cursor
static int cl_set_open(sqlite3_vtab *vtab, sqlite3_vtab_cursor **cursor) {
  sqlite3_vtab_cursor *crsr = sqlite3_malloc(sizeof(sqlite3_vtab_cursor));
  if (!crsr) return SQLITE_NOMEM;

  memset(crsr, 0, sizeof(sqlite3_vtab_cursor));
  *cursor = crsr;
  return SQLITE_OK;
}

// Close cursor
static int cl_set_close(sqlite3_vtab_cursor *cursor) {
  sqlite3_free(cursor);
  return SQLITE_OK;
}

// Filter (no-op)
static int cl_set_filter(sqlite3_vtab_cursor *cursor, int idx_num,
                        const char *idx_str, int argc, sqlite3_value **argv) {
  return SQLITE_OK;
}

// Next (no-op)
static int cl_set_next(sqlite3_vtab_cursor *cursor) {
  return SQLITE_OK;
}

// EOF (always true, no rows)
static int cl_set_eof(sqlite3_vtab_cursor *cursor) {
  return 1;
}

// Column (no-op)
static int cl_set_column(sqlite3_vtab_cursor *cursor, sqlite3_context *ctx,
                        int col_num) {
  return SQLITE_OK;
}

// Rowid (no-op)
static int cl_set_rowid(sqlite3_vtab_cursor *cursor, sqlite3_int64 *row_id) {
  return SQLITE_OK;
}

// Begin transaction (no-op)
static int cl_set_begin(sqlite3_vtab *vtab) {
  return SQLITE_OK;
}

// Commit transaction (no-op)
static int cl_set_commit(sqlite3_vtab *vtab) {
  return SQLITE_OK;
}

// Rollback transaction (no-op)
static int cl_set_rollback(sqlite3_vtab *vtab) {
  return SQLITE_OK;
}

// Module definition
static sqlite3_module cl_set_module = {
  .iVersion = 0,
  .xCreate = cl_set_create,
  .xConnect = cl_set_connect,
  .xBestIndex = cl_set_best_index,
  .xDisconnect = cl_set_disconnect,
  .xDestroy = cl_set_destroy,
  .xOpen = cl_set_open,
  .xClose = cl_set_close,
  .xFilter = cl_set_filter,
  .xNext = cl_set_next,
  .xEof = cl_set_eof,
  .xColumn = cl_set_column,
  .xRowid = cl_set_rowid,
  .xUpdate = NULL,
  .xBegin = cl_set_begin,
  .xSync = NULL,
  .xCommit = cl_set_commit,
  .xRollback = cl_set_rollback,
  .xFindFunction = NULL,
  .xRename = NULL,
  .xSavepoint = NULL,
  .xRelease = NULL,
  .xRollbackTo = NULL,
  .xShadowName = NULL,
};

// Create the module
int crsql_create_cl_set_module(sqlite3 *db) {
  return sqlite3_create_module_v2(db, "clset", &cl_set_module, NULL, NULL);
}
