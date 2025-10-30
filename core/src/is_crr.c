#include "is_crr.h"
#include <string.h>

// Check if a table is a CRR by looking for its insert trigger
int crsql_is_crr(sqlite3 *db, const char *table) {
  sqlite3_stmt *stmt = NULL;
  char *trigger_name = sqlite3_mprintf("%s__crsql_itrig", table);
  if (!trigger_name) return -1;

  int rc = sqlite3_prepare_v2(db,
    "SELECT count(*) FROM sqlite_master WHERE type = 'trigger' AND name = ?",
    -1, &stmt, NULL);

  if (rc != SQLITE_OK) {
    sqlite3_free(trigger_name);
    return -1;
  }

  rc = sqlite3_bind_text(stmt, 1, trigger_name, -1, SQLITE_TRANSIENT);
  sqlite3_free(trigger_name);

  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return -1;
  }

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return -1;
  }

  int count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  return (count > 0) ? 1 : 0;
}
