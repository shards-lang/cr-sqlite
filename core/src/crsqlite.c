#include "crsqlite.h"
SQLITE_EXTENSION_INIT1
#ifdef LIBSQL
LIBSQL_EXTENSION_INIT1
#endif

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "automigrate.h"
#include "bootstrap.h"
#include "changes-vtab.h"
#include "cl-set-vtab.h"
#include "consts.h"
#include "crr.h"
#include "db-version.h"
#include "ext-data.h"
#include "fractindex.h"
#include "local-writes.h"
#include "pack-columns.h"
#include "tableinfo.h"
#include "unpack-columns-vtab.h"

static void freeConnectionExtData(void *pUserData) {
  crsql_ExtData *pExtData = (crsql_ExtData *)pUserData;
  crsql_freeExtData(pExtData);
}

static int commitHook(void *pUserData) {
  crsql_ExtData *pExtData = (crsql_ExtData *)pUserData;
  pExtData->dbVersion = pExtData->pendingDbVersion;
  pExtData->pendingDbVersion = -1;
  pExtData->seq = 0;
  pExtData->updatedTableInfosThisTx = 0;
  return SQLITE_OK;
}

static void rollbackHook(void *pUserData) {
  crsql_ExtData *pExtData = (crsql_ExtData *)pUserData;
  pExtData->pendingDbVersion = -1;
  pExtData->seq = 0;
  pExtData->updatedTableInfosThisTx = 0;
}

#ifdef LIBSQL
static void closeHook(void *pUserData, sqlite3 *db) {
  crsql_ExtData *pExtData = (crsql_ExtData *)pUserData;
  crsql_finalize(pExtData);
}
#endif

// --- SQL function callbacks ---

static void x_crsql_site_id(sqlite3_context *ctx, int argc,
                             sqlite3_value **argv) {
  crsql_ExtData *pExtData =
      (crsql_ExtData *)sqlite3_user_data(ctx);
  sqlite3_result_blob(ctx, pExtData->siteId, SITE_ID_LEN, SQLITE_STATIC);
}

static void x_crsql_db_version(sqlite3_context *ctx, int argc,
                                sqlite3_value **argv) {
  crsql_ExtData *pExtData =
      (crsql_ExtData *)sqlite3_user_data(ctx);
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  char *errmsg = 0;
  int rc = crsql_fill_db_version_if_needed(db, pExtData, &errmsg);
  if (rc != SQLITE_OK) {
    sqlite3_result_error(ctx, errmsg ? errmsg : "failed to fill db version",
                         -1);
    sqlite3_free(errmsg);
    return;
  }
  sqlite3_result_int64(ctx, pExtData->dbVersion);
}

static void x_crsql_next_db_version(sqlite3_context *ctx, int argc,
                                     sqlite3_value **argv) {
  crsql_ExtData *pExtData =
      (crsql_ExtData *)sqlite3_user_data(ctx);
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  char *errmsg = 0;

  sqlite3_int64 providedVersion = 0;
  if (argc == 1) {
    providedVersion = sqlite3_value_int64(argv[0]);
  }

  sqlite3_int64 ret =
      crsql_next_db_version(db, pExtData, providedVersion, &errmsg);
  if (ret < 0) {
    sqlite3_result_error(
        ctx, errmsg ? errmsg : "Unable to determine the next db version", -1);
    sqlite3_free(errmsg);
    return;
  }
  sqlite3_result_int64(ctx, ret);
}

static void x_crsql_increment_and_get_seq(sqlite3_context *ctx, int argc,
                                           sqlite3_value **argv) {
  crsql_ExtData *pExtData =
      (crsql_ExtData *)sqlite3_user_data(ctx);
  sqlite3_result_int(ctx, pExtData->seq);
  pExtData->seq += 1;
}

static void x_crsql_get_seq(sqlite3_context *ctx, int argc,
                             sqlite3_value **argv) {
  crsql_ExtData *pExtData =
      (crsql_ExtData *)sqlite3_user_data(ctx);
  sqlite3_result_int(ctx, pExtData->seq);
}

static void x_crsql_rows_impacted(sqlite3_context *ctx, int argc,
                                   sqlite3_value **argv) {
  crsql_ExtData *pExtData =
      (crsql_ExtData *)sqlite3_user_data(ctx);
  sqlite3_result_int(ctx, pExtData->rowsImpacted);
}

