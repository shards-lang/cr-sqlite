#ifndef CRSQLITE_TABLEINFO_H
#define CRSQLITE_TABLEINFO_H

#include "crsqlite.h"
#include "ext-data.h"
#include "pack-columns.h"

typedef struct crsql_ColumnInfo crsql_ColumnInfo;
struct crsql_ColumnInfo {
  int cid;
  char *name;  // sqlite3_malloc'd
  int pk;      // >0 if PK column, value = position in PK definition

  // Per-column cached statements (NULL until first use)
  sqlite3_stmt *pCurrValueStmt;
  sqlite3_stmt *pMergeInsertStmt;
  sqlite3_stmt *pRowPatchDataStmt;
};

typedef struct crsql_TableInfo crsql_TableInfo;
struct crsql_TableInfo {
  char *tblName;  // sqlite3_malloc'd

  crsql_ColumnInfo *pks;
  int pksLen;
  crsql_ColumnInfo *nonPks;
  int nonPksLen;

  // Cached prepared statements (NULL until first use, lazy-initialized)
  sqlite3_stmt *pSelectKeyStmt;
  sqlite3_stmt *pInsertKeyStmt;
  sqlite3_stmt *pInsertOrIgnoreReturningKeyStmt;
  sqlite3_stmt *pSetWinnerClockStmt;
  sqlite3_stmt *pLocalClStmt;
  sqlite3_stmt *pColVersionStmt;
  sqlite3_stmt *pColSiteIdStmt;
  sqlite3_stmt *pMergePkOnlyInsertStmt;
  sqlite3_stmt *pMergeDeleteStmt;
  sqlite3_stmt *pMergeDeleteDropClocksStmt;
  sqlite3_stmt *pZeroClocksOnResurrectStmt;
  sqlite3_stmt *pMarkLocallyDeletedStmt;
  sqlite3_stmt *pMoveNonSentinelsStmt;
  sqlite3_stmt *pMarkLocallyCreatedStmt;
  sqlite3_stmt *pMarkLocallyUpdatedStmt;
  sqlite3_stmt *pMaybeMarkLocallyReinsertedStmt;
};

typedef struct crsql_TableInfoVec crsql_TableInfoVec;
struct crsql_TableInfoVec {
  crsql_TableInfo *aInfos;
  int len;
  int capacity;
};

// --- TableInfoVec lifecycle ---
void crsql_init_table_info_vec(crsql_ExtData *pExtData);
void crsql_drop_table_info_vec(crsql_ExtData *pExtData);

// --- Statement cache ---
void crsql_clear_stmt_cache(crsql_ExtData *pExtData);

// --- TableInfo creation/destruction ---
int crsql_pull_table_info(sqlite3 *db, const char *tblName,
                          crsql_TableInfo **ppTableInfo, char **errmsg);
void crsql_free_table_info(crsql_TableInfo *pTableInfo);

// --- Table compatibility check ---
int crsql_is_table_compatible(sqlite3 *db, const char *tblName, char **err);

// --- Ensure table infos are current ---
int crsql_ensure_table_infos_are_up_to_date(sqlite3 *db,
                                            crsql_ExtData *pExtData,
                                            char **errmsg);

// --- Key management ---
sqlite3_int64 crsql_get_or_create_key(sqlite3 *db, crsql_TableInfo *tblInfo,
                                      sqlite3_value **pks, int numPks,
                                      char **errmsg);
sqlite3_int64 crsql_get_or_create_key_for_insert(sqlite3 *db,
                                                  crsql_TableInfo *tblInfo,
                                                  sqlite3_value **pks,
                                                  int numPks, char **errmsg);
sqlite3_int64 crsql_get_key(sqlite3 *db, crsql_TableInfo *tblInfo,
                            sqlite3_value **pks, int numPks);
sqlite3_int64 crsql_get_or_create_key_packed(sqlite3 *db,
                                             crsql_TableInfo *tblInfo,
                                             crsql_ColumnValue *pks,
                                             int numPks, char **errmsg);

// --- Lazy statement getters ---
// These prepare statements on first call, cache for subsequent calls.
int crsql_get_set_winner_clock_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                    sqlite3_stmt **ppStmt);
int crsql_get_local_cl_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                            sqlite3_stmt **ppStmt);
int crsql_get_col_version_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                               sqlite3_stmt **ppStmt);
int crsql_get_col_site_id_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                               sqlite3_stmt **ppStmt);
int crsql_get_merge_pk_only_insert_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                        sqlite3_stmt **ppStmt);
int crsql_get_merge_delete_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                sqlite3_stmt **ppStmt);
int crsql_get_merge_delete_drop_clocks_stmt(sqlite3 *db,
                                            crsql_TableInfo *tblInfo,
                                            sqlite3_stmt **ppStmt);
int crsql_get_zero_clocks_on_resurrect_stmt(sqlite3 *db,
                                            crsql_TableInfo *tblInfo,
                                            sqlite3_stmt **ppStmt);
int crsql_get_mark_locally_deleted_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                        sqlite3_stmt **ppStmt);
int crsql_get_move_non_sentinels_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                      sqlite3_stmt **ppStmt);
int crsql_get_mark_locally_created_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                        sqlite3_stmt **ppStmt);
int crsql_get_mark_locally_updated_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                        sqlite3_stmt **ppStmt);
int crsql_get_maybe_mark_locally_reinserted_stmt(sqlite3 *db,
                                                 crsql_TableInfo *tblInfo,
                                                 sqlite3_stmt **ppStmt);

// --- Clear statements for a single table ---
void crsql_clear_table_info_stmts(crsql_TableInfo *tblInfo);

// --- Find table info by name ---
int crsql_find_table_info(crsql_TableInfoVec *vec, const char *tblName);

// --- Per-column statement accessors (find non-PK col by name) ---
int crsql_get_col_value_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                             const char *colName, sqlite3_stmt **ppStmt);
int crsql_get_merge_insert_col_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                    const char *colName,
                                    sqlite3_stmt **ppStmt);
int crsql_get_row_patch_data_stmt(sqlite3 *db, crsql_TableInfo *tblInfo,
                                  const char *colName,
                                  sqlite3_stmt **ppStmt);

#endif
