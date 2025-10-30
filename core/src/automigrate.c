#include "automigrate.h"
#include "str_util.h"
#include "is_crr.h"
#include <string.h>
#include <ctype.h>

static const char *IS_UNIQUE_IDX_SQL =
  "SELECT \"unique\" FROM pragma_index_list(?) WHERE name = ?";
static const char *IDX_COLS_SQL =
  "SELECT name FROM pragma_index_info(?) ORDER BY seqno ASC";

// Simple string set (sorted array)
typedef struct {
  char **strings;
  int count;
  int capacity;
} StringSet;

static StringSet *string_set_create() {
  StringSet *set = sqlite3_malloc(sizeof(StringSet));
  if (!set) return NULL;
  set->strings = NULL;
  set->count = 0;
  set->capacity = 0;
  return set;
}

static void string_set_free(StringSet *set) {
  if (!set) return;
  for (int i = 0; i < set->count; i++) {
    sqlite3_free(set->strings[i]);
  }
  sqlite3_free(set->strings);
  sqlite3_free(set);
}

static int string_set_add(StringSet *set, const char *str) {
  // Check if already exists
  for (int i = 0; i < set->count; i++) {
    if (strcmp(set->strings[i], str) == 0) return SQLITE_OK;
  }

  // Expand if needed
  if (set->count >= set->capacity) {
    int new_cap = set->capacity == 0 ? 8 : set->capacity * 2;
    char **new_strings = sqlite3_realloc(set->strings, new_cap * sizeof(char*));
    if (!new_strings) return SQLITE_NOMEM;
    set->strings = new_strings;
    set->capacity = new_cap;
  }

  set->strings[set->count] = sqlite3_mprintf("%s", str);
  if (!set->strings[set->count]) return SQLITE_NOMEM;
  set->count++;
  return SQLITE_OK;
}

static int string_set_contains(StringSet *set, const char *str) {
  for (int i = 0; i < set->count; i++) {
    if (strcmp(set->strings[i], str) == 0) return 1;
  }
  return 0;
}

// Strip crsql_as_crr and crsql_fract_as_ordered lines from schema
static char *strip_crr_statements(const char *schema) {
  int len = strlen(schema);
  char *result = sqlite3_malloc(len + 1);
  if (!result) return NULL;

  char *out = result;
  const char *line_start = schema;

  while (*line_start) {
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n') line_end++;

    int line_len = line_end - line_start;
    char *line = sqlite3_malloc(line_len + 1);
    if (!line) {
      sqlite3_free(result);
      return NULL;
    }
    memcpy(line, line_start, line_len);
    line[line_len] = '\0';

    // Convert to lowercase for comparison
    char *lower = sqlite3_malloc(line_len + 1);
    if (!lower) {
      sqlite3_free(line);
      sqlite3_free(result);
      return NULL;
    }
    for (int i = 0; i <= line_len; i++) {
      lower[i] = tolower(line[i]);
    }

    int skip = (strstr(lower, "crsql_as_crr") != NULL ||
                strstr(lower, "crsql_fract_as_ordered") != NULL);

    sqlite3_free(lower);

    if (!skip) {
      memcpy(out, line, line_len);
      out += line_len;
      if (*line_end == '\n') {
        *out++ = '\n';
      }
    }

    sqlite3_free(line);

    if (*line_end == '\n') line_start = line_end + 1;
    else line_start = line_end;
  }

  *out = '\0';
  return result;
}

// Forward declarations
static int migrate_to(sqlite3 *local_db, sqlite3 *mem_db);
static int drop_tables(sqlite3 *local_db, StringSet *tables);
static int maybe_modify_table(sqlite3 *local_db, const char *table,
                              sqlite3 *mem_db);
static int drop_columns(sqlite3 *local_db, const char *table, StringSet *columns);
static int add_columns(sqlite3 *local_db, const char *table, StringSet *columns,
                      sqlite3 *mem_db);
static int add_column(sqlite3 *local_db, const char *table, const char *name,
                     const char *col_type, int notnull, sqlite3_value *dflt_val);