static void x_crsql_finalize_fn(sqlite3_context *ctx, int argc,
                                 sqlite3_value **argv) {
  crsql_ExtData *pExtData =
      (crsql_ExtData *)sqlite3_user_data(ctx);
  crsql_finalize(pExtData);
  sqlite3_result_text(ctx, "finalized", -1, SQLITE_STATIC);
}

static void x_crsql_as_crr(sqlite3_context *ctx, int argc,
                            sqlite3_value **argv) {
  if (argc == 0) {
    sqlite3_result_error(
        ctx,
        "Wrong number of args provided to crsql_as_crr. "
        "Provide the schema name and table name or just the table name.",
        -1);
    return;
  }

  const char *schemaName;
  const char *tableName;
  if (argc == 2) {
    schemaName = (const char *)sqlite3_value_text(argv[0]);
    tableName = (const char *)sqlite3_value_text(argv[1]);
  } else {
    schemaName = "main";
    tableName = (const char *)sqlite3_value_text(argv[0]);
  }

  sqlite3 *db = sqlite3_context_db_handle(ctx);
  char *errmsg = 0;

  int rc = sqlite3_exec(db, "SAVEPOINT as_crr", 0, 0, 0);
  if (rc != SQLITE_OK) {
    sqlite3_result_error(ctx, "failed to start as_crr savepoint", -1);
    return;
  }

  rc = crsql_create_crr(db, schemaName, tableName, 0, 0, &errmsg);
  if (rc != SQLITE_OK) {
    if (errmsg) {
      sqlite3_result_error(ctx, errmsg, -1);
      sqlite3_free(errmsg);
    } else {
      sqlite3_result_error(ctx, "failed to create crr", -1);
    }
    sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    return;
  }

  sqlite3_exec(db, "RELEASE as_crr", 0, 0, 0);
  sqlite3_result_text(ctx, "OK", -1, SQLITE_STATIC);
}

// Forward declarations for teardown functions
extern int crsql_remove_crr_clock_table_if_exists(sqlite3 *db,
                                                   const char *table);
extern int crsql_remove_crr_triggers_if_exist(sqlite3 *db,
                                               const char *table);

static void x_crsql_as_table(sqlite3_context *ctx, int argc,
                              sqlite3_value **argv) {
  if (argc < 1) {
    sqlite3_result_error(ctx, "Expected table name argument", -1);
    return;
  }
  const char *table = (const char *)sqlite3_value_text(argv[0]);
  sqlite3 *db = sqlite3_context_db_handle(ctx);

  if (sqlite3_exec(db, "SAVEPOINT as_table", 0, 0, 0) != SQLITE_OK) {
    sqlite3_result_error(ctx, "failed to start as_table savepoint", -1);
    return;
  }

  if (crsql_remove_crr_clock_table_if_exists(db, table) != SQLITE_OK ||
      crsql_remove_crr_triggers_if_exist(db, table) != SQLITE_OK) {
    sqlite3_result_error(ctx, "failed to downgrade the crr", -1);
    sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    return;
  }

  sqlite3_exec(db, "RELEASE as_table", 0, 0, 0);
}

static void x_crsql_begin_alter(sqlite3_context *ctx, int argc,
                                 sqlite3_value **argv) {
  if (argc == 0) {
    sqlite3_result_error(
        ctx,
        "Wrong number of args provided to crsql_begin_alter. "
        "Provide the schema name and table name or just the table name.",
        -1);
    return;
  }

  const char *tableName;
  if (argc == 2) {
    tableName = (const char *)sqlite3_value_text(argv[1]);
  } else {
    tableName = (const char *)sqlite3_value_text(argv[0]);
  }

  sqlite3 *db = sqlite3_context_db_handle(ctx);
  if (sqlite3_exec(db, "SAVEPOINT alter_crr", 0, 0, 0) != SQLITE_OK) {
    sqlite3_result_error(ctx, "failed to start alter_crr savepoint", -1);
    return;
  }

  if (crsql_remove_crr_triggers_if_exist(db, tableName) != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    sqlite3_result_error(ctx, "failed to remove triggers for alter", -1);
    return;
  }
  sqlite3_result_text(ctx, "OK", -1, SQLITE_STATIC);
}

