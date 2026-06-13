#ifndef CRSQLITE_EXTDATA_H
#define CRSQLITE_EXTDATA_H

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT3

#define CRSQL_SITE_ID_MEMO_LEN 16

typedef struct crsql_ExtData crsql_ExtData;
struct crsql_ExtData {
  // perma statement -- used to check db schema version
  sqlite3_stmt *pPragmaSchemaVersionStmt;
  sqlite3_stmt *pPragmaDataVersionStmt;
  int pragmaDataVersion;

  // this gets set at the start of each transaction on the first invocation
  // to crsql_next_db_version()
  // and re-set on transaction commit or rollback.
  sqlite3_int64 dbVersion;
  // the version that the db will be set to at the end of the transaction
  // if that transaction were to commit at the time this value is checked.
  sqlite3_int64 pendingDbVersion;
  int pragmaSchemaVersion;
  int updatedTableInfosThisTx;

  // we need another schema version number that tracks when we checked it
  // for zpTableInfos.
  int pragmaSchemaVersionForTableInfos;

  unsigned char *siteId;
  sqlite3_stmt *pDbVersionStmt;
  void *tableInfos;

  // tracks the number of rows impacted by all inserts into crsql_changes in the
  // current transaction. This number is reset on transaction commit.
  int rowsImpacted;

  int seq;

  sqlite3_stmt *pSetSiteIdOrdinalStmt;
  sqlite3_stmt *pSelectSiteIdOrdinalStmt;
  sqlite3_stmt *pSelectClockTablesStmt;

  int mergeEqualValues;

  // Borrowed pointer to the sync bit owned by the
  // `crsql_internal_sync_bit` SQL function. Lets the merge path toggle the
  // bit directly instead of executing `SELECT crsql_internal_sync_bit(x)`
  // per merged change.
  int *syncBitPtr;

  // ---- merge fast-path memos ----
  // These are pure caches for the crsql_changes insert (merge) path.
  // They are invalidated via crsql_invalidate_merge_memos on transaction
  // commit/rollback, on savepoint rollback over the changes vtab, on local
  // writes to CRRs and whenever table infos are reloaded.

  // Memo of the last site_id -> ordinal lookup. A changeset is typically
  // authored by a single site so this hits ~100% during sync.
  int cachedSiteIdLen;  // 0 = no memo
  unsigned char cachedSiteId[CRSQL_SITE_ID_MEMO_LEN];
  sqlite3_int64 cachedSiteIdOrdinal;

  // Memo of the last (table, packed pks) -> (lookaside key, causal length)
  // resolution. Changesets are ordered by (db_version, seq) so the N column
  // changes of a single row arrive back-to-back; this skips the pk lookaside
  // lookup and causal length select for all but the first change of a row.
  int rowMemoTblInfoIdx;  // -1 = no memo
  int rowMemoPkLen;
  int rowMemoPkCap;
  unsigned char *rowMemoPkBlob;  // sqlite3_malloc'd, owned
  sqlite3_int64 rowMemoKey;
  sqlite3_int64 rowMemoLocalCl;
};

crsql_ExtData *crsql_newExtData(sqlite3 *db, unsigned char *siteIdBuffer);
void crsql_freeExtData(crsql_ExtData *pExtData);
int crsql_fetchPragmaSchemaVersion(sqlite3 *db, crsql_ExtData *pExtData,
                                   int which);
int crsql_fetchPragmaDataVersion(sqlite3 *db, crsql_ExtData *pExtData);
int crsql_recreate_db_version_stmt(sqlite3 *db, crsql_ExtData *pExtData);
void crsql_finalize(crsql_ExtData *pExtData);

// Drop the merge fast-path memos (site_id ordinal + last row key/cl).
// Must be called whenever cached state could go stale: tx boundaries,
// savepoint rollbacks, local CRR writes, table info reloads.
void crsql_invalidate_merge_memos(crsql_ExtData *pExtData);

#endif