static int maybe_update_indices(sqlite3 *local_db, const char *table,
                               sqlite3 *mem_db);
static int drop_indices(sqlite3 *local_db, StringSet *dropped);
static int maybe_recreate_index(sqlite3 *local_db, const char *table,
                               const char *idx, sqlite3 *mem_db);
static int recreate_index(sqlite3 *local_db, const char *idx);

// Main automigrate implementation
static int automigrate_impl(sqlite3_context *ctx, sqlite3_value **args, int argc) {
  sqlite3 *local_db = sqlite3_context_db_handle(ctx);
  const char *desired_schema = (const char *)sqlite3_value_text(args[0]);

  if (!desired_schema) {
    sqlite3_result_error(ctx, "No schema provided", -1);
    return SQLITE_ERROR;
  }

  char *stripped_schema = strip_crr_statements(desired_schema);
  if (!stripped_schema) {
    sqlite3_result_error_nomem(ctx);
    return SQLITE_NOMEM;
  }

  // Open in-memory database
  sqlite3 *mem_db = NULL;
  int rc = sqlite3_open(":memory:", &mem_db);
  if (rc != SQLITE_OK) {
    sqlite3_free(stripped_schema);
    sqlite3_result_error(ctx, "Could not open temporary migration db", -1);
    return rc;
  }

  // Apply stripped schema to mem db
  char *err = NULL;
  rc = sqlite3_exec(mem_db, stripped_schema, NULL, NULL, &err);
  sqlite3_free(stripped_schema);

  if (rc != SQLITE_OK) {
    const char *mem_err = sqlite3_errmsg(mem_db);
    sqlite3_result_error(ctx, mem_err, -1);
    if (err) sqlite3_free(err);

    // Run cleanup if provided
    if (argc == 2) {
      const char *cleanup = (const char *)sqlite3_value_text(args[1]);
      if (cleanup) sqlite3_exec(mem_db, cleanup, NULL, NULL, NULL);
    }

    sqlite3_close(mem_db);
    return SQLITE_ERROR;
  }

  // Start transaction
  rc = sqlite3_exec(local_db, "SAVEPOINT automigrate_tables", NULL, NULL, &err);
  if (err) sqlite3_free(err);
  if (rc != SQLITE_OK) {
    if (argc == 2) {
      const char *cleanup = (const char *)sqlite3_value_text(args[1]);
      if (cleanup) sqlite3_exec(mem_db, cleanup, NULL, NULL, NULL);
    }
    sqlite3_close(mem_db);
    return rc;
  }

  // Perform migration
  rc = migrate_to(local_db, mem_db);

  if (rc != SQLITE_OK) {
    sqlite3_exec(local_db, "ROLLBACK", NULL, NULL, NULL);
    const char *mem_err = sqlite3_errmsg(mem_db);
    sqlite3_result_error(ctx, mem_err, -1);

    if (argc == 2) {
      const char *cleanup = (const char *)sqlite3_value_text(args[1]);
      if (cleanup) sqlite3_exec(mem_db, cleanup, NULL, NULL, NULL);
    }

    sqlite3_close(mem_db);
    return rc;
  }

  // Run cleanup
  if (argc == 2) {
    const char *cleanup = (const char *)sqlite3_value_text(args[1]);
    if (cleanup) {
      rc = sqlite3_exec(mem_db, cleanup, NULL, NULL, &err);
      if (err) sqlite3_free(err);
      if (rc != SQLITE_OK) {
        sqlite3_exec(local_db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(mem_db);
        return rc;
      }
    }
  }

  sqlite3_close(mem_db);

  // Apply full desired schema (includes CRR statements)
  if (desired_schema && *desired_schema) {
    rc = sqlite3_exec(local_db, desired_schema, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
      if (err) {
        sqlite3_result_error(ctx, err, -1);
        sqlite3_free(err);
      }
      sqlite3_exec(local_db, "ROLLBACK", NULL, NULL, NULL);
      return rc;
    }
    if (err) sqlite3_free(err);
  }

  // Commit transaction
  rc = sqlite3_exec(local_db, "RELEASE automigrate_tables", NULL, NULL, &err);
  if (err) sqlite3_free(err);

  return rc;
}

// Migrate local db to match mem db schema
static int migrate_to(sqlite3 *local_db, sqlite3 *mem_db) {
  const char *sql = "SELECT name FROM sqlite_master WHERE type = 'table' "
                    "AND name NOT LIKE 'sqlite_%' "
                    "AND name NOT LIKE 'crsql_%' "
                    "AND name NOT LIKE '__crsql_%' "
                    "AND name NOT LIKE '%__crsql_%'";

  StringSet *mem_tables = string_set_create();
  StringSet *removed_tables = string_set_create();
  StringSet *maybe_modified = string_set_create();

  if (!mem_tables || !removed_tables || !maybe_modified) {
    string_set_free(mem_tables);
    string_set_free(removed_tables);
    string_set_free(maybe_modified);
    return SQLITE_NOMEM;
  }

  // Fetch mem tables
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(mem_db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) goto cleanup;

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    string_set_add(mem_tables, name);
  }
  sqlite3_finalize(stmt);

  // Fetch local tables
  rc = sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) goto cleanup;

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    if (string_set_contains(mem_tables, name)) {
      string_set_add(maybe_modified, name);
    } else {
      string_set_add(removed_tables, name);
    }
  }
  sqlite3_finalize(stmt);
  stmt = NULL;

  // Drop removed tables
  rc = drop_tables(local_db, removed_tables);
  if (rc != SQLITE_OK) goto cleanup;

  // Modify existing tables
  for (int i = 0; i < maybe_modified->count; i++) {
    rc = maybe_modify_table(local_db, maybe_modified->strings[i], mem_db);
    if (rc != SQLITE_OK) goto cleanup;
  }

  rc = SQLITE_OK;

