#include "tableinfo.h"
#include "consts.h"
#include "str_util.h"
#include <string.h>
#include <stdio.h>

// Extract table info from SQLite schema
crsql_TableInfo *crsql_extract_table_info(sqlite3 *db, const char *table_name) {
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, "SELECT name, type, \"notnull\", pk FROM pragma_table_info(?)",
                               -1, &stmt, NULL);
  if (rc != SQLITE_OK) return NULL;

  rc = sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return NULL;
  }

  // Allocate table info
  crsql_TableInfo *tbl_info = sqlite3_malloc(sizeof(crsql_TableInfo));
  if (!tbl_info) {
    sqlite3_finalize(stmt);
    return NULL;
  }
  memset(tbl_info, 0, sizeof(crsql_TableInfo));

  tbl_info->tbl_name = sqlite3_mprintf("%s", table_name);
  if (!tbl_info->tbl_name) {
    sqlite3_free(tbl_info);
    sqlite3_finalize(stmt);
    return NULL;
  }

  // Temporary arrays to collect columns
  crsql_ColumnInfo **all_cols = NULL;
  int num_cols = 0;
  int cap_cols = 8;

  all_cols = sqlite3_malloc(sizeof(crsql_ColumnInfo*) * cap_cols);
  if (!all_cols) {
    sqlite3_free(tbl_info->tbl_name);
    sqlite3_free(tbl_info);
    sqlite3_finalize(stmt);
    return NULL;
  }

  // Read all columns
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (num_cols >= cap_cols) {
      cap_cols *= 2;
      crsql_ColumnInfo **new_cols = sqlite3_realloc(all_cols, sizeof(crsql_ColumnInfo*) * cap_cols);
      if (!new_cols) {
        for (int i = 0; i < num_cols; i++) {
          sqlite3_free(all_cols[i]->name);
          sqlite3_free(all_cols[i]->type);
          sqlite3_free(all_cols[i]);
        }
        sqlite3_free(all_cols);
        sqlite3_free(tbl_info->tbl_name);
        sqlite3_free(tbl_info);
        sqlite3_finalize(stmt);
        return NULL;
      }
      all_cols = new_cols;
    }

    crsql_ColumnInfo *col = sqlite3_malloc(sizeof(crsql_ColumnInfo));
    if (!col) continue;

    col->name = sqlite3_mprintf("%s", sqlite3_column_text(stmt, 0));
    col->type = sqlite3_mprintf("%s", sqlite3_column_text(stmt, 1));
    col->notnull = sqlite3_column_int(stmt, 2);
    col->pk_index = sqlite3_column_int(stmt, 3);

    all_cols[num_cols++] = col;
  }

  sqlite3_finalize(stmt);

  // Separate PKs from non-PKs
  int pk_count = 0;
  int non_pk_count = 0;

  for (int i = 0; i < num_cols; i++) {
    if (all_cols[i]->pk_index > 0) pk_count++;
    else non_pk_count++;
  }

  tbl_info->pks = sqlite3_malloc(sizeof(crsql_ColumnInfo*) * (pk_count + 1));
  tbl_info->non_pks = sqlite3_malloc(sizeof(crsql_ColumnInfo*) * (non_pk_count + 1));

  if (!tbl_info->pks || !tbl_info->non_pks) {
    for (int i = 0; i < num_cols; i++) {
      sqlite3_free(all_cols[i]->name);
      sqlite3_free(all_cols[i]->type);
      sqlite3_free(all_cols[i]);
    }
    sqlite3_free(all_cols);
    sqlite3_free(tbl_info->pks);
    sqlite3_free(tbl_info->non_pks);
    sqlite3_free(tbl_info->tbl_name);
    sqlite3_free(tbl_info);
    return NULL;
  }

  int pk_idx = 0;
  int non_pk_idx = 0;

  for (int i = 0; i < num_cols; i++) {
    if (all_cols[i]->pk_index > 0) {
      tbl_info->pks[pk_idx++] = all_cols[i];
    } else {
      tbl_info->non_pks[non_pk_idx++] = all_cols[i];
    }
  }

  tbl_info->pks_len = pk_count;
  tbl_info->non_pks_len = non_pk_count;

  sqlite3_free(all_cols);

  return tbl_info;
}

