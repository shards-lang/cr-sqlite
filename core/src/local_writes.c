#include "local_writes.h"
#include "tableinfo.h"
#include "ext-data.h"
#include "db_version.h"
#include "stmt_cache.h"
#include "compare_values.h"
#include "consts.h"
#include <string.h>

// Forward declarations
static int trigger_fn_preamble(sqlite3_context *ctx, int argc, sqlite3_value **argv,
                               crsql_TableInfo **table_info_out,
                               crsql_ExtData **ext_data_out,
                               sqlite3_value ***values_out);
static int step_trigger_stmt(sqlite3_stmt *stmt);
static int mark_new_pk_row_created(sqlite3 *db, crsql_TableInfo *tbl_info,
                                   sqlite3_int64 key_new, sqlite3_int64 db_version,
                                   int seq);
static int mark_locally_updated(sqlite3 *db, crsql_TableInfo *tbl_info,
                                sqlite3_int64 new_key, crsql_ColumnInfo *col_info,
                                sqlite3_int64 db_version, int seq);
static int bump_seq(crsql_ExtData *ext_data);

// Preamble: ensure table infos are up to date and find the table
static int trigger_fn_preamble(sqlite3_context *ctx, int argc, sqlite3_value **argv,
                               crsql_TableInfo **table_info_out,
                               crsql_ExtData **ext_data_out,
                               sqlite3_value ***values_out) {
  if (argc < 1) {
    sqlite3_result_error(ctx, "Expected at least 1 argument", -1);
    return SQLITE_ERROR;
  }

  *values_out = argv;
  crsql_ExtData *ext_data = (crsql_ExtData *)sqlite3_user_data(ctx);
  *ext_data_out = ext_data;

  sqlite3 *db = sqlite3_context_db_handle(ctx);
  char *err = NULL;
  int rc = crsql_ensure_table_infos_are_up_to_date(db, ext_data, &err);
  if (rc != SQLITE_OK) {
    if (err) {
      sqlite3_result_error(ctx, err, -1);
      sqlite3_free(err);
    } else {
      sqlite3_result_error(ctx, "Failed to ensure table infos are up to date", -1);
    }
    return rc;
  }

  const char *table_name = (const char *)sqlite3_value_text(argv[0]);
  if (!table_name) {
    sqlite3_result_error(ctx, "Table name is NULL", -1);
    return SQLITE_ERROR;
  }

  // Find table info
  crsql_TableInfo *table_info = NULL;
  for (int i = 0; i < ext_data->tableInfosLen; i++) {
    if (strcmp(ext_data->tableInfos[i]->tbl_name, table_name) == 0) {
      table_info = ext_data->tableInfos[i];
      break;
    }
  }

  if (!table_info) {
    char *errmsg = sqlite3_mprintf("Table %s not found", table_name);
    sqlite3_result_error(ctx, errmsg, -1);
    sqlite3_free(errmsg);
    return SQLITE_ERROR;
  }

  *table_info_out = table_info;
  return SQLITE_OK;
}

// Step and reset a cached trigger statement
static int step_trigger_stmt(sqlite3_stmt *stmt) {
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) {
    crsql_reset_cached_stmt(stmt);
    return SQLITE_OK;
  }
  crsql_reset_cached_stmt(stmt);
  return SQLITE_ERROR;
}

// Mark a new PK row as created (sentinel)
static int mark_new_pk_row_created(sqlite3 *db, crsql_TableInfo *tbl_info,
                                   sqlite3_int64 key_new, sqlite3_int64 db_version,
                                   int seq) {
  sqlite3_stmt *stmt = NULL;
  int rc = crsql_get_mark_locally_created_stmt(db, tbl_info, &stmt);
  if (rc != SQLITE_OK || !stmt) return SQLITE_ERROR;

  sqlite3_bind_int64(stmt, 1, key_new);
  sqlite3_bind_int64(stmt, 2, db_version);
  sqlite3_bind_int(stmt, 3, seq);
  sqlite3_bind_int64(stmt, 4, db_version);
  sqlite3_bind_int(stmt, 5, seq);

  return step_trigger_stmt(stmt);
}

