#include "stmt_cache.h"

// Reset a cached statement
int crsql_reset_cached_stmt(sqlite3_stmt *stmt) {
  if (!stmt) return SQLITE_OK;

  int rc = sqlite3_clear_bindings(stmt);
  if (rc != SQLITE_OK) return rc;

  return sqlite3_reset(stmt);
}