cleanup:
  if (stmt) sqlite3_finalize(stmt);
  string_set_free(mem_tables);
  string_set_free(removed_tables);
  string_set_free(maybe_modified);
  return rc;
}

static int drop_tables(sqlite3 *local_db, StringSet *tables) {
  for (int i = 0; i < tables->count; i++) {
    char *escaped = crsql_escape_ident(tables->strings[i]);
    char *sql = sqlite3_mprintf("DROP TABLE \"%s\"", escaped);
    sqlite3_free(escaped);

    if (!sql) return SQLITE_NOMEM;

    char *err = NULL;
    int rc = sqlite3_exec(local_db, sql, NULL, NULL, &err);
    sqlite3_free(sql);
    if (err) sqlite3_free(err);
    if (rc != SQLITE_OK) return rc;
  }
  return SQLITE_OK;
}

static int maybe_modify_table(sqlite3 *local_db, const char *table,
                              sqlite3 *mem_db) {
  StringSet *local_columns = string_set_create();
  StringSet *mem_columns = string_set_create();
  StringSet *removed_columns = string_set_create();
  StringSet *added_columns = string_set_create();

  if (!local_columns || !mem_columns || !removed_columns || !added_columns) {
    string_set_free(local_columns);
    string_set_free(mem_columns);
    string_set_free(removed_columns);
    string_set_free(added_columns);
    return SQLITE_NOMEM;
  }

  const char *sql = "SELECT name FROM pragma_table_info(?)";

  // Fetch mem columns
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(mem_db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) goto cleanup;

  sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    string_set_add(mem_columns, name);
  }
  sqlite3_finalize(stmt);

  // Fetch local columns
  rc = sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) goto cleanup;

  sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    string_set_add(local_columns, name);
    if (!string_set_contains(mem_columns, name)) {
      string_set_add(removed_columns, name);
    }
  }
  sqlite3_finalize(stmt);
  stmt = NULL;

  // Find added columns
  for (int i = 0; i < mem_columns->count; i++) {
    if (!string_set_contains(local_columns, mem_columns->strings[i])) {
      string_set_add(added_columns, mem_columns->strings[i]);
    }
  }

  // Check if table is a CRR
  int is_crr = crsql_is_crr(local_db, table);
  if (is_crr < 0) {
    rc = SQLITE_ERROR;
    goto cleanup;
  }

  // Begin alter if CRR
  if (is_crr) {
    rc = sqlite3_prepare_v2(local_db, "SELECT crsql_begin_alter(?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto cleanup;
  }

  // Drop columns
  rc = drop_columns(local_db, table, removed_columns);
  if (rc != SQLITE_OK) goto cleanup;

  // Add columns
  rc = add_columns(local_db, table, added_columns, mem_db);
  if (rc != SQLITE_OK) goto cleanup;

  // Update indices
  rc = maybe_update_indices(local_db, table, mem_db);
  if (rc != SQLITE_OK) goto cleanup;

  // Commit alter if CRR
  if (is_crr) {
    rc = sqlite3_prepare_v2(local_db, "SELECT crsql_commit_alter(?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto cleanup;
  }

  rc = SQLITE_OK;

cleanup:
  if (stmt) sqlite3_finalize(stmt);
  string_set_free(local_columns);
  string_set_free(mem_columns);
  string_set_free(removed_columns);
  string_set_free(added_columns);
  return rc;
}

static int drop_columns(sqlite3 *local_db, const char *table, StringSet *columns) {
  // Drop fractindex view if it exists
  char *escaped_table = crsql_escape_ident(table);
  char *sql = sqlite3_mprintf("DROP VIEW IF EXISTS \"%s_fractindex\"", escaped_table);
  sqlite3_free(escaped_table);

  if (!sql) return SQLITE_NOMEM;

  char *err = NULL;
  int rc = sqlite3_exec(local_db, sql, NULL, NULL, &err);
  sqlite3_free(sql);
  if (err) sqlite3_free(err);
  if (rc != SQLITE_OK) return rc;

  // Drop each column
  for (int i = 0; i < columns->count; i++) {
    char *escaped_tbl = crsql_escape_ident(table);
    char *escaped_col = crsql_escape_ident(columns->strings[i]);
    sql = sqlite3_mprintf("ALTER TABLE \"%s\" DROP \"%s\"", escaped_tbl, escaped_col);
    sqlite3_free(escaped_tbl);
    sqlite3_free(escaped_col);

    if (!sql) return SQLITE_NOMEM;

    err = NULL;
    rc = sqlite3_exec(local_db, sql, NULL, NULL, &err);
    sqlite3_free(sql);
    if (err) sqlite3_free(err);
    if (rc != SQLITE_OK) return rc;
  }

  return SQLITE_OK;
}

static int add_columns(sqlite3 *local_db, const char *table, StringSet *columns,
                      sqlite3 *mem_db) {
  if (columns->count == 0) return SQLITE_OK;

  // Build query with placeholders
  char *placeholders = sqlite3_malloc(1);
  placeholders[0] = '\0';
  for (int i = 0; i < columns->count; i++) {
    char *new_ph = sqlite3_mprintf("%s%s?", placeholders, i > 0 ? ", " : "");
    sqlite3_free(placeholders);
    placeholders = new_ph;
    if (!placeholders) return SQLITE_NOMEM;
  }

  char *sql = sqlite3_mprintf(
    "SELECT name, type, \"notnull\", dflt_value, pk "
    "FROM pragma_table_info(?) WHERE name IN (%s)",
    placeholders
  );
  sqlite3_free(placeholders);

  if (!sql) return SQLITE_NOMEM;

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(mem_db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return rc;

  sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  for (int i = 0; i < columns->count; i++) {
    sqlite3_bind_text(stmt, i + 2, columns->strings[i], -1, SQLITE_STATIC);
  }

  int processed = 0;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    int is_pk = sqlite3_column_int(stmt, 4) == 1;
    if (is_pk) {
      // Cannot add PK columns
      sqlite3_finalize(stmt);
      return SQLITE_MISUSE;
    }

    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    const char *col_type = (const char *)sqlite3_column_text(stmt, 1);
    int notnull = sqlite3_column_int(stmt, 2) == 1;
    sqlite3_value *dflt_val = sqlite3_column_value(stmt, 3);

    rc = add_column(local_db, table, name, col_type, notnull, dflt_val);
    if (rc != SQLITE_OK) {
      sqlite3_finalize(stmt);
      return rc;
    }
    processed++;
  }

  sqlite3_finalize(stmt);

  if (processed != columns->count) {
    return SQLITE_ERROR;
  }

  return SQLITE_OK;
}

static int add_column(sqlite3 *local_db, const char *table, const char *name,
                     const char *col_type, int notnull, sqlite3_value *dflt_val) {
  const char *dflt_str = "";
  char *dflt_formatted = NULL;

  if (sqlite3_value_type(dflt_val) != SQLITE_NULL) {
    const char *val_text = (const char *)sqlite3_value_text(dflt_val);
    dflt_formatted = sqlite3_mprintf("DEFAULT %s", val_text);
    if (!dflt_formatted) return SQLITE_NOMEM;
    dflt_str = dflt_formatted;
  }

  char *escaped_table = crsql_escape_ident(table);
  char *escaped_name = crsql_escape_ident(name);
  char *sql = sqlite3_mprintf(
    "ALTER TABLE \"%s\" ADD COLUMN \"%s\" %s %s %s",
    escaped_table, escaped_name, col_type,
    notnull ? "NOT NULL" : "",
    dflt_str
  );
  sqlite3_free(escaped_table);
  sqlite3_free(escaped_name);
  if (dflt_formatted) sqlite3_free(dflt_formatted);

  if (!sql) return SQLITE_NOMEM;

  char *err = NULL;
  int rc = sqlite3_exec(local_db, sql, NULL, NULL, &err);
  sqlite3_free(sql);
  if (err) sqlite3_free(err);

  return rc;
}

static int maybe_update_indices(sqlite3 *local_db, const char *table,
                               sqlite3 *mem_db) {
  const char *sql = "SELECT name FROM pragma_index_list(?) WHERE origin != 'pk'";

  StringSet *local_indices = string_set_create();
  StringSet *mem_indices = string_set_create();
  StringSet *removed = string_set_create();
  StringSet *maybe_modified = string_set_create();

  if (!local_indices || !mem_indices || !removed || !maybe_modified) {
    string_set_free(local_indices);
    string_set_free(mem_indices);
    string_set_free(removed);
    string_set_free(maybe_modified);
    return SQLITE_NOMEM;
  }

  // Fetch mem indices
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(mem_db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) goto cleanup;

  sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    string_set_add(mem_indices, name);
  }
  sqlite3_finalize(stmt);

  // Fetch local indices
  rc = sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) goto cleanup;

  sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    string_set_add(local_indices, name);
    if (!string_set_contains(mem_indices, name)) {
      string_set_add(removed, name);
    } else {
      string_set_add(maybe_modified, name);
    }
  }
  sqlite3_finalize(stmt);
  stmt = NULL;

  // Drop removed indices
  rc = drop_indices(local_db, removed);
  if (rc != SQLITE_OK) goto cleanup;

  // Check for modified indices
  for (int i = 0; i < maybe_modified->count; i++) {
    rc = maybe_recreate_index(local_db, table, maybe_modified->strings[i], mem_db);
    if (rc != SQLITE_OK) goto cleanup;
  }

  rc = SQLITE_OK;