// Mark a column as locally updated
static int mark_locally_updated(sqlite3 *db, crsql_TableInfo *tbl_info,
                                sqlite3_int64 new_key, crsql_ColumnInfo *col_info,
                                sqlite3_int64 db_version, int seq) {
  sqlite3_stmt *stmt = NULL;
  int rc = crsql_get_mark_locally_updated_stmt(db, tbl_info, &stmt);
  if (rc != SQLITE_OK || !stmt) return SQLITE_ERROR;

  sqlite3_bind_int64(stmt, 1, new_key);
  sqlite3_bind_text(stmt, 2, col_info->name, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 3, db_version);
  sqlite3_bind_int(stmt, 4, seq);
  sqlite3_bind_int64(stmt, 5, db_version);
  sqlite3_bind_int(stmt, 6, seq);

  return step_trigger_stmt(stmt);
}

// Increment sequence
static int bump_seq(crsql_ExtData *ext_data) {
  int seq = ext_data->seq;
  ext_data->seq++;
  return seq;
}

// === AFTER INSERT ===

static int update_create_record(sqlite3 *db, crsql_TableInfo *tbl_info,
                                sqlite3_int64 new_key, sqlite3_int64 db_version,
                                int seq) {
  sqlite3_stmt *stmt = NULL;
  int rc = crsql_get_maybe_mark_locally_reinserted_stmt(db, tbl_info, &stmt);
  if (rc != SQLITE_OK || !stmt) return SQLITE_ERROR;

  sqlite3_bind_int64(stmt, 1, db_version);
  sqlite3_bind_int(stmt, 2, seq);
  sqlite3_bind_int64(stmt, 3, new_key);
  sqlite3_bind_text(stmt, 4, CL_SENTINEL, -1, SQLITE_STATIC);

  return step_trigger_stmt(stmt);
}

static int after_insert_impl(sqlite3 *db, crsql_ExtData *ext_data,
                             crsql_TableInfo *tbl_info, sqlite3_value **pks_new) {
  char *err = NULL;
  sqlite3_int64 db_version = crsql_next_db_version(db, ext_data, -1, &err);
  if (err) {
    sqlite3_free(err);
    return SQLITE_ERROR;
  }

  int create_record_existed;
  sqlite3_int64 key_new;
  int rc = crsql_get_or_create_key_for_insert(db, tbl_info, pks_new,
                                              &create_record_existed, &key_new);
  if (rc != SQLITE_OK) return rc;

  if (tbl_info->non_pks_len == 0) {
    // Just a sentinel record
    int seq = bump_seq(ext_data);
    return mark_new_pk_row_created(db, tbl_info, key_new, db_version, seq);
  } else if (create_record_existed) {
    // Update create record since it already exists
    int seq = bump_seq(ext_data);
    rc = update_create_record(db, tbl_info, key_new, db_version, seq);
    if (rc != SQLITE_OK) return rc;
  }

  // For each non-pk column, create or update the column record
  for (int i = 0; i < tbl_info->non_pks_len; i++) {
    int seq = bump_seq(ext_data);
    rc = mark_locally_updated(db, tbl_info, key_new, tbl_info->non_pks[i],
                             db_version, seq);
    if (rc != SQLITE_OK) return rc;
  }

  return SQLITE_OK;
}

void crsql_after_insert(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  crsql_TableInfo *table_info = NULL;
  crsql_ExtData *ext_data = NULL;
  sqlite3_value **values = NULL;

  int rc = trigger_fn_preamble(ctx, argc, argv, &table_info, &ext_data, &values);
  if (rc != SQLITE_OK) return;

  sqlite3 *db = sqlite3_context_db_handle(ctx);
  rc = after_insert_impl(db, ext_data, table_info, &values[1]);

  if (rc != SQLITE_OK) {
    sqlite3_result_error(ctx, "Failed in after_insert", -1);
  } else {
    sqlite3_result_int64(ctx, 0);
  }
}

