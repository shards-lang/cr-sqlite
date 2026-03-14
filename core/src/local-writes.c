#include "local-writes.h"

#include <string.h>

#include "consts.h"
#include "tableinfo.h"
#include "util.h"

// From rust.h / db_version
extern sqlite_int64 crsql_next_db_version(sqlite3 *db,
                                          crsql_ExtData *pExtData,
                                          sqlite3_int64 mergingVersion,
                                          char **errmsg);

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static int reset_cached_stmt(sqlite3_stmt *pStmt) {
  if (pStmt == 0) return SQLITE_OK;
  sqlite3_clear_bindings(pStmt);
  return sqlite3_reset(pStmt);
}

/**
 * Step a cached trigger statement. Expects SQLITE_DONE.
 * Resets the statement after stepping.
 * Returns 0 on success, sets *errOut on failure.
 */
static int step_trigger_stmt(sqlite3_stmt *pStmt, const char **errOut) {
  int rc = sqlite3_step(pStmt);
  if (rc == SQLITE_DONE) {
    reset_cached_stmt(pStmt);
    return 0;
  }
  reset_cached_stmt(pStmt);
  if (errOut) *errOut = "unexpected result code from trigger_stmt step";
  return -1;
}

static int bump_seq(crsql_ExtData *extData) {
  extData->seq += 1;
  return extData->seq - 1;
}

/**
 * Common preamble for trigger functions.
 * Validates args, ensures table infos are up to date, finds the table info.
 * On success: *ppTblInfo is set, *ppValues points to argv as sqlite3_value**,
 * *ppExtData is set. Returns 0 on success, -1 on error (with errOut set).
 */
static int trigger_fn_preamble(sqlite3_context *ctx, int argc,
                               sqlite3_value **argv,
                               crsql_TableInfo **ppTblInfo,
                               crsql_ExtData **ppExtData,
                               const char **errOut) {
  if (argc < 1) {
    *errOut = "expected at least 1 argument";
    return -1;
  }

  crsql_ExtData *extData = (crsql_ExtData *)sqlite3_user_data(ctx);
  *ppExtData = extData;

  char *innerErr = 0;
  int rc = crsql_ensure_table_infos_are_up_to_date(
      sqlite3_context_db_handle(ctx), extData, &innerErr);
  if (rc != SQLITE_OK) {
    sqlite3_free(innerErr);
    *errOut = "failed to ensure table infos are up to date";
    return -1;
  }

  const char *tableName = (const char *)sqlite3_value_text(argv[0]);
  if (!tableName) {
    *errOut = "table name is null";
    return -1;
  }

  crsql_TableInfoVec *tblInfoVec = (crsql_TableInfoVec *)extData->tableInfos;
  int idx = crsql_find_table_info(tblInfoVec, tableName);
  if (idx < 0) {
    *errOut = "table not found in table infos";
    return -1;
  }

  *ppTblInfo = &tblInfoVec->aInfos[idx];
  return 0;
}

/**
 * Record that a new PK-only row was created (sentinel insert).
 * INSERT OR REPLACE into clock table with sentinel col_name.
 */
static int mark_new_pk_row_created(sqlite3 *db, crsql_TableInfo *tblInfo,
                                   sqlite3_int64 keyNew, sqlite3_int64 dbVersion,
                                   int seq, const char **errOut) {
  sqlite3_stmt *pStmt = 0;
  int rc = crsql_get_mark_locally_created_stmt(db, tblInfo, &pStmt);
  if (rc != SQLITE_OK || !pStmt) {
    *errOut = "failed to get mark_locally_created_stmt";
    return -1;
  }

  rc = sqlite3_bind_int64(pStmt, 1, keyNew);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(pStmt, 2, dbVersion);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(pStmt, 3, seq);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(pStmt, 4, dbVersion);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(pStmt, 5, seq);
  if (rc != SQLITE_OK) {
    reset_cached_stmt(pStmt);
    *errOut = "failed binding to mark_locally_created_stmt";
    return -1;
  }
  return step_trigger_stmt(pStmt, errOut);
}

/**
 * Record a local update for a single column.
 */
