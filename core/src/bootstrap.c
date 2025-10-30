#include "bootstrap.h"
#include "consts.h"
#include "str_util.h"
#include <string.h>

// Helper to check if a table exists
static int has_table(sqlite3 *db, const char *table_name, int *exists) {
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db,
    "SELECT 1 FROM sqlite_master WHERE type='table' AND tbl_name=?",
    -1, &stmt, NULL);

  if (rc != SQLITE_OK) return rc;

  rc = sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return rc;
  }

  rc = sqlite3_step(stmt);
  *exists = (rc == SQLITE_ROW);
  sqlite3_finalize(stmt);

  return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

// Generate UUID v4
void crsql_gen_uuid(unsigned char *blob) {
  sqlite3_randomness(16, blob);
  // Set version bits (v4)
  blob[6] = (blob[6] & 0x0f) | 0x40;
  // Set variant bits (RFC 4122)
  blob[8] = (blob[8] & 0x3f) | 0x80;
}

// Insert a new site_id
static int insert_site_id(sqlite3 *db, unsigned char *site_id) {
  sqlite3_stmt *stmt = NULL;
  char *sql = sqlite3_mprintf("INSERT INTO \"%w\" (site_id, ordinal) VALUES (?, 0)", TBL_SITE_ID);
  if (!sql) return SQLITE_NOMEM;

  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return rc;

  crsql_gen_uuid(site_id);
  rc = sqlite3_bind_blob(stmt, 1, site_id, SITE_ID_LEN, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return rc;
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

// Create site_id table and generate site_id
static int create_site_id_and_site_id_table(sqlite3 *db, unsigned char *site_id) {
  char *sql = sqlite3_mprintf(
    "CREATE TABLE \"%w\" (site_id BLOB NOT NULL, ordinal INTEGER PRIMARY KEY);"
    "CREATE UNIQUE INDEX %w_site_id ON \"%w\" (site_id);",
    TBL_SITE_ID, TBL_SITE_ID, TBL_SITE_ID
  );
  if (!sql) return SQLITE_NOMEM;

  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    if (err) sqlite3_free(err);
    return rc;
  }

  return insert_site_id(db, site_id);
}

// Initialize or load site_id
int crsql_init_site_id(sqlite3 *db, unsigned char *ret) {
  int exists = 0;
  int rc = has_table(db, TBL_SITE_ID, &exists);
  if (rc != SQLITE_OK) return rc;

  if (!exists) {
    return create_site_id_and_site_id_table(db, ret);
  }

  // Table exists, try to load site_id
  sqlite3_stmt *stmt = NULL;
  char *sql = sqlite3_mprintf("SELECT site_id FROM \"%w\" WHERE ordinal = 0", TBL_SITE_ID);
  if (!sql) return SQLITE_NOMEM;

  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return rc;

  rc = sqlite3_step(stmt);

  if (rc == SQLITE_DONE) {
    // No site_id exists, create one
    sqlite3_finalize(stmt);
    return insert_site_id(db, ret);
  } else if (rc == SQLITE_ROW) {
    // Load existing site_id
    const void *blob = sqlite3_column_blob(stmt, 0);
    int blob_len = sqlite3_column_bytes(stmt, 0);

    if (blob_len == SITE_ID_LEN) {
      memcpy(ret, blob, SITE_ID_LEN);
      sqlite3_finalize(stmt);
      return SQLITE_OK;
    } else {
      sqlite3_finalize(stmt);
      return SQLITE_ERROR;
    }
  }

  sqlite3_finalize(stmt);
  return rc;
}

// Create peer tracking table
int crsql_init_peer_tracking_table(sqlite3 *db) {
  char *err = NULL;
  int rc = sqlite3_exec(db,
    "CREATE TABLE IF NOT EXISTS crsql_tracked_peers ("
    "  \"site_id\" BLOB NOT NULL,"
    "  \"version\" INTEGER NOT NULL,"
    "  \"seq\" INTEGER DEFAULT 0,"
    "  \"tag\" INTEGER,"
    "  \"event\" INTEGER,"
    "  PRIMARY KEY (\"site_id\", \"tag\", \"event\")"
    ") STRICT;",
    NULL, NULL, &err);

  if (err) sqlite3_free(err);
  return rc;
}

// Create schema table
int crsql_create_schema_table_if_not_exists(sqlite3 *db) {
  char *err = NULL;
  int rc = sqlite3_exec(db, "SAVEPOINT crsql_create_schema_table;", NULL, NULL, NULL);
  if (rc != SQLITE_OK) return rc;

  char *sql = sqlite3_mprintf(
    "CREATE TABLE IF NOT EXISTS \"%w\" (\"key\" TEXT PRIMARY KEY, \"value\" ANY);",
    TBL_SCHEMA
  );
  if (!sql) {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return SQLITE_NOMEM;
  }

  rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    if (err) sqlite3_free(err);
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return rc;
  }

  return sqlite3_exec(db, "RELEASE crsql_create_schema_table;", NULL, NULL, NULL);
}