// === AFTER DELETE ===

static int after_delete_impl(sqlite3 *db, crsql_ExtData *ext_data,
                             crsql_TableInfo *tbl_info, sqlite3_value **pks_old) {
  char *err = NULL;
  sqlite3_int64 db_version = crsql_next_db_version(db, ext_data, -1, &err);
  if (err) {
    sqlite3_free(err);
    return SQLITE_ERROR;
  }

  int seq = bump_seq(ext_data);

  sqlite3_int64 key;
  int rc = crsql_get_or_create_key_via_raw_values(db, tbl_info, pks_old, &key);
  if (rc != SQLITE_OK) return rc;

  // Mark as deleted
  sqlite3_stmt *stmt = NULL;
  rc = crsql_get_mark_locally_deleted_stmt(db, tbl_info, &stmt);
  if (rc != SQLITE_OK || !stmt) return SQLITE_ERROR;

  sqlite3_bind_int64(stmt, 1, key);
  sqlite3_bind_int64(stmt, 2, db_version);
  sqlite3_bind_int(stmt, 3, seq);
  sqlite3_bind_int64(stmt, 4, db_version);
  sqlite3_bind_int(stmt, 5, seq);

  rc = step_trigger_stmt(stmt);
  if (rc != SQLITE_OK) return rc;

  // Drop clock entries
  rc = crsql_get_merge_delete_drop_clocks_stmt(db, tbl_info, &stmt);
  if (rc != SQLITE_OK || !stmt) return SQLITE_ERROR;

  sqlite3_bind_int64(stmt, 1, key);

  return step_trigger_stmt(stmt);
}

void crsql_after_delete(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  crsql_TableInfo *table_info = NULL;
  crsql_ExtData *ext_data = NULL;
  sqlite3_value **values = NULL;

  int rc = trigger_fn_preamble(ctx, argc, argv, &table_info, &ext_data, &values);
  if (rc != SQLITE_OK) return;

  sqlite3 *db = sqlite3_context_db_handle(ctx);
  rc = after_delete_impl(db, ext_data, table_info, &values[1]);

  if (rc != SQLITE_OK) {
    sqlite3_result_error(ctx, "Failed in after_delete", -1);
  } else {
    sqlite3_result_int64(ctx, 0);
  }
}

// === AFTER UPDATE ===

static int any_value_changed(sqlite3_value **new_vals, sqlite3_value **old_vals, int count) {
  for (int i = 0; i < count; i++) {
    if (crsql_compare_values(new_vals[i], old_vals[i]) != 0) {
      return 1;
    }
  }
  return 0;
}

static int after_update__mark_old_pk_row_deleted(sqlite3 *db, crsql_TableInfo *tbl_info,
                                                 sqlite3_int64 old_key,
                                                 sqlite3_int64 db_version, int seq) {
  sqlite3_stmt *stmt = NULL;
  int rc = crsql_get_mark_locally_deleted_stmt(db, tbl_info, &stmt);
  if (rc != SQLITE_OK || !stmt) return SQLITE_ERROR;

  sqlite3_bind_int64(stmt, 1, old_key);
  sqlite3_bind_int64(stmt, 2, db_version);
  sqlite3_bind_int(stmt, 3, seq);
  sqlite3_bind_int64(stmt, 4, db_version);
  sqlite3_bind_int(stmt, 5, seq);

  return step_trigger_stmt(stmt);
}