static void x_crsql_commit_alter(sqlite3_context *ctx, int argc,
                                  sqlite3_value **argv) {
  if (argc == 0) {
    sqlite3_result_error(
        ctx,
        "Wrong number of args provided to crsql_commit_alter. "
        "Provide the schema name and table name or just the table name.",
        -1);
    return;
  }

  const char *schemaName;
  const char *tableName;
  if (argc == 2) {
    schemaName = (const char *)sqlite3_value_text(argv[0]);
    tableName = (const char *)sqlite3_value_text(argv[1]);
  } else {
    schemaName = "main";
    tableName = (const char *)sqlite3_value_text(argv[0]);
  }

  crsql_ExtData *pExtData =
      (crsql_ExtData *)sqlite3_user_data(ctx);
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  char *errmsg = 0;

  int rc = crsql_compact_post_alter(db, tableName, pExtData, &errmsg);
  if (rc == SQLITE_OK) {
    rc = crsql_create_crr(db, schemaName, tableName, 1, 0, &errmsg);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_exec(db, "RELEASE alter_crr", 0, 0, 0);
  }
  if (rc != SQLITE_OK) {
    sqlite3_result_error(
        ctx,
        errmsg ? errmsg : "failed compacting tables post alteration", -1);
    sqlite3_free(errmsg);
    sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    return;
  }
}

static void x_crsql_sync_bit(sqlite3_context *ctx, int argc,
                              sqlite3_value **argv) {
  int *syncBitPtr = (int *)sqlite3_user_data(ctx);
  if (argc != 1) {
    sqlite3_result_int(ctx, *syncBitPtr);
    return;
  }
  *syncBitPtr = sqlite3_value_int(argv[0]);
  sqlite3_result_int(ctx, *syncBitPtr);
}

static void x_crsql_sha(sqlite3_context *ctx, int argc,
                         sqlite3_value **argv) {
#ifdef CRSQLITE_COMMIT_SHA
  sqlite3_result_text(ctx, CRSQLITE_COMMIT_SHA, -1, SQLITE_STATIC);
#else
  sqlite3_result_text(ctx, "unknown", -1, SQLITE_STATIC);
#endif
}

// --- Main extension init ---

