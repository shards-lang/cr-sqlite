#include "db-version.h"

#include <string.h>

#include "consts.h"
#include "crsqlite.h"
#include "util.h"

#define DB_VERSION_SCHEMA_VERSION 0

/**
 * Fetch db_version from storage by stepping the db version union statement.
 * On error, returns an error string via the return value (caller must
 * sqlite3_free). On success, returns NULL and pExtData->dbVersion is set.
 */
STATIC char *crsql_fetch_db_version_from_storage(sqlite3 *db,
                                                  crsql_ExtData *pExtData) {
  int schemaChanged;

  if (pExtData->pDbVersionStmt == 0) {
    schemaChanged = 1;
  } else {
    schemaChanged =
        crsql_fetchPragmaSchemaVersion(db, pExtData, DB_VERSION_SCHEMA_VERSION);
  }

  if (schemaChanged < 0) {
    return sqlite3_mprintf("failed to fetch the pragma schema version");
  }

  if (schemaChanged > 0) {
    int rc = crsql_recreate_db_version_stmt(db, pExtData);
    if (rc == -1) {
      // No clock tables -- clean db
      pExtData->dbVersion = 0;
      return 0;
    }
    if (rc != SQLITE_OK) {
      return sqlite3_mprintf("failed to recreate db version stmt: %d", rc);
    }
  }

  sqlite3_stmt *pStmt = pExtData->pDbVersionStmt;
  int rc = sqlite3_step(pStmt);

  if (rc == SQLITE_DONE) {
    if (sqlite3_reset(pStmt) != SQLITE_OK) {
      return sqlite3_mprintf("failed to reset db version stmt after DONE");
    }
    pExtData->dbVersion = MIN_POSSIBLE_DB_VERSION;
    return 0;
  }

  if (rc == SQLITE_ROW) {
    pExtData->dbVersion = sqlite3_column_int64(pStmt, 0);
    if (sqlite3_reset(pStmt) != SQLITE_OK) {
      return sqlite3_mprintf("failed to reset db version stmt after ROW");
    }
    return 0;
  }

  // Unexpected result code
  sqlite3_reset(pStmt);
  return sqlite3_mprintf("failed to step db version stmt: %d", rc);
}

int crsql_fill_db_version_if_needed(sqlite3 *db, crsql_ExtData *pExtData,
                                    char **errmsg) {
  int pv = crsql_fetchPragmaDataVersion(db, pExtData);
  if (pv == -1) {
    *errmsg = sqlite3_mprintf("failed to fetch PRAGMA data_version");
    return SQLITE_ERROR;
  }

  // Already cached and data version hasn't changed
  if (pExtData->dbVersion != -1 && pv == 0) {
    return SQLITE_OK;
  }

  char *err = crsql_fetch_db_version_from_storage(db, pExtData);
  if (err != 0) {
    *errmsg = err;
    return SQLITE_ERROR;
  }

  return SQLITE_OK;
}

sqlite_int64 crsql_next_db_version(sqlite3 *db, crsql_ExtData *pExtData,
                                   sqlite3_int64 merging_version,
                                   char **errmsg) {
  int rc = crsql_fill_db_version_if_needed(db, pExtData, errmsg);
  if (rc != SQLITE_OK) {
    return -1;
  }

  sqlite3_int64 ret = pExtData->dbVersion + 1;
  if (ret < pExtData->pendingDbVersion) {
    ret = pExtData->pendingDbVersion;
  }
  if (merging_version >= 0 && ret < merging_version) {
    ret = merging_version;
  }

  pExtData->pendingDbVersion = ret;
  return ret;
}

int crsql_recreate_db_version_stmt(sqlite3 *db, crsql_ExtData *pExtData) {
  sqlite3_stmt *pClockStmt = pExtData->pSelectClockTablesStmt;

  // Finalize old db version statement
  sqlite3_finalize(pExtData->pDbVersionStmt);
  pExtData->pDbVersionStmt = 0;

  // Collect clock table names
  int numTables = 0;
  int capacity = 8;
  char **tblNames = sqlite3_malloc(capacity * sizeof(char *));
  if (tblNames == 0) {
    sqlite3_reset(pClockStmt);
    return SQLITE_NOMEM;
  }

  for (;;) {
    int rc = sqlite3_step(pClockStmt);
    if (rc == SQLITE_DONE) {
      sqlite3_reset(pClockStmt);
      if (numTables == 0) {
        sqlite3_free(tblNames);
        return -1;  // no clock tables
      }
      break;
    }
    if (rc == SQLITE_ROW) {
      const char *name = (const char *)sqlite3_column_text(pClockStmt, 0);
      if (name == 0) {
        goto cleanup_error;
      }

      if (numTables >= capacity) {
        capacity *= 2;
        char **newBuf = sqlite3_realloc(tblNames, capacity * sizeof(char *));
        if (newBuf == 0) {
          goto cleanup_error;
        }
        tblNames = newBuf;
      }

      tblNames[numTables] = sqlite3_mprintf("%s", name);
      if (tblNames[numTables] == 0) {
        goto cleanup_error;
      }
      numTables++;
    } else {
      goto cleanup_error;
    }
  }

  {
    char *unionQuery =
        crsql_get_db_version_union_query(tblNames, numTables);

    // Free table names -- no longer needed
    for (int i = 0; i < numTables; i++) {
      sqlite3_free(tblNames[i]);
    }
    sqlite3_free(tblNames);

    if (unionQuery == 0) {
      return SQLITE_NOMEM;
    }

    int rc = sqlite3_prepare_v3(db, unionQuery, -1, SQLITE_PREPARE_PERSISTENT,
                                &(pExtData->pDbVersionStmt), 0);
    sqlite3_free(unionQuery);

    return rc;
  }

cleanup_error:
  sqlite3_reset(pClockStmt);
  for (int i = 0; i < numTables; i++) {
    sqlite3_free(tblNames[i]);
  }
  sqlite3_free(tblNames);
  return SQLITE_ERROR;
}
