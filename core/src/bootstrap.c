#include "bootstrap.h"

#include <string.h>

#include "consts.h"
#include "crsqlite.h"

/**
 * Generate a UUID v4 using sqlite3_randomness.
 * Writes 16 bytes into out.
 */
STATIC void crsql_uuid(unsigned char *out) {
  sqlite3_randomness(SITE_ID_LEN, out);
  out[6] = (out[6] & 0x0f) | 0x40;
  out[8] = (out[8] & 0x3f) | 0x80;
}

/**
 * Check if a table exists in sqlite_master.
 * Sets *exists to 1 if found, 0 if not.
 * Returns SQLITE_OK on success.
 */
STATIC int crsql_has_table(sqlite3 *db, const char *tableName, int *exists) {
  sqlite3_stmt *pStmt = 0;
  int rc = sqlite3_prepare_v2(
      db,
      "SELECT 1 FROM sqlite_master WHERE type = 'table' AND tbl_name = ?", -1,
      &pStmt, 0);
  if (rc != SQLITE_OK) {
    return rc;
  }

  sqlite3_bind_text(pStmt, 1, tableName, -1, SQLITE_STATIC);
  rc = sqlite3_step(pStmt);

  if (rc == SQLITE_ROW) {
    *exists = 1;
    sqlite3_finalize(pStmt);
    return SQLITE_OK;
  } else if (rc == SQLITE_DONE) {
    *exists = 0;
    sqlite3_finalize(pStmt);
    return SQLITE_OK;
  }

  sqlite3_finalize(pStmt);
  return rc;
}

/**
 * Insert a new site_id into the site_id table with ordinal 0.
 * Writes the generated id into siteId (must be SITE_ID_LEN bytes).
 * Returns SQLITE_OK on success.
 */
STATIC int crsql_insert_site_id(sqlite3 *db, unsigned char *siteId) {
  sqlite3_stmt *pStmt = 0;
  int rc = sqlite3_prepare_v2(
      db,
      "INSERT INTO \"" TBL_SITE_ID "\" (site_id, ordinal) VALUES (?, 0)", -1,
      &pStmt, 0);
  if (rc != SQLITE_OK) {
    return rc;
  }

  crsql_uuid(siteId);
  sqlite3_bind_blob(pStmt, 1, siteId, SITE_ID_LEN, SQLITE_STATIC);
  rc = sqlite3_step(pStmt);
  sqlite3_finalize(pStmt);

  if (rc != SQLITE_DONE) {
    return rc;
  }
  return SQLITE_OK;
}

/**
 * Create the site_id table and its unique index, then insert a new site_id.
 */
STATIC int crsql_create_site_id_and_table(sqlite3 *db, unsigned char *siteId) {
  int rc = sqlite3_exec(
      db,
      "CREATE TABLE \"" TBL_SITE_ID
      "\" (site_id BLOB NOT NULL, ordinal INTEGER PRIMARY KEY);"
      "CREATE UNIQUE INDEX " TBL_SITE_ID "_site_id ON \"" TBL_SITE_ID
      "\" (site_id);",
      0, 0, 0);
  if (rc != SQLITE_OK) {
    return rc;
  }

  return crsql_insert_site_id(db, siteId);
}

int crsql_init_site_id(sqlite3 *db, unsigned char *ret) {
  int exists = 0;
  int rc = crsql_has_table(db, TBL_SITE_ID, &exists);
  if (rc != SQLITE_OK) {
    return rc;
  }

  if (!exists) {
    return crsql_create_site_id_and_table(db, ret);
  }

  sqlite3_stmt *pStmt = 0;
  rc = sqlite3_prepare_v2(
      db, "SELECT site_id FROM \"" TBL_SITE_ID "\" WHERE ordinal = 0", -1,
      &pStmt, 0);
  if (rc != SQLITE_OK) {
    return rc;
  }

  rc = sqlite3_step(pStmt);
  if (rc == SQLITE_DONE) {
    // Table exists but no row with ordinal 0 -- insert one
    sqlite3_finalize(pStmt);
    return crsql_insert_site_id(db, ret);
  } else if (rc == SQLITE_ROW) {
    const void *blob = sqlite3_column_blob(pStmt, 0);
    int blobLen = sqlite3_column_bytes(pStmt, 0);
    if (blob == 0 || blobLen != SITE_ID_LEN) {
      sqlite3_finalize(pStmt);
      return SQLITE_ERROR;
    }
    memcpy(ret, blob, SITE_ID_LEN);
    sqlite3_finalize(pStmt);
    return SQLITE_OK;
  }

  sqlite3_finalize(pStmt);
  return rc;
}

int crsql_init_peer_tracking_table(sqlite3 *db) {
  return sqlite3_exec(
      db,
      "CREATE TABLE IF NOT EXISTS crsql_tracked_peers ("
      "\"site_id\" BLOB NOT NULL, "
      "\"version\" INTEGER NOT NULL, "
      "\"seq\" INTEGER DEFAULT 0, "
      "\"tag\" INTEGER, "
      "\"event\" INTEGER, "
      "PRIMARY KEY (\"site_id\", \"tag\", \"event\")"
      ") STRICT;",
      0, 0, 0);
}

