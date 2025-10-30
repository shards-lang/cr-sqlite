#include "create_crr.h"
#include "tableinfo.h"
#include "is_crr.h"
#include "bootstrap.h"
#include "triggers.h"
#include "teardown.h"

// Forward declaration (backfill not yet ported)
int crsql_backfill_table(sqlite3 *db, const char *table, crsql_ColumnInfo **pks,
                         int pks_len, crsql_ColumnInfo **non_pks, int non_pks_len,
                         int is_commit_alter, int no_tx);

// Create a CRR
int crsql_create_crr(sqlite3 *db, const char *schema, const char *table,
                     int is_commit_alter, int no_tx, char **err) {
  // Check table compatibility
  int rc = crsql_is_table_compatible(db, table, err);
  if (rc != SQLITE_OK) {
    return rc;
  }

  // Already a CRR? Nothing to do
  int is_crr_result = crsql_is_crr(db, table);
  if (is_crr_result == 1) {
    return SQLITE_OK;
  } else if (is_crr_result < 0) {
    if (err) *err = sqlite3_mprintf("Failed to check if table is CRR");
    return SQLITE_ERROR;
  }

  // Extract table info
  crsql_TableInfo *table_info = crsql_extract_table_info(db, table);
  if (!table_info) {
    if (err) *err = sqlite3_mprintf("Failed to extract table info");
    return SQLITE_ERROR;
  }

  // Create clock table
  rc = crsql_create_clock_table(db, table_info, err);
  if (rc != SQLITE_OK) {
    crsql_free_table_info(table_info);
    return rc;
  }

  // Remove old triggers if they exist
  rc = crsql_remove_crr_triggers_if_exist(db, table);
  if (rc != SQLITE_OK) {
    crsql_free_table_info(table_info);
    return rc;
  }

  // Create triggers
  rc = crsql_create_triggers(db, table_info, err);
  if (rc != SQLITE_OK) {
    crsql_free_table_info(table_info);
    return rc;
  }

  // Backfill table
  rc = crsql_backfill_table(db, table, table_info->pks, table_info->pks_len,
                            table_info->non_pks, table_info->non_pks_len,
                            is_commit_alter, no_tx);
  if (rc != SQLITE_OK) {
    if (err) *err = sqlite3_mprintf("Failed to backfill table");
    crsql_free_table_info(table_info);
    return rc;
  }

  crsql_free_table_info(table_info);
  return SQLITE_OK;
}
