#include "db_version.h"
#include "consts.h"
#include <string.h>

#define DB_VERSION_SCHEMA_VERSION 0

// Forward declaration
static int fetch_db_version_from_storage(sqlite3 *db, crsql_ExtData *ext_data, char **errmsg);

// Fill dbVersion if needed
int crsql_fill_db_version_if_needed(sqlite3 *db, crsql_ExtData *ext_data, char **errmsg) {
  // Check if data has changed
  int rc = crsql_fetchPragmaDataVersion(db, ext_data);
  if (rc == -1) {
    if (errmsg) *errmsg = sqlite3_mprintf("failed to fetch PRAGMA data_version");
    return SQLITE_ERROR;
  }

  // If dbVersion is already set and data hasn't changed, we're done
  if (ext_data->dbVersion != -1 && rc == 0) {
    return SQLITE_OK;
  }

  // Need to fetch from storage
  return fetch_db_version_from_storage(db, ext_data, errmsg);
}

// Get next db version
sqlite3_int64 crsql_next_db_version(sqlite3 *db, crsql_ExtData *ext_data,
                                    sqlite3_int64 merging_version, char **errmsg) {
  int rc = crsql_fill_db_version_if_needed(db, ext_data, errmsg);
  if (rc != SQLITE_OK) {
    return -1;
  }

  sqlite3_int64 ret = ext_data->dbVersion + 1;

  // If pending version is higher, use it
  if (ret < ext_data->pendingDbVersion) {
    ret = ext_data->pendingDbVersion;
  }

  // If merging version is provided and higher, use it
  if (merging_version > 0 && ret < merging_version) {
    ret = merging_version;
  }

  ext_data->pendingDbVersion = ret;
  return ret;
}

// Fetch db_version from storage
static int fetch_db_version_from_storage(sqlite3 *db, crsql_ExtData *ext_data, char **errmsg) {
  // Check if schema changed or stmt doesn't exist
  int schema_changed;
  if (ext_data->pDbVersionStmt == NULL) {
    schema_changed = 1;
  } else {
    schema_changed = crsql_fetchPragmaSchemaVersion(db, ext_data, DB_VERSION_SCHEMA_VERSION);
  }

  if (schema_changed < 0) {
    if (errmsg) *errmsg = sqlite3_mprintf("failed to fetch the pragma schema version");
    return SQLITE_ERROR;
  }

  // If schema changed, recreate the statement
  if (schema_changed > 0) {
    int rc = crsql_recreate_db_version_stmt(db, ext_data);

    // SQLITE_DONE means no clock tables exist (clean db)
    if (rc == SQLITE_DONE) {
      ext_data->dbVersion = 0;
      return SQLITE_OK;
    }

    if (rc != SQLITE_OK) {
      if (errmsg) *errmsg = sqlite3_mprintf("failed to recreate db version stmt: %d", rc);
      return SQLITE_ERROR;
    }
  }

  // Execute the statement
  int rc = sqlite3_step(ext_data->pDbVersionStmt);

  if (rc == SQLITE_DONE) {
    // No rows - fresh db with min starting version
    sqlite3_reset(ext_data->pDbVersionStmt);
    ext_data->dbVersion = MIN_POSSIBLE_DB_VERSION;
    return SQLITE_OK;
  }

  if (rc == SQLITE_ROW) {
    // Got a row - it's our db version
    ext_data->dbVersion = sqlite3_column_int64(ext_data->pDbVersionStmt, 0);
    sqlite3_reset(ext_data->pDbVersionStmt);
    return SQLITE_OK;
  }

  // Something went wrong
  sqlite3_reset(ext_data->pDbVersionStmt);
  if (errmsg) *errmsg = sqlite3_mprintf("failed to step db version stmt: %d", rc);
  return SQLITE_ERROR;
}
