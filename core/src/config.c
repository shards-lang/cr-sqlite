#include "config.h"
#include "ext-data.h"
#include <string.h>

// Set a configuration value
void crsql_config_set(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  if (argc < 2) {
    sqlite3_result_error(ctx, "Expected 2 arguments: name and value", -1);
    return;
  }

  const char *name = (const char *)sqlite3_value_text(argv[0]);
  sqlite3_value *value = argv[1];

  // Handle known config settings
  if (strcmp(name, MERGE_EQUAL_VALUES) == 0) {
    crsql_ExtData *ext_data = (crsql_ExtData *)sqlite3_user_data(ctx);
    ext_data->mergeEqualValues = sqlite3_value_int(value);
  } else {
    sqlite3_result_error(ctx, "Unknown setting name", -1);
    return;
  }

  // Persist to database
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  sqlite3_stmt *stmt = NULL;

  char *sql = sqlite3_mprintf(
    "INSERT OR REPLACE INTO crsql_master VALUES ('config.%s', ?) RETURNING value",
    name
  );

  if (!sql) {
    sqlite3_result_error(ctx, "Out of memory", -1);
    return;
  }

  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    sqlite3_result_error(ctx, "Could not persist config in database", -1);
    return;
  }

  rc = sqlite3_bind_value(stmt, 1, value);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt);
    sqlite3_result_error(ctx, "Could not bind value", -1);
    return;
  }

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    sqlite3_result_value(ctx, sqlite3_column_value(stmt, 0));
  } else {
    sqlite3_result_error(ctx, "Could not insert config", -1);
  }

  sqlite3_finalize(stmt);
}

// Get a configuration value
void crsql_config_get(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  if (argc < 1) {
    sqlite3_result_error(ctx, "Expected 1 argument: name", -1);
    return;
  }

  const char *name = (const char *)sqlite3_value_text(argv[0]);

  // Handle known config settings
  if (strcmp(name, MERGE_EQUAL_VALUES) == 0) {
    crsql_ExtData *ext_data = (crsql_ExtData *)sqlite3_user_data(ctx);
    sqlite3_result_int(ctx, ext_data->mergeEqualValues);
  } else {
    sqlite3_result_error(ctx, "Unknown setting name", -1);
  }
}