// Free table info
void crsql_free_table_info(crsql_TableInfo *tbl_info) {
  if (!tbl_info) return;

  sqlite3_free(tbl_info->tbl_name);

  for (int i = 0; i < tbl_info->pks_len; i++) {
    sqlite3_free(tbl_info->pks[i]->name);
    sqlite3_free(tbl_info->pks[i]->type);
    sqlite3_free(tbl_info->pks[i]);
  }

  for (int i = 0; i < tbl_info->non_pks_len; i++) {
    sqlite3_free(tbl_info->non_pks[i]->name);
    sqlite3_free(tbl_info->non_pks[i]->type);
    sqlite3_free(tbl_info->non_pks[i]);
  }

  sqlite3_free(tbl_info->pks);
  sqlite3_free(tbl_info->non_pks);

  // Finalize all cached statements
  sqlite3_finalize(tbl_info->select_key_stmt);
  sqlite3_finalize(tbl_info->insert_key_stmt);
  sqlite3_finalize(tbl_info->insert_or_ignore_returning_key_stmt);
  sqlite3_finalize(tbl_info->set_winner_clock_stmt);
  sqlite3_finalize(tbl_info->local_cl_stmt);
  sqlite3_finalize(tbl_info->col_version_stmt);
  sqlite3_finalize(tbl_info->col_site_id_stmt);
  sqlite3_finalize(tbl_info->merge_pk_only_insert_stmt);
  sqlite3_finalize(tbl_info->merge_delete_stmt);
  sqlite3_finalize(tbl_info->merge_delete_drop_clocks_stmt);
  sqlite3_finalize(tbl_info->zero_clocks_on_resurrect_stmt);
  sqlite3_finalize(tbl_info->mark_locally_deleted_stmt);
  sqlite3_finalize(tbl_info->move_non_sentinels_stmt);
  sqlite3_finalize(tbl_info->mark_locally_created_stmt);
  sqlite3_finalize(tbl_info->mark_locally_updated_stmt);
  sqlite3_finalize(tbl_info->maybe_mark_locally_reinserted_stmt);

  sqlite3_free(tbl_info);
}

// Check if table is compatible (has primary key)
int crsql_is_table_compatible(sqlite3 *db, const char *tbl_name, char **err) {
  crsql_TableInfo *info = crsql_extract_table_info(db, tbl_name);
  if (!info) {
    if (err) *err = sqlite3_mprintf("Failed to extract table info");
    return SQLITE_ERROR;
  }

  if (info->pks_len == 0) {
    if (err) *err = sqlite3_mprintf("Table %s has no primary key", tbl_name);
    crsql_free_table_info(info);
    return SQLITE_ERROR;
  }

  crsql_free_table_info(info);
  return SQLITE_OK;
}

// Stub: Ensure table infos are up to date
// This will need full implementation with schema version checking
int crsql_ensure_table_infos_are_up_to_date(sqlite3 *db, crsql_ExtData *ext_data, char **err) {
  // TODO: Full implementation with schema version check and table info caching
  // For now, just return OK as a stub
  return SQLITE_OK;
}

// Get or create key in __crsql_pks table
sqlite3_int64 crsql_get_or_create_key(sqlite3 *db, crsql_TableInfo *tbl_info,
                                      sqlite3_value **pks, int pk_count) {
  // Build SELECT query if not cached
  if (!tbl_info->select_key_stmt) {
    char *pk_list = crsql_as_identifier_list(tbl_info->pks, tbl_info->pks_len, NULL);
    char *where_list = crsql_where_list(tbl_info->pks, tbl_info->pks_len, NULL);

    char *sql = sqlite3_mprintf(
      "SELECT __crsql_key FROM \"%s__crsql_pks\" WHERE %s",
      tbl_info->tbl_name, where_list
    );

    sqlite3_free(pk_list);
    sqlite3_free(where_list);

    if (!sql) return -1;

    int rc = sqlite3_prepare_v2(db, sql, -1, &tbl_info->select_key_stmt, NULL);
    sqlite3_free(sql);

    if (rc != SQLITE_OK) return -1;
  }

  // Bind PK values
  for (int i = 0; i < pk_count; i++) {
    sqlite3_bind_value(tbl_info->select_key_stmt, i + 1, pks[i]);
  }

  // Try to find existing key
  int rc = sqlite3_step(tbl_info->select_key_stmt);
  if (rc == SQLITE_ROW) {
    sqlite3_int64 key = sqlite3_column_int64(tbl_info->select_key_stmt, 0);
    sqlite3_reset(tbl_info->select_key_stmt);
    return key;
  }

  sqlite3_reset(tbl_info->select_key_stmt);

  // Key doesn't exist, create it
  if (!tbl_info->insert_key_stmt) {
    char *pk_list = crsql_as_identifier_list(tbl_info->pks, tbl_info->pks_len, NULL);
    char *binding_list = crsql_binding_list(tbl_info->pks_len);

    char *sql = sqlite3_mprintf(
      "INSERT INTO \"%s__crsql_pks\" (%s) VALUES (%s) RETURNING __crsql_key",
      tbl_info->tbl_name, pk_list, binding_list
    );

    sqlite3_free(pk_list);
    sqlite3_free(binding_list);

    if (!sql) return -1;

    rc = sqlite3_prepare_v2(db, sql, -1, &tbl_info->insert_key_stmt, NULL);
    sqlite3_free(sql);

    if (rc != SQLITE_OK) return -1;
  }

  // Bind and insert
  for (int i = 0; i < pk_count; i++) {
    sqlite3_bind_value(tbl_info->insert_key_stmt, i + 1, pks[i]);
  }

  rc = sqlite3_step(tbl_info->insert_key_stmt);
  if (rc == SQLITE_ROW) {
    sqlite3_int64 key = sqlite3_column_int64(tbl_info->insert_key_stmt, 0);
    sqlite3_reset(tbl_info->insert_key_stmt);
    return key;
  }

  sqlite3_reset(tbl_info->insert_key_stmt);
  return -1;
}
