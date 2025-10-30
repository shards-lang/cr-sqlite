#ifndef CRSQLITE_TABLEINFO_H
#define CRSQLITE_TABLEINFO_H

#include "sqlite3ext.h"
#include "ext-data.h"

// Column information
typedef struct crsql_ColumnInfo {
  char *name;
  char *type;
  int notnull;
  int pk_index;  // 0 if not a pk, otherwise the index in the pk
} crsql_ColumnInfo;

// Table information and metadata with cached statements
typedef struct crsql_TableInfo {
  char *tbl_name;
  crsql_ColumnInfo **pks;
  int pks_len;
  crsql_ColumnInfo **non_pks;
  int non_pks_len;

  // Cached statements (lazily initialized)
  // For lookaside key management
  sqlite3_stmt *select_key_stmt;
  sqlite3_stmt *insert_key_stmt;
  sqlite3_stmt *insert_or_ignore_returning_key_stmt;

  // For merges
  sqlite3_stmt *set_winner_clock_stmt;
  sqlite3_stmt *local_cl_stmt;
  sqlite3_stmt *col_version_stmt;
  sqlite3_stmt *col_site_id_stmt;
  sqlite3_stmt *merge_pk_only_insert_stmt;
  sqlite3_stmt *merge_delete_stmt;
  sqlite3_stmt *merge_delete_drop_clocks_stmt;
  sqlite3_stmt *zero_clocks_on_resurrect_stmt;

  // For local writes
  sqlite3_stmt *mark_locally_deleted_stmt;
  sqlite3_stmt *move_non_sentinels_stmt;
  sqlite3_stmt *mark_locally_created_stmt;
  sqlite3_stmt *mark_locally_updated_stmt;
  sqlite3_stmt *maybe_mark_locally_reinserted_stmt;
} crsql_TableInfo;

// Create table info from database schema
crsql_TableInfo *crsql_extract_table_info(sqlite3 *db, const char *table_name);

// Free table info and all cached statements
void crsql_free_table_info(crsql_TableInfo *tbl_info);

// Ensure all table infos are up to date
int crsql_ensure_table_infos_are_up_to_date(sqlite3 *db, crsql_ExtData *ext_data, char **err);

// Check if table schema is compatible with CRR
int crsql_is_table_compatible(sqlite3 *db, const char *tbl_name, char **err);

// Get or create primary key in __crsql_pks table
sqlite3_int64 crsql_get_or_create_key(sqlite3 *db, crsql_TableInfo *tbl_info,
                                      sqlite3_value **pks, int pk_count);

#endif