#ifdef _WIN32
__declspec(dllexport)
#endif
    int sqlite3_crsqlite_init(sqlite3 *db, char **pzErrMsg,
                              const sqlite3_api_routines *pApi
#ifdef LIBSQL
                              ,
                              const libsql_api_routines *pLibsqlApi
#endif
    ) {
  int rc = SQLITE_OK;

  SQLITE_EXTENSION_INIT2(pApi);
#ifdef LIBSQL
  LIBSQL_EXTENSION_INIT2(pLibsqlApi);
#endif

  // --- Register functions that don't need ExtData ---

  rc = sqlite3_create_function_v2(db, "crsql_automigrate", -1, SQLITE_UTF8, 0,
                                  crsql_automigrate, 0, 0, 0);
  if (rc != SQLITE_OK) return rc;

  rc = sqlite3_create_function_v2(db, "crsql_pack_columns", -1, SQLITE_UTF8, 0,
                                  crsql_pack_columns_fn, 0, 0, 0);
  if (rc != SQLITE_OK) return rc;

  rc = sqlite3_create_function_v2(db, "crsql_as_table", 1, SQLITE_UTF8, 0,
                                  x_crsql_as_table, 0, 0, 0);
  if (rc != SQLITE_OK) return rc;

  // --- Register virtual table modules ---

  rc = crsql_create_unpack_columns_module(db);
  if (rc != SQLITE_OK) return rc;

  rc = crsql_create_cl_set_module(db);
  if (rc != SQLITE_OK) return rc;

  // --- Bootstrap ---

  rc = crsql_init_peer_tracking_table(db);
  if (rc != SQLITE_OK) return rc;

  // Sync bit (shared mutable state for trigger guard)
  int *syncBitPtr = sqlite3_malloc(sizeof(int));
  if (!syncBitPtr) return SQLITE_NOMEM;
  *syncBitPtr = 0;

  rc = sqlite3_create_function_v2(
      db, "crsql_internal_sync_bit", -1,
      SQLITE_UTF8 | SQLITE_INNOCUOUS, syncBitPtr, x_crsql_sync_bit, 0, 0,
      sqlite3_free);
  if (rc != SQLITE_OK) {
    sqlite3_free(syncBitPtr);
    return rc;
  }

  rc = crsql_maybe_update_db(db, pzErrMsg);
  if (rc != SQLITE_OK) return rc;

  // --- Site ID ---

  unsigned char *siteIdBuffer = sqlite3_malloc(SITE_ID_LEN);
  if (!siteIdBuffer) return SQLITE_NOMEM;

  rc = crsql_init_site_id(db, siteIdBuffer);
  if (rc != SQLITE_OK) {
    sqlite3_free(siteIdBuffer);
    return rc;
  }

  // --- ExtData ---

  crsql_ExtData *pExtData = crsql_newExtData(db, siteIdBuffer);
  if (!pExtData) return SQLITE_ERROR;

  // --- Register functions that need ExtData ---

  rc = sqlite3_create_function_v2(
      db, "crsql_site_id", 0,
      SQLITE_UTF8 | SQLITE_INNOCUOUS | SQLITE_DETERMINISTIC, pExtData,
      x_crsql_site_id, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  // crsql_db_version owns the destructor for ExtData
  rc = sqlite3_create_function_v2(
      db, "crsql_db_version", 0,
      SQLITE_UTF8 | SQLITE_INNOCUOUS, pExtData,
      x_crsql_db_version, 0, 0, freeConnectionExtData);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(
      db, "crsql_next_db_version", -1,
      SQLITE_UTF8 | SQLITE_INNOCUOUS, pExtData,
      x_crsql_next_db_version, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(
      db, "crsql_sha", 0,
      SQLITE_UTF8 | SQLITE_INNOCUOUS | SQLITE_DETERMINISTIC, 0,
      x_crsql_sha, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(
      db, "crsql_increment_and_get_seq", 0,
      SQLITE_UTF8 | SQLITE_INNOCUOUS, pExtData,
      x_crsql_increment_and_get_seq, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(
      db, "crsql_get_seq", 0,
      SQLITE_UTF8 | SQLITE_INNOCUOUS | SQLITE_DETERMINISTIC, pExtData,
      x_crsql_get_seq, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(
      db, "crsql_as_crr", -1,
      SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
      x_crsql_as_crr, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(
      db, "crsql_begin_alter", -1,
      SQLITE_UTF8 | SQLITE_DIRECTONLY, 0,
      x_crsql_begin_alter, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(
      db, "crsql_commit_alter", -1,
      SQLITE_UTF8 | SQLITE_DIRECTONLY, pExtData,
      x_crsql_commit_alter, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(
      db, "crsql_finalize", -1,
      SQLITE_UTF8 | SQLITE_DIRECTONLY, pExtData,
      x_crsql_finalize_fn, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(
      db, "crsql_after_update", -1,
      SQLITE_UTF8 | SQLITE_INNOCUOUS, pExtData,
      x_crsql_after_update, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(
      db, "crsql_after_insert", -1,
      SQLITE_UTF8 | SQLITE_INNOCUOUS, pExtData,
      x_crsql_after_insert, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(
      db, "crsql_after_delete", -1,
      SQLITE_UTF8 | SQLITE_INNOCUOUS, pExtData,
      x_crsql_after_delete, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(
      db, "crsql_rows_impacted", 0,
      SQLITE_UTF8 | SQLITE_INNOCUOUS, pExtData,
      x_crsql_rows_impacted, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(db, "crsql_config_set", 2, SQLITE_UTF8,
                                  pExtData, crsql_config_set, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  rc = sqlite3_create_function_v2(
      db, "crsql_config_get", 1,
      SQLITE_UTF8 | SQLITE_INNOCUOUS | SQLITE_DETERMINISTIC, pExtData,
      crsql_config_get, 0, 0, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  // --- Fractional indexing ---
  rc = crsql_init_fractional_index(db);
  if (rc != SQLITE_OK) goto err_free_ext;

  // --- Changes virtual table ---

  rc = sqlite3_create_module_v2(db, "crsql_changes", &crsql_changesModule,
                                pExtData, 0);
  if (rc != SQLITE_OK) goto err_free_ext;

  // --- Hooks ---

#ifdef LIBSQL
  libsql_close_hook(db, closeHook, pExtData);
#endif
  sqlite3_commit_hook(db, commitHook, pExtData);
  sqlite3_rollback_hook(db, rollbackHook, pExtData);

  return SQLITE_OK;

err_free_ext:
  crsql_freeExtData(pExtData);
  return rc;
}