cleanup:
  if (stmt) sqlite3_finalize(stmt);
  string_set_free(local_indices);
  string_set_free(mem_indices);
  string_set_free(removed);
  string_set_free(maybe_modified);
  return rc;
}

static int drop_indices(sqlite3 *local_db, StringSet *dropped) {
  for (int i = 0; i < dropped->count; i++) {
    char *escaped = crsql_escape_ident(dropped->strings[i]);
    char *sql = sqlite3_mprintf("DROP INDEX IF EXISTS \"%s\"", escaped);
    sqlite3_free(escaped);

    if (!sql) return SQLITE_NOMEM;

    char *err = NULL;
    int rc = sqlite3_exec(local_db, sql, NULL, NULL, &err);
    sqlite3_free(sql);
    if (err) sqlite3_free(err);
    if (rc != SQLITE_OK) return rc;
  }
  return SQLITE_OK;
}

static int maybe_recreate_index(sqlite3 *local_db, const char *table,
                               const char *idx, sqlite3 *mem_db) {
  // Check if unique flag differs
  sqlite3_stmt *local_stmt = NULL, *mem_stmt = NULL;
  int rc = sqlite3_prepare_v2(mem_db, IS_UNIQUE_IDX_SQL, -1, &mem_stmt, NULL);
  if (rc != SQLITE_OK) return rc;

  sqlite3_bind_text(mem_stmt, 1, table, -1, SQLITE_STATIC);
  sqlite3_bind_text(mem_stmt, 2, idx, -1, SQLITE_STATIC);

  rc = sqlite3_prepare_v2(local_db, IS_UNIQUE_IDX_SQL, -1, &local_stmt, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(mem_stmt);
    return rc;
  }

  sqlite3_bind_text(local_stmt, 1, table, -1, SQLITE_STATIC);
  sqlite3_bind_text(local_stmt, 2, idx, -1, SQLITE_STATIC);

  int mem_result = sqlite3_step(mem_stmt);
  int local_result = sqlite3_step(local_stmt);

  if (mem_result != SQLITE_ROW || local_result != SQLITE_ROW) {
    sqlite3_finalize(mem_stmt);
    sqlite3_finalize(local_stmt);
    return SQLITE_CONSTRAINT;
  }

  if (sqlite3_column_int(mem_stmt, 0) != sqlite3_column_int(local_stmt, 0)) {
    sqlite3_finalize(mem_stmt);
    sqlite3_finalize(local_stmt);
    return recreate_index(local_db, idx);
  }

  sqlite3_finalize(mem_stmt);
  sqlite3_finalize(local_stmt);

  // Check if column list differs
  rc = sqlite3_prepare_v2(mem_db, IDX_COLS_SQL, -1, &mem_stmt, NULL);
  if (rc != SQLITE_OK) return rc;

  sqlite3_bind_text(mem_stmt, 1, idx, -1, SQLITE_STATIC);

  rc = sqlite3_prepare_v2(local_db, IDX_COLS_SQL, -1, &local_stmt, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(mem_stmt);
    return rc;
  }

  sqlite3_bind_text(local_stmt, 1, idx, -1, SQLITE_STATIC);

  while (1) {
    mem_result = sqlite3_step(mem_stmt);
    local_result = sqlite3_step(local_stmt);

    if (mem_result != local_result) {
      sqlite3_finalize(mem_stmt);
      sqlite3_finalize(local_stmt);
      return recreate_index(local_db, idx);
    }

    if (mem_result != SQLITE_ROW) break;

    const char *mem_col = (const char *)sqlite3_column_text(mem_stmt, 0);
    const char *local_col = (const char *)sqlite3_column_text(local_stmt, 0);

    if (strcmp(mem_col, local_col) != 0) {
      sqlite3_finalize(mem_stmt);
      sqlite3_finalize(local_stmt);
      return recreate_index(local_db, idx);
    }
  }

  sqlite3_finalize(mem_stmt);
  sqlite3_finalize(local_stmt);

  return SQLITE_OK;
}

static int recreate_index(sqlite3 *local_db, const char *idx) {
  StringSet *indices = string_set_create();
  if (!indices) return SQLITE_NOMEM;

  string_set_add(indices, idx);
  int rc = drop_indices(local_db, indices);
  string_set_free(indices);

  return rc;
}

// Entry point
void crsql_automigrate(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  if (argc < 1) {
    sqlite3_result_error(ctx, "Expected at least 1 argument: schema to migrate to", -1);
    return;
  }

  int rc = automigrate_impl(ctx, argv, argc);

  if (rc != SQLITE_OK) {
    // Error already set by automigrate_impl if needed
    return;
  }

  sqlite3_result_text(ctx, "migration complete", -1, SQLITE_STATIC);
}