/**
 * Create the crsql_master schema table if it doesn't exist.
 * Uses a savepoint so a failure doesn't pollute the outer transaction.
 */
int crsql_create_schema_table_if_not_exists(sqlite3 *db) {
  int rc = sqlite3_exec(db, "SAVEPOINT crsql_create_schema_table;", 0, 0, 0);
  if (rc != SQLITE_OK) {
    return rc;
  }

  rc = sqlite3_exec(
      db,
      "CREATE TABLE IF NOT EXISTS \"" TBL_SCHEMA
      "\" (\"key\" TEXT PRIMARY KEY, \"value\" ANY);",
      0, 0, 0);

  if (rc == SQLITE_OK) {
    return sqlite3_exec(db, "RELEASE crsql_create_schema_table;", 0, 0, 0);
  }

  sqlite3_exec(db, "ROLLBACK;", 0, 0, 0);
  return SQLITE_ERROR;
}

/**
 * Inner migration logic.
 * isBlankSlate is true when this is the first time the extension opens the db.
 */
STATIC int crsql_maybe_update_db_inner(sqlite3 *db, int isBlankSlate,
                                        char **errmsg) {
  int recordedVersion = 0;

  if (isBlankSlate) {
    recordedVersion = CRSQLITE_VERSION;
  } else {
    sqlite3_stmt *pStmt = 0;
    int rc = sqlite3_prepare_v2(
        db, "SELECT value FROM crsql_master WHERE key = 'crsqlite_version'",
        -1, &pStmt, 0);
    if (rc != SQLITE_OK) {
      return rc;
    }

    rc = sqlite3_step(pStmt);
    if (rc == SQLITE_ROW) {
      recordedVersion = sqlite3_column_int(pStmt, 0);
    }
    sqlite3_finalize(pStmt);
  }

  if (recordedVersion < CRSQLITE_VERSION_0_15_0 && !isBlankSlate) {
    *errmsg = sqlite3_mprintf(
        "Opening a db created with cr-sqlite version %d is not supported. "
        "Upcoming release 0.15.0 is a breaking change.",
        recordedVersion);
    return SQLITE_ERROR;
  }

  // Write the db version if we migrated or this is a brand new db
  if (recordedVersion < CRSQLITE_VERSION || isBlankSlate) {
    sqlite3_stmt *pStmt = 0;
    int rc = sqlite3_prepare_v2(
        db,
        "INSERT OR REPLACE INTO crsql_master VALUES ('crsqlite_version', ?)",
        -1, &pStmt, 0);
    if (rc != SQLITE_OK) {
      return rc;
    }

    sqlite3_bind_int(pStmt, 1, CRSQLITE_VERSION);
    rc = sqlite3_step(pStmt);
    sqlite3_finalize(pStmt);

    if (rc != SQLITE_DONE) {
      return rc;
    }
  }

  return SQLITE_OK;
}

int crsql_maybe_update_db(sqlite3 *db, char **errmsg) {
  int hasSchemaTable = 0;
  int rc = crsql_has_table(db, TBL_SCHEMA, &hasSchemaTable);
  if (rc != SQLITE_OK) {
    return SQLITE_ERROR;
  }

  rc = crsql_create_schema_table_if_not_exists(db);
  if (rc != SQLITE_OK) {
    return rc;
  }

  rc = sqlite3_exec(db, "SAVEPOINT crsql_maybe_update_db;", 0, 0, 0);
  if (rc != SQLITE_OK) {
    return rc;
  }

  int isBlankSlate = (hasSchemaTable == 0) ? 1 : 0;
  rc = crsql_maybe_update_db_inner(db, isBlankSlate, errmsg);
  if (rc == SQLITE_OK) {
    sqlite3_exec(db, "RELEASE crsql_maybe_update_db;", 0, 0, 0);
    return SQLITE_OK;
  }

  sqlite3_exec(db, "ROLLBACK;", 0, 0, 0);
  return SQLITE_ERROR;
}

int crsql_is_crr(sqlite3 *db, const char *table) {
  sqlite3_stmt *pStmt = 0;
  int rc = sqlite3_prepare_v2(
      db,
      "SELECT count(*) FROM sqlite_master WHERE type = 'trigger' AND name = ?",
      -1, &pStmt, 0);
  if (rc != SQLITE_OK) {
    return -1;
  }

  char *triggerName = sqlite3_mprintf("%s__crsql_itrig", table);
  if (triggerName == 0) {
    sqlite3_finalize(pStmt);
    return -1;
  }

  sqlite3_bind_text(pStmt, 1, triggerName, -1, SQLITE_TRANSIENT);
  sqlite3_free(triggerName);

  rc = sqlite3_step(pStmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(pStmt);
    return -1;
  }

  int count = sqlite3_column_int(pStmt, 0);
  sqlite3_finalize(pStmt);

  return (count > 0) ? 1 : 0;
}