static int mark_locally_updated(sqlite3 *db, crsql_TableInfo *tblInfo,
                                sqlite3_int64 newKey,
                                crsql_ColumnInfo *colInfo,
                                sqlite3_int64 dbVersion, int seq,
                                const char **errOut) {
  sqlite3_stmt *pStmt = 0;
  int rc = crsql_get_mark_locally_updated_stmt(db, tblInfo, &pStmt);
  if (rc != SQLITE_OK || !pStmt) {
    *errOut = "failed to get mark_locally_updated_stmt";
    return -1;
  }

  rc = sqlite3_bind_int64(pStmt, 1, newKey);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_text(pStmt, 2, colInfo->name, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(pStmt, 3, dbVersion);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(pStmt, 4, seq);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(pStmt, 5, dbVersion);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(pStmt, 6, seq);
  if (rc != SQLITE_OK) {
    reset_cached_stmt(pStmt);
    *errOut = "failed binding to mark_locally_updated_stmt";
    return -1;
  }
  return step_trigger_stmt(pStmt, errOut);
}

/* ------------------------------------------------------------------ */
/*  after_insert helpers                                                */
/* ------------------------------------------------------------------ */

static int update_create_record(sqlite3 *db, crsql_TableInfo *tblInfo,
                                sqlite3_int64 newKey, sqlite3_int64 dbVersion,
                                int seq, const char **errOut) {
  sqlite3_stmt *pStmt = 0;
  int rc =
      crsql_get_maybe_mark_locally_reinserted_stmt(db, tblInfo, &pStmt);
  if (rc != SQLITE_OK || !pStmt) {
    *errOut = "failed to get update_create_record_stmt";
    return -1;
  }

  rc = sqlite3_bind_int64(pStmt, 1, dbVersion);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(pStmt, 2, seq);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(pStmt, 3, newKey);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_text(pStmt, 4, INSERT_SENTINEL, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    reset_cached_stmt(pStmt);
    *errOut = "failed binding to update_create_record_stmt";
    return -1;
  }
  return step_trigger_stmt(pStmt, errOut);
}

static int after_insert(sqlite3 *db, crsql_ExtData *extData,
                        crsql_TableInfo *tblInfo, sqlite3_value **pksNew,
                        int numPks, const char **errOut) {
  char *verErr = 0;
  sqlite3_int64 dbVersion = crsql_next_db_version(db, extData, 0, &verErr);
  if (dbVersion < 0) {
    sqlite3_free(verErr);
    *errOut = "failed to get next db version";
    return -1;
  }

  // Check if key already exists (create_record_existed)
  sqlite3_int64 existingKey = crsql_get_key(db, tblInfo, pksNew, numPks);
  int createRecordExisted = (existingKey >= 0) ? 1 : 0;

  char *keyErr = 0;
  sqlite3_int64 keyNew =
      crsql_get_or_create_key_for_insert(db, tblInfo, pksNew, numPks, &keyErr);
  if (keyNew < 0) {
    sqlite3_free(keyErr);
    *errOut = "failed getting or creating lookaside key";
    return -1;
  }

  if (tblInfo->nonPksLen == 0) {
    int seq = bump_seq(extData);
    return mark_new_pk_row_created(db, tblInfo, keyNew, dbVersion, seq, errOut);
  } else if (createRecordExisted) {
    int seq = bump_seq(extData);
    int ret = update_create_record(db, tblInfo, keyNew, dbVersion, seq, errOut);
    if (ret != 0) return ret;
  }

  // For each non-pk column, mark as locally updated
  for (int i = 0; i < tblInfo->nonPksLen; i++) {
    int seq = bump_seq(extData);
    int ret = mark_locally_updated(db, tblInfo, keyNew, &tblInfo->nonPks[i],
                                   dbVersion, seq, errOut);
    if (ret != 0) return ret;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/*  after_update helpers                                                */
/* ------------------------------------------------------------------ */

static int after_update_mark_old_pk_row_deleted(sqlite3 *db,
                                                crsql_TableInfo *tblInfo,
                                                sqlite3_int64 oldKey,
                                                sqlite3_int64 dbVersion,
                                                int seq,
                                                const char **errOut) {
  sqlite3_stmt *pStmt = 0;
  int rc = crsql_get_mark_locally_deleted_stmt(db, tblInfo, &pStmt);
  if (rc != SQLITE_OK || !pStmt) {
    *errOut = "failed to get mark_locally_deleted_stmt";
    return -1;
  }

  rc = sqlite3_bind_int64(pStmt, 1, oldKey);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(pStmt, 2, dbVersion);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(pStmt, 3, seq);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(pStmt, 4, dbVersion);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(pStmt, 5, seq);
  if (rc != SQLITE_OK) {
    reset_cached_stmt(pStmt);
    *errOut = "failed binding to mark_locally_deleted_stmt";
    return -1;
  }
  return step_trigger_stmt(pStmt, errOut);
}

static int after_update_move_non_sentinels(sqlite3 *db,
                                           crsql_TableInfo *tblInfo,
                                           sqlite3_int64 newKey,
                                           sqlite3_int64 oldKey,
                                           const char **errOut) {
  sqlite3_stmt *pStmt = 0;
  int rc = crsql_get_move_non_sentinels_stmt(db, tblInfo, &pStmt);
  if (rc != SQLITE_OK || !pStmt) {
    *errOut = "failed to get move_non_sentinels_stmt";
    return -1;
  }

  rc = sqlite3_bind_int64(pStmt, 1, newKey);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(pStmt, 2, oldKey);
  if (rc != SQLITE_OK) {
    reset_cached_stmt(pStmt);
    *errOut = "failed to bind pks to move_non_sentinels_stmt";
    return -1;
  }
  return step_trigger_stmt(pStmt, errOut);
}

static int after_update(sqlite3 *db, crsql_ExtData *extData,
                        crsql_TableInfo *tblInfo, sqlite3_value **pksNew,
                        sqlite3_value **pksOld, sqlite3_value **nonPksNew,
                        sqlite3_value **nonPksOld, int numPks, int numNonPks,
                        const char **errOut) {
  char *verErr = 0;
  sqlite3_int64 nextDbVersion = crsql_next_db_version(db, extData, 0, &verErr);
  if (nextDbVersion < 0) {
    sqlite3_free(verErr);
    *errOut = "failed to get next db version";
    return -1;
  }

  char *keyErr = 0;
  sqlite3_int64 newKey =
      crsql_get_or_create_key(db, tblInfo, pksNew, numPks, &keyErr);
  if (newKey < 0) {
    sqlite3_free(keyErr);
    *errOut = "failed getting or creating lookaside key";
    return -1;
  }

  // Check if PKs changed
  int pksChanged = crsql_any_value_changed(pksNew, pksOld, numPks);
  if (pksChanged < 0) {
    *errOut = "error comparing pk values";
    return -1;
  }

  if (pksChanged) {
    char *oldKeyErr = 0;
    sqlite3_int64 oldKey =
        crsql_get_or_create_key(db, tblInfo, pksOld, numPks, &oldKeyErr);
    if (oldKey < 0) {
      sqlite3_free(oldKeyErr);
      *errOut = "failed getting or creating lookaside key for old pks";
      return -1;
    }

    int nextSeq = bump_seq(extData);
    int ret = after_update_mark_old_pk_row_deleted(db, tblInfo, oldKey,
                                                   nextDbVersion, nextSeq,
                                                   errOut);
    if (ret != 0) return ret;

    ret = after_update_move_non_sentinels(db, tblInfo, newKey, oldKey, errOut);
    if (ret != 0) return ret;

    nextSeq = bump_seq(extData);
    ret = mark_new_pk_row_created(db, tblInfo, newKey, nextDbVersion, nextSeq,
                                  errOut);
    if (ret != 0) return ret;
  }

  // For each non-pk column where value changed, mark locally updated
  for (int i = 0; i < numNonPks; i++) {
    if (crsql_compare_sqlite_values(nonPksNew[i], nonPksOld[i]) != 0) {
      int nextSeq = bump_seq(extData);
      int ret = mark_locally_updated(db, tblInfo, newKey, &tblInfo->nonPks[i],
                                     nextDbVersion, nextSeq, errOut);
      if (ret != 0) return ret;
    }
  }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  after_delete                                                       */
/* ------------------------------------------------------------------ */

static int after_delete(sqlite3 *db, crsql_ExtData *extData,
                        crsql_TableInfo *tblInfo, sqlite3_value **pksOld,
                        int numPks, const char **errOut) {
  char *verErr = 0;
  sqlite3_int64 dbVersion = crsql_next_db_version(db, extData, 0, &verErr);
  if (dbVersion < 0) {
    sqlite3_free(verErr);
    *errOut = "failed to get next db version";
    return -1;
  }

  int seq = bump_seq(extData);
  char *keyErr = 0;
  sqlite3_int64 key =
      crsql_get_or_create_key(db, tblInfo, pksOld, numPks, &keyErr);
  if (key < 0) {
    sqlite3_free(keyErr);
    *errOut = "failed getting or creating lookaside key";
    return -1;
  }

  // Mark the row as locally deleted
  sqlite3_stmt *pDeleteStmt = 0;
  int rc = crsql_get_mark_locally_deleted_stmt(db, tblInfo, &pDeleteStmt);
  if (rc != SQLITE_OK || !pDeleteStmt) {
    *errOut = "failed to get mark_locally_deleted_stmt";
    return -1;
  }

  rc = sqlite3_bind_int64(pDeleteStmt, 1, key);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(pDeleteStmt, 2, dbVersion);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(pDeleteStmt, 3, seq);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(pDeleteStmt, 4, dbVersion);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(pDeleteStmt, 5, seq);
  if (rc != SQLITE_OK) {
    reset_cached_stmt(pDeleteStmt);
    *errOut = "failed binding to mark locally deleted stmt";
    return -1;
  }
  if (step_trigger_stmt(pDeleteStmt, errOut) != 0) return -1;

  // Drop non-sentinel clock entries
  sqlite3_stmt *pDropStmt = 0;
  rc = crsql_get_merge_delete_drop_clocks_stmt(db, tblInfo, &pDropStmt);
  if (rc != SQLITE_OK || !pDropStmt) {
    *errOut = "failed to get drop_clocks_stmt";
    return -1;
  }

  rc = sqlite3_bind_int64(pDropStmt, 1, key);
  if (rc != SQLITE_OK) {
    reset_cached_stmt(pDropStmt);
    *errOut = "failed to bind pks to drop_clocks_stmt";
    return -1;
  }
  return step_trigger_stmt(pDropStmt, errOut);
}

/* ------------------------------------------------------------------ */
/*  Public trigger function callbacks                                  */
/* ------------------------------------------------------------------ */

void x_crsql_after_insert(sqlite3_context *ctx, int argc,
                          sqlite3_value **argv) {
  crsql_TableInfo *tblInfo = 0;
  crsql_ExtData *extData = 0;
  const char *errMsg = 0;

  if (trigger_fn_preamble(ctx, argc, argv, &tblInfo, &extData, &errMsg) != 0) {
    sqlite3_result_error(ctx, errMsg, -1);
    return;
  }

  // argv[0] = table name, argv[1..] = pk values
  int numPks = tblInfo->pksLen;
  int ret = after_insert(sqlite3_context_db_handle(ctx), extData, tblInfo,
                         &argv[1], numPks, &errMsg);
  if (ret != 0) {
    sqlite3_result_error(ctx, errMsg, -1);
  } else {
    sqlite3_result_int64(ctx, 0);
  }
}

void x_crsql_after_update(sqlite3_context *ctx, int argc,
                           sqlite3_value **argv) {
  crsql_TableInfo *tblInfo = 0;
  crsql_ExtData *extData = 0;
  const char *errMsg = 0;

  if (trigger_fn_preamble(ctx, argc, argv, &tblInfo, &extData, &errMsg) != 0) {
    sqlite3_result_error(ctx, errMsg, -1);
    return;
  }

  int numPks = tblInfo->pksLen;
  int numNonPks = tblInfo->nonPksLen;
  int offset = 1;

  // Validate argument count:
  // 1 (tbl name) + numPks*2 + numNonPks*2
  int expectedArgc = offset + numPks * 2 + numNonPks * 2;
  if (argc != expectedArgc) {
    sqlite3_result_error(ctx, "wrong number of arguments to after_update", -1);
    return;
  }

  sqlite3_value **pksNew = &argv[offset];
  sqlite3_value **pksOld = &argv[offset + numPks];
  sqlite3_value **nonPksNew = &argv[offset + numPks * 2];
  sqlite3_value **nonPksOld = &argv[offset + numPks * 2 + numNonPks];

  int ret =
      after_update(sqlite3_context_db_handle(ctx), extData, tblInfo, pksNew,
                   pksOld, nonPksNew, nonPksOld, numPks, numNonPks, &errMsg);
  if (ret != 0) {
    sqlite3_result_error(ctx, errMsg, -1);
  } else {
    sqlite3_result_int64(ctx, 0);
  }
}

void x_crsql_after_delete(sqlite3_context *ctx, int argc,
                           sqlite3_value **argv) {
  crsql_TableInfo *tblInfo = 0;
  crsql_ExtData *extData = 0;
  const char *errMsg = 0;

  if (trigger_fn_preamble(ctx, argc, argv, &tblInfo, &extData, &errMsg) != 0) {
    sqlite3_result_error(ctx, errMsg, -1);
    return;
  }

  // argv[0] = table name, argv[1..] = old pk values
  int numPks = tblInfo->pksLen;
  int ret = after_delete(sqlite3_context_db_handle(ctx), extData, tblInfo,
                         &argv[1], numPks, &errMsg);
  if (ret != 0) {
    sqlite3_result_error(ctx, errMsg, -1);
  } else {
    sqlite3_result_int64(ctx, 0);
  }
}