static int after_update__move_non_sentinels(sqlite3 *db, crsql_TableInfo *tbl_info,
                                            sqlite3_int64 new_key,
                                            sqlite3_int64 old_key) {
  sqlite3_stmt *stmt = NULL;
  int rc = crsql_get_move_non_sentinels_stmt(db, tbl_info, &stmt);
  if (rc != SQLITE_OK || !stmt) return SQLITE_ERROR;

  sqlite3_bind_int64(stmt, 1, new_key);
  sqlite3_bind_int64(stmt, 2, old_key);

  return step_trigger_stmt(stmt);
}

static int after_update_impl(sqlite3 *db, crsql_ExtData *ext_data,
                             crsql_TableInfo *tbl_info,
                             sqlite3_value **pks_new, sqlite3_value **pks_old,
                             sqlite3_value **non_pks_new, sqlite3_value **non_pks_old) {
  char *err = NULL;
  sqlite3_int64 db_version = crsql_next_db_version(db, ext_data, -1, &err);
  if (err) {
    sqlite3_free(err);
    return SQLITE_ERROR;
  }

  sqlite3_int64 new_key;
  int rc = crsql_get_or_create_key_via_raw_values(db, tbl_info, pks_new, &new_key);
  if (rc != SQLITE_OK) return rc;

  // If primary key changed, treat as delete + insert
  if (any_value_changed(pks_new, pks_old, tbl_info->pks_len)) {
    sqlite3_int64 old_key;
    rc = crsql_get_or_create_key_via_raw_values(db, tbl_info, pks_old, &old_key);
    if (rc != SQLITE_OK) return rc;

    int seq = bump_seq(ext_data);
    rc = after_update__mark_old_pk_row_deleted(db, tbl_info, old_key, db_version, seq);
    if (rc != SQLITE_OK) return rc;

    rc = after_update__move_non_sentinels(db, tbl_info, new_key, old_key);
    if (rc != SQLITE_OK) return rc;

    seq = bump_seq(ext_data);
    rc = mark_new_pk_row_created(db, tbl_info, new_key, db_version, seq);
    if (rc != SQLITE_OK) return rc;
  }

  // For each non-pk column that changed, update clock entry
  for (int i = 0; i < tbl_info->non_pks_len; i++) {
    if (crsql_compare_values(non_pks_new[i], non_pks_old[i]) != 0) {
      int seq = bump_seq(ext_data);
      rc = mark_locally_updated(db, tbl_info, new_key, tbl_info->non_pks[i],
                               db_version, seq);
      if (rc != SQLITE_OK) return rc;
    }
  }

  return SQLITE_OK;
}

void crsql_after_update(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  crsql_TableInfo *table_info = NULL;
  crsql_ExtData *ext_data = NULL;
  sqlite3_value **values = NULL;

  int rc = trigger_fn_preamble(ctx, argc, argv, &table_info, &ext_data, &values);
  if (rc != SQLITE_OK) return;

  // Partition values: [table_name, pk_new..., pk_old..., non_pk_new..., non_pk_old...]
  int num_pks = table_info->pks_len;
  int num_non_pks = table_info->non_pks_len;
  int expected_len = 1 + num_pks * 2 + num_non_pks * 2;

  if (argc != expected_len) {
    char *errmsg = sqlite3_mprintf("Expected %d values, got %d", expected_len, argc);
    sqlite3_result_error(ctx, errmsg, -1);
    sqlite3_free(errmsg);
    return;
  }

  sqlite3_value **pks_new = &values[1];
  sqlite3_value **pks_old = &values[1 + num_pks];
  sqlite3_value **non_pks_new = &values[1 + num_pks * 2];
  sqlite3_value **non_pks_old = &values[1 + num_pks * 2 + num_non_pks];

  sqlite3 *db = sqlite3_context_db_handle(ctx);
  rc = after_update_impl(db, ext_data, table_info, pks_new, pks_old,
                        non_pks_new, non_pks_old);

  if (rc != SQLITE_OK) {
    sqlite3_result_error(ctx, "Failed in after_update", -1);
  } else {
    sqlite3_result_int64(ctx, 0);
  }
}