// Check and migrate database version
int crsql_maybe_update_db(sqlite3 *db, char **err_msg) {
  int exists = 0;
  int rc = has_table(db, TBL_SCHEMA, &exists);
  if (rc != SQLITE_OK) return rc;

  int is_blank_slate = !exists;

  // Create schema table if needed
  rc = crsql_create_schema_table_if_not_exists(db);
  if (rc != SQLITE_OK) return rc;

  // Start transaction
  rc = sqlite3_exec(db, "SAVEPOINT crsql_maybe_update_db;", NULL, NULL, NULL);
  if (rc != SQLITE_OK) return rc;

  // Get recorded version
  int recorded_version = 0;

  if (is_blank_slate) {
    recorded_version = CRSQLITE_VERSION;
  } else {
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db,
      "SELECT value FROM crsql_master WHERE key = 'crsqlite_version'",
      -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
      sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
      return rc;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      recorded_version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }

  // Check version compatibility
  if (recorded_version < CRSQLITE_VERSION_0_15_0 && !is_blank_slate) {
    if (err_msg) {
      *err_msg = sqlite3_mprintf(
        "Opening a db created with cr-sqlite version %d is not supported. "
        "Version 0.15.0 is a breaking change.",
        recorded_version
      );
    }
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return SQLITE_ERROR;
  }

  // TODO: Add migration functions here if we need to migrate from older versions
  // if (recorded_version < CRSQLITE_VERSION_0_15_0) {
  //   update_to_0_15_0(db);
  // }

  // Write version if upgraded or new database
  if (recorded_version < CRSQLITE_VERSION || is_blank_slate) {
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db,
      "INSERT OR REPLACE INTO crsql_master VALUES ('crsqlite_version', ?)",
      -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
      sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
      return rc;
    }

    rc = sqlite3_bind_int(stmt, 1, CRSQLITE_VERSION);
    if (rc != SQLITE_OK) {
      sqlite3_finalize(stmt);
      sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
      return rc;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
      sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
      return rc;
    }
  }

  return sqlite3_exec(db, "RELEASE crsql_maybe_update_db;", NULL, NULL, NULL);
}

// Create clock tables for a CRR table
int crsql_create_clock_table(sqlite3 *db, crsql_TableInfo *table_info, char **err) {
  char *escaped_tbl = crsql_escape_ident(table_info->tbl_name);
  char *pk_list = crsql_as_identifier_list(table_info->pks, table_info->pks_len, NULL);

  if (!escaped_tbl || !pk_list) {
    sqlite3_free(escaped_tbl);
    sqlite3_free(pk_list);
    if (err) *err = sqlite3_mprintf("Out of memory creating clock table");
    return SQLITE_NOMEM;
  }

  // Create clock table
  char *sql = sqlite3_mprintf(
    "CREATE TABLE IF NOT EXISTS \"%s__crsql_clock\" (\n"
    "  key INTEGER NOT NULL,\n"
    "  col_name TEXT NOT NULL,\n"
    "  col_version INTEGER NOT NULL,\n"
    "  db_version INTEGER NOT NULL,\n"
    "  site_id INTEGER NOT NULL DEFAULT 0,\n"
    "  seq INTEGER NOT NULL,\n"
    "  PRIMARY KEY (key, col_name)\n"
    ") WITHOUT ROWID, STRICT",
    escaped_tbl
  );

  if (!sql) {
    sqlite3_free(escaped_tbl);
    sqlite3_free(pk_list);
    if (err) *err = sqlite3_mprintf("Out of memory");
    return SQLITE_NOMEM;
  }

  int rc = sqlite3_exec(db, sql, NULL, NULL, err);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    sqlite3_free(escaped_tbl);
    sqlite3_free(pk_list);
    return rc;
  }

  // Create db_version index on clock table
  sql = sqlite3_mprintf(
    "CREATE INDEX IF NOT EXISTS \"%s__crsql_clock_dbv_idx\" "
    "ON \"%s__crsql_clock\" (\"db_version\")",
    escaped_tbl, escaped_tbl
  );

  if (!sql) {
    sqlite3_free(escaped_tbl);
    sqlite3_free(pk_list);
    if (err) *err = sqlite3_mprintf("Out of memory");
    return SQLITE_NOMEM;
  }

  rc = sqlite3_exec(db, sql, NULL, NULL, err);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    sqlite3_free(escaped_tbl);
    sqlite3_free(pk_list);
    return rc;
  }

  // Create primary key table
  sql = sqlite3_mprintf(
    "CREATE TABLE IF NOT EXISTS \"%s__crsql_pks\" (__crsql_key INTEGER PRIMARY KEY, %s)",
    table_info->tbl_name, pk_list
  );

  if (!sql) {
    sqlite3_free(escaped_tbl);
    sqlite3_free(pk_list);
    if (err) *err = sqlite3_mprintf("Out of memory");
    return SQLITE_NOMEM;
  }

  rc = sqlite3_exec(db, sql, NULL, NULL, err);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    sqlite3_free(escaped_tbl);
    sqlite3_free(pk_list);
    return rc;
  }

  // Create unique index on PKs
  sql = sqlite3_mprintf(
    "CREATE UNIQUE INDEX IF NOT EXISTS \"%s__crsql_pks_pks\" "
    "ON \"%s__crsql_pks\" (%s)",
    table_info->tbl_name, table_info->tbl_name, pk_list
  );

  sqlite3_free(escaped_tbl);
  sqlite3_free(pk_list);

  if (!sql) {
    if (err) *err = sqlite3_mprintf("Out of memory");
    return SQLITE_NOMEM;
  }

  rc = sqlite3_exec(db, sql, NULL, NULL, err);
  sqlite3_free(sql);

  return rc;
}
