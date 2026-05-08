/**
 * changes-vtab-impl.c
 *
 * Implementation of the crsql_changes virtual table methods.
 * Ported from Rust: changes_vtab.rs, changes_vtab_read.rs, changes_vtab_write.rs
 */
#include "changes-vtab.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "consts.h"
#include "ext-data.h"
#include "pack-columns.h"
#include "tableinfo.h"
#include "util.h"

// Column enums for the changes virtual table output
enum CrsqlChangesColumn {
  CHANGES_COL_TBL = 0,
  CHANGES_COL_PK = 1,
  CHANGES_COL_CID = 2,
  CHANGES_COL_CVAL = 3,
  CHANGES_COL_COL_VRSN = 4,
  CHANGES_COL_DB_VRSN = 5,
  CHANGES_COL_SITE_ID = 6,
  CHANGES_COL_CL = 7,
  CHANGES_COL_SEQ = 8,
};

// Column enums for the clock union query result
enum ClockUnionColumn {
  CLOCK_COL_TBL = 0,
  CLOCK_COL_PKS = 1,
  CLOCK_COL_CID = 2,
  CLOCK_COL_COL_VRSN = 3,
  CLOCK_COL_DB_VRSN = 4,
  CLOCK_COL_SITE_ID = 5,
  CLOCK_COL_ROWID = 6,
  CLOCK_COL_SEQ = 7,
  CLOCK_COL_CL = 8,
};

// ---------- Internal helpers ----------

static int resetCachedStmt(sqlite3_stmt *pStmt) {
  if (pStmt == 0) return SQLITE_OK;
  sqlite3_clear_bindings(pStmt);
  return sqlite3_reset(pStmt);
}

int changesCrsrFinalize(crsql_Changes_cursor *crsr) {
  int rc = SQLITE_OK;
  rc += sqlite3_finalize(crsr->pChangesStmt);
  crsr->pChangesStmt = 0;
  if (crsr->pRowStmt != 0) {
    rc += sqlite3_clear_bindings(crsr->pRowStmt);
    rc += sqlite3_reset(crsr->pRowStmt);
  }
  crsr->pRowStmt = 0;
  crsr->dbVersion = MIN_POSSIBLE_DB_VERSION;
  return rc;
}

// ---------- Query generation ----------

/**
 * Build the per-table SELECT for the changes union query.
 * Returns a sqlite3_malloc'd string. Caller must sqlite3_free.
 */
static char *changes_query_for_table(crsql_TableInfo *tblInfo) {
  if (tblInfo->pksLen == 0) {
    return 0;
  }

  char *pk_list =
      crsql_as_identifier_list(tblInfo->pks, tblInfo->pksLen, "pk_tbl.");
  if (!pk_list) return 0;

  char *escaped_tbl_name = crsql_escape_ident(tblInfo->tblName);
  char *escaped_tbl_val = crsql_escape_ident_as_value(tblInfo->tblName);
  if (!escaped_tbl_name || !escaped_tbl_val) {
    sqlite3_free(pk_list);
    sqlite3_free(escaped_tbl_name);
    sqlite3_free(escaped_tbl_val);
    return 0;
  }

  char *sql = sqlite3_mprintf(
      "SELECT"
      " '%s' as tbl,"
      " crsql_pack_columns(%s) as pks,"
      " t1.col_name as cid,"
      " t1.col_version as col_vrsn,"
      " t1.db_version as db_vrsn,"
      " site_tbl.site_id as site_id,"
      " t1.key,"
      " t1.seq as seq,"
      " COALESCE(t2.col_version, 1) as cl"
      " FROM \"%s__crsql_clock\" AS t1"
      " JOIN \"%s__crsql_pks\" AS pk_tbl ON t1.key = pk_tbl.__crsql_key"
      " LEFT JOIN crsql_site_id AS site_tbl ON t1.site_id = site_tbl.ordinal"
      " LEFT JOIN \"%s__crsql_clock\" AS t2 ON"
      " t1.key = t2.key AND t2.col_name = '%s'",
      escaped_tbl_val, pk_list, escaped_tbl_name, escaped_tbl_name,
      escaped_tbl_name, SENTINEL_CID);

  sqlite3_free(pk_list);
  sqlite3_free(escaped_tbl_name);
  sqlite3_free(escaped_tbl_val);

  return sql;
}

/**
 * Build the UNION ALL query across all clock tables.
 * Returns a sqlite3_malloc'd string. Caller must sqlite3_free.
 */
static char *changes_union_query(crsql_TableInfoVec *tblInfos,
                                 const char *idxStr) {
  if (tblInfos->len == 0) return 0;

  // Build UNION ALL of all per-table queries
  char *unions = 0;
  for (int i = 0; i < tblInfos->len; i++) {
    char *sub = changes_query_for_table(&tblInfos->aInfos[i]);
    if (!sub) {
      sqlite3_free(unions);
      return 0;
    }
    if (unions == 0) {
      unions = sub;
    } else {
      char *prev = unions;
      unions = sqlite3_mprintf("%s UNION ALL %s", prev, sub);
      sqlite3_free(prev);
      sqlite3_free(sub);
      if (!unions) return 0;
    }
  }

  char *sql = sqlite3_mprintf(
      "SELECT tbl, pks, cid, col_vrsn, db_vrsn, site_id, key, seq, cl"
      " FROM (%s) %s",
      unions, idxStr);

  sqlite3_free(unions);
  return sql;
}

// ---------- Operator / column name helpers ----------

static const char *get_operator_string(unsigned char op) {
  switch ((int)op) {
    case SQLITE_INDEX_CONSTRAINT_EQ:
      return "=";
    case SQLITE_INDEX_CONSTRAINT_GT:
      return ">";
    case SQLITE_INDEX_CONSTRAINT_LE:
      return "<=";
    case SQLITE_INDEX_CONSTRAINT_LT:
      return "<";
    case SQLITE_INDEX_CONSTRAINT_GE:
      return ">=";
    case SQLITE_INDEX_CONSTRAINT_MATCH:
      return "MATCH";
    case SQLITE_INDEX_CONSTRAINT_LIKE:
      return "LIKE";
    case SQLITE_INDEX_CONSTRAINT_GLOB:
      return "GLOB";
    case SQLITE_INDEX_CONSTRAINT_REGEXP:
      return "REGEXP";
    case SQLITE_INDEX_CONSTRAINT_NE:
      return "!=";
    case SQLITE_INDEX_CONSTRAINT_ISNOT:
      return "IS NOT";
    case SQLITE_INDEX_CONSTRAINT_ISNOTNULL:
      return "IS NOT NULL";
    case SQLITE_INDEX_CONSTRAINT_ISNULL:
      return "IS NULL";
    case SQLITE_INDEX_CONSTRAINT_IS:
      return "IS";
    default:
      return 0;
  }
}

/**
 * Returns the clock-table column name for a given changes column index,
 * or NULL if the column is not filterable (e.g., Cval).
 */
static const char *get_clock_table_col_name(int col) {
  switch (col) {
    case CHANGES_COL_TBL:
      return "tbl";
    case CHANGES_COL_PK:
      return "pks";
    case CHANGES_COL_CID:
      return "cid";
    case CHANGES_COL_CVAL:
      return 0;
    case CHANGES_COL_COL_VRSN:
      return "col_vrsn";
    case CHANGES_COL_DB_VRSN:
      return "db_vrsn";
    case CHANGES_COL_SITE_ID:
      return "site_id";
    case CHANGES_COL_SEQ:
      return "seq";
    case CHANGES_COL_CL:
      return "cl";
    default:
      return 0;
  }
}

static int constraint_is_usable(int iColumn, unsigned char usable) {
  if (!usable) return 0;
  switch (iColumn) {
    case CHANGES_COL_TBL:
    case CHANGES_COL_PK:
    case CHANGES_COL_CVAL:
      return 0;
    case CHANGES_COL_CID:
    case CHANGES_COL_COL_VRSN:
    case CHANGES_COL_DB_VRSN:
    case CHANGES_COL_SITE_ID:
    case CHANGES_COL_CL:
    case CHANGES_COL_SEQ:
      return 1;
    default:
      return 0;
  }
}

// ---------- xBestIndex ----------

int crsql_changes_best_index(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo) {
  int idxNum = 0;
  int firstConstraint = 1;
  int argvIndex = 1;

  // We build the idxStr dynamically.
  // Start with a generous buffer; we'll copy into sqlite3_malloc at the end.
  int bufSize = 1024;
  int bufUsed = 0;
  char *buf = sqlite3_malloc(bufSize);
  if (!buf) return SQLITE_NOMEM;
  buf[0] = '\0';

#define APPEND_STR(s)                                          \
  do {                                                         \
    int _len = (int)strlen(s);                                 \
    if (bufUsed + _len + 1 > bufSize) {                        \
      bufSize = (bufUsed + _len + 1) * 2;                     \
      char *_new = sqlite3_realloc(buf, bufSize);              \
      if (!_new) {                                             \
        sqlite3_free(buf);                                     \
        return SQLITE_NOMEM;                                   \
      }                                                        \
      buf = _new;                                              \
    }                                                          \
    memcpy(buf + bufUsed, s, _len);                            \
    bufUsed += _len;                                           \
    buf[bufUsed] = '\0';                                       \
  } while (0)

  for (int i = 0; i < pIdxInfo->nConstraint; i++) {
    struct sqlite3_index_constraint *c = &pIdxInfo->aConstraint[i];
    if (!constraint_is_usable(c->iColumn, c->usable)) {
      continue;
    }
    const char *colName = get_clock_table_col_name(c->iColumn);
    if (!colName) continue;
    const char *opStr = get_operator_string(c->op);
    if (!opStr) continue;

    if (firstConstraint) {
      APPEND_STR("WHERE ");
      firstConstraint = 0;
    } else {
      APPEND_STR(" AND ");
    }

    if (c->op == SQLITE_INDEX_CONSTRAINT_ISNOTNULL ||
        c->op == SQLITE_INDEX_CONSTRAINT_ISNULL) {
      APPEND_STR(colName);
      APPEND_STR(" ");
      APPEND_STR(opStr);
      pIdxInfo->aConstraintUsage[i].argvIndex = 0;
      pIdxInfo->aConstraintUsage[i].omit = 1;
    } else {
      APPEND_STR(colName);
      APPEND_STR(" ");
      APPEND_STR(opStr);
      APPEND_STR(" ?");
      pIdxInfo->aConstraintUsage[i].argvIndex = argvIndex;
      pIdxInfo->aConstraintUsage[i].omit = 1;
      argvIndex++;
    }

    // idx bit mask
    if (c->iColumn == CHANGES_COL_DB_VRSN) {
      idxNum |= 2;
    } else if (c->iColumn == CHANGES_COL_SITE_ID) {
      idxNum |= 4;
    }
  }

  // ORDER BY
  int orderByConsumed = 1;
  if (pIdxInfo->nOrderBy > 0) {
    APPEND_STR(" ORDER BY ");
  } else {
    APPEND_STR(" ORDER BY db_vrsn, seq ASC");
  }

  int firstOrder = 1;
  for (int i = 0; i < pIdxInfo->nOrderBy; i++) {
    const char *colName =
        get_clock_table_col_name(pIdxInfo->aOrderBy[i].iColumn);
    if (colName) {
      if (firstOrder) {
        firstOrder = 0;
      } else {
        APPEND_STR(", ");
      }
      APPEND_STR(colName);
      if (pIdxInfo->aOrderBy[i].desc) {
        APPEND_STR(" DESC");
      } else {
        APPEND_STR(" ASC");
      }
    } else {
      orderByConsumed = 0;
    }
  }

#undef APPEND_STR

  // Cost estimation
  if ((idxNum & 6) == 6) {
    // both db_version and site_id constraints
    pIdxInfo->estimatedCost = 1.0;
    pIdxInfo->estimatedRows = 1;
  } else if ((idxNum & 2) == 2) {
    // only version constraint
    pIdxInfo->estimatedCost = 10.0;
    pIdxInfo->estimatedRows = 10;
  } else if ((idxNum & 4) == 4) {
    // only site_id constraint
    pIdxInfo->estimatedCost = 2147483647.0;
    pIdxInfo->estimatedRows = 2147483647;
  } else {
    // no constraints
    pIdxInfo->estimatedCost = 2147483647.0;
    pIdxInfo->estimatedRows = 2147483647;
  }

  pIdxInfo->idxNum = idxNum;
  pIdxInfo->orderByConsumed = orderByConsumed;
  pIdxInfo->idxStr = buf;
  pIdxInfo->needToFreeIdxStr = 1;

  return SQLITE_OK;
}

// ---------- xFilter ----------

static int changes_next_impl(crsql_Changes_cursor *cursor,
                              sqlite3_vtab *vtab);

int crsql_changes_filter(sqlite3_vtab_cursor *pVtabCursor, int idxNum,
                          const char *idxStr, int argc,
                          sqlite3_value **argv) {
  crsql_Changes_cursor *cursor = (crsql_Changes_cursor *)pVtabCursor;
  crsql_Changes_vtab *tab = cursor->pTab;
  sqlite3 *db = tab->db;

  // pChangesStmt should be finalized before filter is ever invoked.
  if (cursor->pChangesStmt != 0) {
    sqlite3_finalize(cursor->pChangesStmt);
    cursor->pChangesStmt = 0;
  }

  int rc = crsql_ensure_table_infos_are_up_to_date(
      db, tab->pExtData, &((sqlite3_vtab *)tab)->zErrMsg);
  if (rc != SQLITE_OK) {
    return rc;
  }

  // Nothing to fetch if no CRRs exist.
  crsql_TableInfoVec *tblInfos =
      (crsql_TableInfoVec *)tab->pExtData->tableInfos;
  if (tblInfos == 0 || tblInfos->len == 0) {
    return SQLITE_OK;
  }

  char *sql = changes_union_query(tblInfos, idxStr);
  if (!sql) {
    return SQLITE_ERROR;
  }

  sqlite3_stmt *pStmt = 0;
  rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    return rc;
  }

  for (int i = 0; i < argc; i++) {
    rc = sqlite3_bind_value(pStmt, i + 1, argv[i]);
    if (rc != SQLITE_OK) {
      sqlite3_finalize(pStmt);
      return rc;
    }
  }

  cursor->pChangesStmt = pStmt;
  return changes_next_impl(cursor, (sqlite3_vtab *)tab);
}

// ---------- xNext ----------

static int changes_next_impl(crsql_Changes_cursor *cursor,
                              sqlite3_vtab *vtab) {
  if (cursor->pChangesStmt == 0) {
    vtab->zErrMsg =
        sqlite3_mprintf("pChangesStmt is null in changes_next");
    return SQLITE_ABORT;
  }

  if (cursor->pRowStmt != 0) {
    int rc2 = resetCachedStmt(cursor->pRowStmt);
    cursor->pRowStmt = 0;
    if (rc2 != SQLITE_OK) return rc2;
  }

  int rc = sqlite3_step(cursor->pChangesStmt);
  if (rc == SQLITE_DONE) {
    int frc = changesCrsrFinalize(cursor);
    return (frc == SQLITE_OK) ? SQLITE_OK : SQLITE_ERROR;
  }
  if (rc != SQLITE_ROW) {
    changesCrsrFinalize(cursor);
    return rc;
  }

  // We have a row
  const char *tbl =
      (const char *)sqlite3_column_text(cursor->pChangesStmt, CLOCK_COL_TBL);
  sqlite3_value *pks =
      sqlite3_column_value(cursor->pChangesStmt, CLOCK_COL_PKS);
  const char *cid =
      (const char *)sqlite3_column_text(cursor->pChangesStmt, CLOCK_COL_CID);
  sqlite3_int64 dbVersion =
      sqlite3_column_int64(cursor->pChangesStmt, CLOCK_COL_DB_VRSN);
  sqlite3_int64 changesRowid =
      sqlite3_column_int64(cursor->pChangesStmt, CLOCK_COL_ROWID);

  cursor->dbVersion = dbVersion;
  cursor->changesRowid = changesRowid;

  // Find the table info
  crsql_TableInfoVec *tblInfos =
      (crsql_TableInfoVec *)cursor->pTab->pExtData->tableInfos;

  int tblInfoIdx = crsql_find_table_info(tblInfos, tbl);
  if (tblInfoIdx < 0) {
    vtab->zErrMsg =
        sqlite3_mprintf("could not find schema for table %s", tbl);
    return SQLITE_ERROR;
  }

  crsql_TableInfo *tblInfo = &tblInfos->aInfos[tblInfoIdx];
  cursor->tblInfoIdx = tblInfoIdx;

  if (tblInfo->pksLen == 0) {
    vtab->zErrMsg =
        sqlite3_mprintf("crr %s is missing primary keys", tbl);
    return SQLITE_ERROR;
  }

  if (!cid) {
    vtab->zErrMsg = sqlite3_mprintf("out of memory reading cid");
    return SQLITE_NOMEM;
  }

  if (strcmp(cid, SENTINEL_CID) == 0) {
    // Sentinel row: use CL parity to distinguish delete vs pk-only insert.
    // Even CL = deleted, odd CL = alive (pk-only insert).
    sqlite3_int64 cl =
        sqlite3_column_int64(cursor->pChangesStmt, CLOCK_COL_CL);
    cursor->rowType = (cl % 2 == 0) ? ROW_TYPE_DELETE : ROW_TYPE_PKONLY;
    return SQLITE_OK;
  } else {
    cursor->rowType = ROW_TYPE_UPDATE;
  }

  // Get row patch data stmt for this column
  sqlite3_stmt *pRowStmt = 0;
  rc = crsql_get_row_patch_data_stmt(cursor->pTab->db, tblInfo, cid,
                                      &pRowStmt);
  if (rc != SQLITE_OK || pRowStmt == 0) {
    return SQLITE_ERROR;
  }

  // Unpack primary keys and bind to the row stmt
  const unsigned char *pkBlob =
      (const unsigned char *)sqlite3_value_blob(pks);
  int pkBlobLen = sqlite3_value_bytes(pks);
  int numUnpackedPks = 0;
  crsql_ColumnValue *unpackedPks =
      crsql_unpack_columns(pkBlob, pkBlobLen, &numUnpackedPks);
  if (!unpackedPks) {
    resetCachedStmt(pRowStmt);
    return SQLITE_ERROR;
  }

  rc = crsql_bind_package_to_stmt(pRowStmt, unpackedPks, numUnpackedPks, 0);
  crsql_free_column_values(unpackedPks, numUnpackedPks);
  if (rc != SQLITE_OK) {
    resetCachedStmt(pRowStmt);
    return rc;
  }

  rc = sqlite3_step(pRowStmt);
  if (rc == SQLITE_DONE) {
    // No row found; reset the stmt, pRowStmt stays null so cval is null
    resetCachedStmt(pRowStmt);
    pRowStmt = 0;
  } else if (rc != SQLITE_ROW) {
    resetCachedStmt(pRowStmt);
    return rc;
  }

  cursor->pRowStmt = pRowStmt;
  return SQLITE_OK;
}

int crsql_changes_next(sqlite3_vtab_cursor *cur) {
  crsql_Changes_cursor *cursor = (crsql_Changes_cursor *)cur;
  sqlite3_vtab *vtab = (sqlite3_vtab *)cursor->pTab;
  int rc = changes_next_impl(cursor, vtab);
  if (rc != SQLITE_OK) {
    changesCrsrFinalize(cursor);
  }
  return rc;
}

// ---------- xEof ----------

int crsql_changes_eof(sqlite3_vtab_cursor *cur) {
  crsql_Changes_cursor *cursor = (crsql_Changes_cursor *)cur;
  return (cursor->pChangesStmt == 0) ? 1 : 0;
}

// ---------- xColumn ----------

int crsql_changes_column(sqlite3_vtab_cursor *cur, sqlite3_context *ctx,
                          int i) {
  crsql_Changes_cursor *cursor = (crsql_Changes_cursor *)cur;
  sqlite3_stmt *changesStmt = cursor->pChangesStmt;

  switch (i) {
    case CHANGES_COL_TBL:
      sqlite3_result_value(ctx,
                           sqlite3_column_value(changesStmt, CLOCK_COL_TBL));
      break;
    case CHANGES_COL_PK:
      sqlite3_result_value(ctx,
                           sqlite3_column_value(changesStmt, CLOCK_COL_PKS));
      break;
    case CHANGES_COL_CVAL:
      if (cursor->pRowStmt == 0) {
        sqlite3_result_null(ctx);
      } else {
        sqlite3_result_value(ctx, sqlite3_column_value(cursor->pRowStmt, 0));
      }
      break;
    case CHANGES_COL_CID:
      switch (cursor->rowType) {
        case ROW_TYPE_PKONLY:
          sqlite3_result_text(ctx, SENTINEL_CID, -1, SQLITE_STATIC);
          break;
        case ROW_TYPE_DELETE:
          sqlite3_result_text(ctx, SENTINEL_CID, -1, SQLITE_STATIC);
          break;
        case ROW_TYPE_UPDATE:
          if (cursor->pRowStmt == 0) {
            // Row data is missing -- report as delete
            sqlite3_result_text(ctx, SENTINEL_CID, -1, SQLITE_STATIC);
          } else {
            sqlite3_result_value(
                ctx, sqlite3_column_value(changesStmt, CLOCK_COL_CID));
          }
          break;
        default:
          return SQLITE_ABORT;
      }
      break;
    case CHANGES_COL_COL_VRSN:
      sqlite3_result_value(
          ctx, sqlite3_column_value(changesStmt, CLOCK_COL_COL_VRSN));
      break;
    case CHANGES_COL_DB_VRSN:
      sqlite3_result_value(
          ctx, sqlite3_column_value(changesStmt, CLOCK_COL_DB_VRSN));
      break;
    case CHANGES_COL_SITE_ID:
      sqlite3_result_value(
          ctx, sqlite3_column_value(changesStmt, CLOCK_COL_SITE_ID));
      break;
    case CHANGES_COL_SEQ:
      sqlite3_result_value(ctx,
                           sqlite3_column_value(changesStmt, CLOCK_COL_SEQ));
      break;
    case CHANGES_COL_CL:
      sqlite3_result_value(ctx,
                           sqlite3_column_value(changesStmt, CLOCK_COL_CL));
      break;
    default:
      return SQLITE_MISUSE;
  }

  return SQLITE_OK;
}

// ---------- xRowid ----------

int crsql_changes_rowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid) {
  crsql_Changes_cursor *cursor = (crsql_Changes_cursor *)cur;
  *pRowid = crsql_slab_rowid(cursor->tblInfoIdx, cursor->changesRowid);
  if (*pRowid < 0) {
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}

// ---------- Merge helpers ----------

/**
 * Get the local causal length for a key.
 * Returns 0 if no record exists.
 */
static int get_local_cl(sqlite3 *db, crsql_TableInfo *tblInfo,
                         sqlite3_int64 key, sqlite3_int64 *outCl) {
  sqlite3_stmt *pStmt = 0;
  int rc = crsql_get_local_cl_stmt(db, tblInfo, &pStmt);
  if (rc != SQLITE_OK || !pStmt) return SQLITE_ERROR;

  rc = sqlite3_bind_int64(pStmt, 1, key);
  if (rc != SQLITE_OK) {
    resetCachedStmt(pStmt);
    return rc;
  }
  rc = sqlite3_bind_int64(pStmt, 2, key);
  if (rc != SQLITE_OK) {
    resetCachedStmt(pStmt);
    return rc;
  }

  rc = sqlite3_step(pStmt);
  if (rc == SQLITE_ROW) {
    *outCl = sqlite3_column_int64(pStmt, 0);
    resetCachedStmt(pStmt);
    return SQLITE_OK;
  } else if (rc == SQLITE_DONE) {
    *outCl = 0;
    resetCachedStmt(pStmt);
    return SQLITE_OK;
  } else {
    resetCachedStmt(pStmt);
    return rc;
  }
}

/**
 * did_cid_win - determine if the incoming change beats the local value.
 *
 * Causal length concerns have already been handled:
 * - early return if insert_cl < local_cl
 * - automatic win if insert_cl > local_cl
 * - come here only if insert_cl == local_cl
 */
static int did_cid_win(sqlite3 *db, crsql_ExtData *extData,
                        const char *insertTbl, crsql_TableInfo *tblInfo,
                        crsql_ColumnValue *unpackedPks, int numPks,
                        sqlite3_int64 key, sqlite3_value *insertVal,
                        const unsigned char *insertSiteId, int insertSiteIdLen,
                        const char *colName, sqlite3_int64 colVersion,
                        char **errmsg, int *outWon) {
  // Check local column version
  sqlite3_stmt *colVrsnStmt = 0;
  int rc = crsql_get_col_version_stmt(db, tblInfo, &colVrsnStmt);
  if (rc != SQLITE_OK || !colVrsnStmt) return SQLITE_ERROR;

  rc = sqlite3_bind_int64(colVrsnStmt, 1, key);
  if (rc != SQLITE_OK) {
    resetCachedStmt(colVrsnStmt);
    return rc;
  }
  rc = sqlite3_bind_text(colVrsnStmt, 2, colName, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    resetCachedStmt(colVrsnStmt);
    return rc;
  }

  rc = sqlite3_step(colVrsnStmt);
  if (rc == SQLITE_ROW) {
    sqlite3_int64 localVersion = sqlite3_column_int64(colVrsnStmt, 0);
    resetCachedStmt(colVrsnStmt);
    // causal lengths are the same. Fall back to original algorithm.
    if (colVersion > localVersion) {
      *outWon = 1;
      return SQLITE_OK;
    } else if (colVersion < localVersion) {
      *outWon = 0;
      return SQLITE_OK;
    }
    // versions equal, fall through to value comparison
  } else if (rc == SQLITE_DONE) {
    resetCachedStmt(colVrsnStmt);
    // no rows -- incoming wins
    *outWon = 1;
    return SQLITE_OK;
  } else {
    resetCachedStmt(colVrsnStmt);
    *errmsg = sqlite3_mprintf(
        "Bad return code when selecting local column version");
    return rc;
  }

  // Versions are equal -- need to compare values
  sqlite3_stmt *colValStmt = 0;
  rc = crsql_get_col_value_stmt(db, tblInfo, colName, &colValStmt);
  if (rc != SQLITE_OK || !colValStmt) return SQLITE_ERROR;

  rc = crsql_bind_package_to_stmt(colValStmt, unpackedPks, numPks, 0);
  if (rc != SQLITE_OK) {
    resetCachedStmt(colValStmt);
    return rc;
  }

  rc = sqlite3_step(colValStmt);
  if (rc == SQLITE_ROW) {
    sqlite3_value *localValue = sqlite3_column_value(colValStmt, 0);
    int cmp = crsql_compare_sqlite_values(insertVal, localValue);
    resetCachedStmt(colValStmt);

    if (cmp == 0 && extData->mergeEqualValues == 1) {
      // Values are the same and we should tie-break on site_id
      sqlite3_stmt *colSiteIdStmt = 0;
      rc = crsql_get_col_site_id_stmt(db, tblInfo, &colSiteIdStmt);
      if (rc != SQLITE_OK || !colSiteIdStmt) return SQLITE_ERROR;

      rc = sqlite3_bind_int64(colSiteIdStmt, 1, key);
      if (rc != SQLITE_OK) {
        resetCachedStmt(colSiteIdStmt);
        return rc;
      }
      rc = sqlite3_bind_text(colSiteIdStmt, 2, colName, -1, SQLITE_STATIC);
      if (rc != SQLITE_OK) {
        resetCachedStmt(colSiteIdStmt);
        return rc;
      }

      rc = sqlite3_step(colSiteIdStmt);
      if (rc == SQLITE_ROW) {
        const unsigned char *localSiteId =
            (const unsigned char *)sqlite3_column_blob(colSiteIdStmt, 0);
        int localSiteIdLen = sqlite3_column_bytes(colSiteIdStmt, 0);
        // Compare site_ids (memcmp-style)
        int minLen = insertSiteIdLen < localSiteIdLen ? insertSiteIdLen
                                                       : localSiteIdLen;
        cmp = memcmp(insertSiteId, localSiteId, minLen);
        if (cmp == 0) {
          if (insertSiteIdLen < localSiteIdLen)
            cmp = -1;
          else if (insertSiteIdLen > localSiteIdLen)
            cmp = 1;
        }
        resetCachedStmt(colSiteIdStmt);
      } else if (rc == SQLITE_DONE) {
        resetCachedStmt(colSiteIdStmt);
        *errmsg = sqlite3_mprintf(
            "could not find site_id for previous change, cr-sqlite clock "
            "table might be corrupt for tbl %s",
            insertTbl);
        return SQLITE_ERROR;
      } else {
        resetCachedStmt(colSiteIdStmt);
        *errmsg = sqlite3_mprintf(
            "Bad return code when selecting local column site_id");
        return rc;
      }
    }

    *outWon = (cmp > 0) ? 1 : 0;
    return SQLITE_OK;
  } else {
    // DONE or error -- could not find row to merge with
    resetCachedStmt(colValStmt);
    *errmsg = sqlite3_mprintf(
        "could not find row to merge with for tbl %s", insertTbl);
    return SQLITE_ERROR;
  }
}

/**
 * Resolve the site_id ordinal and set the winner clock entry.
 */
static int set_winner_clock(sqlite3 *db, crsql_ExtData *extData,
                             crsql_TableInfo *tblInfo, sqlite3_int64 key,
                             const char *insertColName,
                             sqlite3_int64 insertColVrsn,
                             sqlite3_int64 insertDbVrsn,
                             const unsigned char *insertSiteId,
                             int insertSiteIdLen, sqlite3_int64 insertSeq,
                             sqlite3_int64 *outRowid) {
  int rc;
  int hasOrdinal = 0;
  sqlite3_int64 ordinal = 0;

  if (insertSiteIdLen > 0 && insertSiteId != 0) {
    // Try to select existing ordinal
    rc = sqlite3_bind_blob(extData->pSelectSiteIdOrdinalStmt, 1, insertSiteId,
                           insertSiteIdLen, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
      sqlite3_clear_bindings(extData->pSelectSiteIdOrdinalStmt);
      sqlite3_reset(extData->pSelectSiteIdOrdinalStmt);
      return rc;
    }
    rc = sqlite3_step(extData->pSelectSiteIdOrdinalStmt);
    if (rc == SQLITE_ROW) {
      ordinal = sqlite3_column_int64(extData->pSelectSiteIdOrdinalStmt, 0);
      hasOrdinal = 1;
      sqlite3_clear_bindings(extData->pSelectSiteIdOrdinalStmt);
      sqlite3_reset(extData->pSelectSiteIdOrdinalStmt);
    } else {
      sqlite3_clear_bindings(extData->pSelectSiteIdOrdinalStmt);
      sqlite3_reset(extData->pSelectSiteIdOrdinalStmt);

      // Insert new ordinal
      rc = sqlite3_bind_blob(extData->pSetSiteIdOrdinalStmt, 1, insertSiteId,
                             insertSiteIdLen, SQLITE_STATIC);
      if (rc != SQLITE_OK) {
        sqlite3_clear_bindings(extData->pSetSiteIdOrdinalStmt);
        sqlite3_reset(extData->pSetSiteIdOrdinalStmt);
        return rc;
      }
      rc = sqlite3_step(extData->pSetSiteIdOrdinalStmt);
      if (rc == SQLITE_DONE) {
        sqlite3_clear_bindings(extData->pSetSiteIdOrdinalStmt);
        sqlite3_reset(extData->pSetSiteIdOrdinalStmt);
        return SQLITE_ABORT;
      }
      // Should be SQLITE_ROW with the returning ordinal
      ordinal = sqlite3_column_int64(extData->pSetSiteIdOrdinalStmt, 0);
      hasOrdinal = 1;
      sqlite3_clear_bindings(extData->pSetSiteIdOrdinalStmt);
      sqlite3_reset(extData->pSetSiteIdOrdinalStmt);
    }
  }

  sqlite3_stmt *setStmt = 0;
  rc = crsql_get_set_winner_clock_stmt(db, tblInfo, &setStmt);
  if (rc != SQLITE_OK || !setStmt) return SQLITE_ERROR;

  rc = sqlite3_bind_int64(setStmt, 1, key);
  if (rc != SQLITE_OK) {
    resetCachedStmt(setStmt);
    return rc;
  }
  rc = sqlite3_bind_text(setStmt, 2, insertColName, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    resetCachedStmt(setStmt);
    return rc;
  }
  rc = sqlite3_bind_int64(setStmt, 3, insertColVrsn);
  if (rc != SQLITE_OK) {
    resetCachedStmt(setStmt);
    return rc;
  }
  rc = sqlite3_bind_int64(setStmt, 4, insertDbVrsn);
  if (rc != SQLITE_OK) {
    resetCachedStmt(setStmt);
    return rc;
  }
  rc = sqlite3_bind_int64(setStmt, 5, insertSeq);
  if (rc != SQLITE_OK) {
    resetCachedStmt(setStmt);
    return rc;
  }
  if (hasOrdinal) {
    rc = sqlite3_bind_int64(setStmt, 6, ordinal);
  } else {
    rc = sqlite3_bind_null(setStmt, 6);
  }
  if (rc != SQLITE_OK) {
    resetCachedStmt(setStmt);
    return rc;
  }

  rc = sqlite3_step(setStmt);
  if (rc == SQLITE_ROW) {
    *outRowid = sqlite3_column_int64(setStmt, 0);
    resetCachedStmt(setStmt);
    return SQLITE_OK;
  } else {
    resetCachedStmt(setStmt);
    return SQLITE_ERROR;
  }
}

/**
 * Reset clocks when a row is resurrected (causal length increased).
 */
static int zero_clocks_on_resurrect(sqlite3 *db, crsql_TableInfo *tblInfo,
                                     sqlite3_int64 key,
                                     sqlite3_int64 insertDbVrsn) {
  sqlite3_stmt *zeroStmt = 0;
  int rc =
      crsql_get_zero_clocks_on_resurrect_stmt(db, tblInfo, &zeroStmt);
  if (rc != SQLITE_OK || !zeroStmt) return SQLITE_ERROR;

  rc = sqlite3_bind_int64(zeroStmt, 1, insertDbVrsn);
  if (rc != SQLITE_OK) {
    resetCachedStmt(zeroStmt);
    return rc;
  }
  rc = sqlite3_bind_int64(zeroStmt, 2, key);
  if (rc != SQLITE_OK) {
    resetCachedStmt(zeroStmt);
    return rc;
  }
  rc = sqlite3_step(zeroStmt);
  resetCachedStmt(zeroStmt);
  // step returns DONE on success for UPDATE/INSERT
  if (rc == SQLITE_DONE) return SQLITE_OK;
  return rc;
}

/**
 * Handle insertion of a row that only has a sentinel (PK-only insert).
 */
static int merge_sentinel_only_insert(
    sqlite3 *db, crsql_ExtData *extData, crsql_TableInfo *tblInfo,
    crsql_ColumnValue *unpackedPks, int numPks, sqlite3_int64 key,
    sqlite3_int64 remoteColVrsn, sqlite3_int64 remoteDbVsn,
    const unsigned char *remoteSiteId, int remoteSiteIdLen,
    sqlite3_int64 remoteSeq, sqlite3_int64 *outRowid) {
  sqlite3_stmt *mergeStmt = 0;
  int rc = crsql_get_merge_pk_only_insert_stmt(db, tblInfo, &mergeStmt);
  if (rc != SQLITE_OK || !mergeStmt) return SQLITE_ERROR;

  rc = crsql_bind_package_to_stmt(mergeStmt, unpackedPks, numPks, 0);
  if (rc != SQLITE_OK) {
    resetCachedStmt(mergeStmt);
    return rc;
  }

  // Set sync bit, execute merge, clear sync bit
  rc = sqlite3_step(extData->pSetSyncBitStmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    sqlite3_reset(extData->pSetSyncBitStmt);
    resetCachedStmt(mergeStmt);
    return rc;
  }
  sqlite3_reset(extData->pSetSyncBitStmt);

  rc = sqlite3_step(mergeStmt);
  int mergeRc = rc;
  resetCachedStmt(mergeStmt);

  int syncRc = sqlite3_step(extData->pClearSyncBitStmt);
  sqlite3_reset(extData->pClearSyncBitStmt);
  if (syncRc != SQLITE_ROW && syncRc != SQLITE_DONE) {
    return syncRc;
  }

  if (mergeRc != SQLITE_DONE && mergeRc != SQLITE_ROW) {
    return mergeRc;
  }

  // Success: zero clocks on resurrect, then set winner clock
  rc = zero_clocks_on_resurrect(db, tblInfo, key, remoteDbVsn);
  if (rc != SQLITE_OK) return rc;

  return set_winner_clock(db, extData, tblInfo, key, SENTINEL_CID,
                          remoteColVrsn, remoteDbVsn, remoteSiteId,
                          remoteSiteIdLen, remoteSeq, outRowid);
}

/**
 * Apply a delete merge.
 */
static int merge_delete(sqlite3 *db, crsql_ExtData *extData,
                         crsql_TableInfo *tblInfo,
                         crsql_ColumnValue *unpackedPks, int numPks,
                         sqlite3_int64 key, sqlite3_int64 remoteColVrsn,
                         sqlite3_int64 remoteDbVrsn,
                         const unsigned char *remoteSiteId,
                         int remoteSiteIdLen, sqlite3_int64 remoteSeq,
                         sqlite3_int64 *outRowid) {
  sqlite3_stmt *deleteStmt = 0;
  int rc = crsql_get_merge_delete_stmt(db, tblInfo, &deleteStmt);
  if (rc != SQLITE_OK || !deleteStmt) return SQLITE_ERROR;

  rc = crsql_bind_package_to_stmt(deleteStmt, unpackedPks, numPks, 0);
  if (rc != SQLITE_OK) {
    resetCachedStmt(deleteStmt);
    return rc;
  }

  // Set sync bit
  rc = sqlite3_step(extData->pSetSyncBitStmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    sqlite3_reset(extData->pSetSyncBitStmt);
    resetCachedStmt(deleteStmt);
    return rc;
  }
  sqlite3_reset(extData->pSetSyncBitStmt);

  rc = sqlite3_step(deleteStmt);
  resetCachedStmt(deleteStmt);

  // Clear sync bit
  int syncRc = sqlite3_step(extData->pClearSyncBitStmt);
  sqlite3_reset(extData->pClearSyncBitStmt);
  if (syncRc != SQLITE_ROW && syncRc != SQLITE_DONE) {
    return syncRc;
  }

  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    return rc;
  }

  // Set winner clock for the delete sentinel
  rc = set_winner_clock(db, extData, tblInfo, key, SENTINEL_CID,
                        remoteColVrsn, remoteDbVrsn, remoteSiteId,
                        remoteSiteIdLen, remoteSeq, outRowid);
  if (rc != SQLITE_OK) return rc;

  // Drop clocks AFTER setting the winner clock so we don't lose track
  // of max db_version. This must never come before set_winner_clock.
  sqlite3_stmt *dropClocksStmt = 0;
  rc = crsql_get_merge_delete_drop_clocks_stmt(db, tblInfo, &dropClocksStmt);
  if (rc != SQLITE_OK || !dropClocksStmt) return SQLITE_ERROR;

  rc = sqlite3_bind_int64(dropClocksStmt, 1, key);
  if (rc != SQLITE_OK) {
    resetCachedStmt(dropClocksStmt);
    return rc;
  }
  rc = sqlite3_step(dropClocksStmt);
  resetCachedStmt(dropClocksStmt);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    return rc;
  }

  return SQLITE_OK;
}

// ---------- xUpdate (merge_insert) ----------

static int merge_insert_impl(sqlite3_vtab *vtab, int argc,
                               sqlite3_value **argv, sqlite3_int64 *rowid,
                               char **errmsg);

// Best-effort auto-update of crsql_tracked_peers for the RECEIVED watermark.
// Skips when the peer site_id is missing or matches the local site_id (so
// loop-back inserts produced by local triggers do not pollute the table).
// Failures are intentionally swallowed: tracking is an optimization for the
// puller and must not fail an otherwise successful merge. The cached
// statement uses a monotonic upsert (UPSERT_TRACKED_PEER) so concurrent
// out-of-order arrivals never roll the watermark backwards.
static void track_peer_recv(crsql_ExtData *pExtData,
                             const unsigned char *siteId, int siteIdLen,
                             sqlite3_int64 version, sqlite3_int64 seq) {
  if (!siteId || siteIdLen != SITE_ID_LEN) return;
  if (memcmp(siteId, pExtData->siteId, SITE_ID_LEN) == 0) return;

  sqlite3_stmt *s = pExtData->pUpsertTrackedPeerStmt;
  if (!s) return;
  sqlite3_reset(s);
  if (sqlite3_bind_blob(s, 1, siteId, SITE_ID_LEN, SQLITE_TRANSIENT) !=
          SQLITE_OK ||
      sqlite3_bind_int64(s, 2, version) != SQLITE_OK ||
      sqlite3_bind_int64(s, 3, seq) != SQLITE_OK ||
      sqlite3_bind_int(s, 4, 0) != SQLITE_OK ||
      sqlite3_bind_int(s, 5, TRACKED_EVENT_RECEIVED) != SQLITE_OK) {
    sqlite3_reset(s);
    return;
  }
  (void)sqlite3_step(s);
  sqlite3_reset(s);
}

int crsql_changes_update(sqlite3_vtab *pVTab, int argc, sqlite3_value **argv,
                          sqlite3_int64 *pRowid) {
  if (argc > 1 && sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    // INSERT statement
    char *errMsg = 0;
    int rc = merge_insert_impl(pVTab, argc, argv, pRowid, &errMsg);
    if (rc != SQLITE_OK) {
      pVTab->zErrMsg = errMsg;
    }
    return rc;
  } else {
    pVTab->zErrMsg = sqlite3_mprintf(
        "Only INSERT and SELECT statements are allowed against the "
        "crsql changes table");
    return SQLITE_MISUSE;
  }
}

static int merge_insert_impl(sqlite3_vtab *vtab, int argc,
                               sqlite3_value **argv, sqlite3_int64 *rowid,
                               char **errmsg) {
  crsql_Changes_vtab *tab = (crsql_Changes_vtab *)vtab;
  sqlite3 *db = tab->db;

  int rc = crsql_ensure_table_infos_are_up_to_date(db, tab->pExtData, errmsg);
  if (rc != SQLITE_OK) {
    *errmsg = sqlite3_mprintf("Failed to update CRR table information");
    return SQLITE_ERROR;
  }

  // argv layout: [0]=old rowid (NULL for INSERT), [1]=new rowid, [2+col]=value
  sqlite3_value *insertTblVal = argv[2 + CHANGES_COL_TBL];
  if (sqlite3_value_bytes(insertTblVal) > MAX_TBL_NAME_LEN) {
    *errmsg = sqlite3_mprintf("crsql - table name exceeded max length");
    return SQLITE_ERROR;
  }
  const char *insertTbl = (const char *)sqlite3_value_text(insertTblVal);

  sqlite3_value *insertPksVal = argv[2 + CHANGES_COL_PK];
  sqlite3_value *insertColVal = argv[2 + CHANGES_COL_CID];
  if (sqlite3_value_bytes(insertColVal) > MAX_TBL_NAME_LEN) {
    *errmsg = sqlite3_mprintf("crsql - column name exceeded max length");
    return SQLITE_ERROR;
  }
  const char *insertCol = (const char *)sqlite3_value_text(insertColVal);
  if (!insertCol) {
    *errmsg = sqlite3_mprintf("crsql - cid column must not be null");
    return SQLITE_ERROR;
  }

  sqlite3_value *insertVal = argv[2 + CHANGES_COL_CVAL];
  sqlite3_int64 insertColVrsn =
      sqlite3_value_int64(argv[2 + CHANGES_COL_COL_VRSN]);
  sqlite3_int64 insertDbVrsn =
      sqlite3_value_int64(argv[2 + CHANGES_COL_DB_VRSN]);
  sqlite3_value *insertSiteIdVal = argv[2 + CHANGES_COL_SITE_ID];
  sqlite3_int64 insertCl = sqlite3_value_int64(argv[2 + CHANGES_COL_CL]);
  sqlite3_int64 insertSeq = sqlite3_value_int64(argv[2 + CHANGES_COL_SEQ]);

  if (sqlite3_value_bytes(insertSiteIdVal) > SITE_ID_LEN) {
    *errmsg = sqlite3_mprintf("crsql - site id exceeded max length");
    return SQLITE_ERROR;
  }

  const unsigned char *insertSiteId =
      (const unsigned char *)sqlite3_value_blob(insertSiteIdVal);
  int insertSiteIdLen = sqlite3_value_bytes(insertSiteIdVal);

  // Auto-record the RECEIVED watermark for this peer. Done eagerly here so
  // that no-op merges (older CL, losing cid, idempotent re-applies) still
  // advance the puller's watermark — otherwise we'd keep refetching the
  // same losing changes. Best-effort; rolls back with the surrounding txn
  // if the merge ultimately fails.
  track_peer_recv(tab->pExtData, insertSiteId, insertSiteIdLen, insertDbVrsn,
                  insertSeq);

  // Find table info
  crsql_TableInfoVec *tblInfos =
      (crsql_TableInfoVec *)tab->pExtData->tableInfos;
  int tblInfoIdx = crsql_find_table_info(tblInfos, insertTbl);
  if (tblInfoIdx < 0) {
    *errmsg = sqlite3_mprintf(
        "crsql - could not find the schema information for table %s",
        insertTbl);
    return SQLITE_ERROR;
  }

  crsql_TableInfo *tblInfo = &tblInfos->aInfos[tblInfoIdx];

  // Unpack primary keys
  const unsigned char *pkBlob =
      (const unsigned char *)sqlite3_value_blob(insertPksVal);
  int pkBlobLen = sqlite3_value_bytes(insertPksVal);
  int numUnpackedPks = 0;
  crsql_ColumnValue *unpackedPks =
      crsql_unpack_columns(pkBlob, pkBlobLen, &numUnpackedPks);
  if (!unpackedPks) {
    *errmsg = sqlite3_mprintf("crsql - failed to unpack primary keys");
    return SQLITE_ERROR;
  }

  // Convert unpacked PKs to sqlite3_value** for crsql_get_or_create_key.
  // We prepare a temporary "SELECT ?,?,?..." statement, bind the unpacked
  // column values, step it, then extract sqlite3_column_value pointers.
  sqlite3_int64 key = -1;
  {
    char *bindList = crsql_binding_list(numUnpackedPks);
    if (!bindList) {
      crsql_free_column_values(unpackedPks, numUnpackedPks);
      return SQLITE_NOMEM;
    }
    char *tmpSql = sqlite3_mprintf("SELECT %s", bindList);
    sqlite3_free(bindList);
    if (!tmpSql) {
      crsql_free_column_values(unpackedPks, numUnpackedPks);
      return SQLITE_NOMEM;
    }
    sqlite3_stmt *pTmpStmt = 0;
    rc = sqlite3_prepare_v2(db, tmpSql, -1, &pTmpStmt, 0);
    sqlite3_free(tmpSql);
    if (rc != SQLITE_OK) {
      crsql_free_column_values(unpackedPks, numUnpackedPks);
      return rc;
    }
    rc = crsql_bind_package_to_stmt(pTmpStmt, unpackedPks, numUnpackedPks, 0);
    if (rc != SQLITE_OK) {
      sqlite3_finalize(pTmpStmt);
      crsql_free_column_values(unpackedPks, numUnpackedPks);
      return rc;
    }
    rc = sqlite3_step(pTmpStmt);
    if (rc != SQLITE_ROW) {
      sqlite3_finalize(pTmpStmt);
      crsql_free_column_values(unpackedPks, numUnpackedPks);
      return SQLITE_ERROR;
    }
    // Extract column values as sqlite3_value*
    sqlite3_value **pkValues =
        (sqlite3_value **)sqlite3_malloc(sizeof(sqlite3_value *) * numUnpackedPks);
    if (!pkValues) {
      sqlite3_finalize(pTmpStmt);
      crsql_free_column_values(unpackedPks, numUnpackedPks);
      return SQLITE_NOMEM;
    }
    for (int i = 0; i < numUnpackedPks; i++) {
      pkValues[i] = sqlite3_column_value(pTmpStmt, i);
    }

    key = crsql_get_or_create_key(db, tblInfo, pkValues, numUnpackedPks, errmsg);
    sqlite3_free(pkValues);
    sqlite3_finalize(pTmpStmt);
  }

  if (key < 0) {
    crsql_free_column_values(unpackedPks, numUnpackedPks);
    if (!*errmsg) {
      *errmsg = sqlite3_mprintf("crsql - failed to get or create key");
    }
    return SQLITE_ERROR;
  }

  sqlite3_int64 localCl = 0;
  rc = get_local_cl(db, tblInfo, key, &localCl);
  if (rc != SQLITE_OK) {
    crsql_free_column_values(unpackedPks, numUnpackedPks);
    return rc;
  }

  // We can ignore all updates from older causal lengths.
  if (insertCl < localCl) {
    crsql_free_column_values(unpackedPks, numUnpackedPks);
    return SQLITE_OK;
  }

  int isDelete = (insertCl % 2 == 0);
  int needsResurrect = (insertCl > localCl && insertCl % 2 == 1);
  int rowExistsLocally = (localCl != 0);
  int isSentinelOnly = (strcmp(insertCol, SENTINEL_CID) == 0);

  // Handle delete
  if (isDelete) {
    if (insertCl == localCl) {
      // Already processed a delete at this version
      crsql_free_column_values(unpackedPks, numUnpackedPks);
      return SQLITE_OK;
    }
    sqlite3_int64 innerRowid = 0;
    rc = merge_delete(db, tab->pExtData, tblInfo, unpackedPks, numUnpackedPks,
                      key, insertColVrsn, insertDbVrsn, insertSiteId,
                      insertSiteIdLen, insertSeq, &innerRowid);
    crsql_free_column_values(unpackedPks, numUnpackedPks);
    if (rc != SQLITE_OK) return rc;
    tab->pExtData->rowsImpacted += 1;
    *rowid = crsql_slab_rowid(tblInfoIdx, innerRowid);
    return SQLITE_OK;
  }

  // Handle sentinel-only insert
  if (isSentinelOnly) {
    if (insertCl == localCl) {
      crsql_free_column_values(unpackedPks, numUnpackedPks);
      return SQLITE_OK;
    }
    sqlite3_int64 innerRowid = 0;
    rc = merge_sentinel_only_insert(db, tab->pExtData, tblInfo, unpackedPks,
                                     numUnpackedPks, key, insertColVrsn,
                                     insertDbVrsn, insertSiteId,
                                     insertSiteIdLen, insertSeq, &innerRowid);
    crsql_free_column_values(unpackedPks, numUnpackedPks);
    if (rc != SQLITE_OK) return rc;
    if (innerRowid != -1) {
      tab->pExtData->rowsImpacted += 1;
      *rowid = crsql_slab_rowid(tblInfoIdx, innerRowid);
    }
    return SQLITE_OK;
  }

  // We got a causal length which would resurrect the row.
  // In an in-order delivery situation then sentinel_only would have already
  // resurrected the row. In out-of-order delivery, we need to resurrect ASAP.
  if (needsResurrect && (rowExistsLocally || (!rowExistsLocally && insertCl > 1))) {
    sqlite3_int64 sentinelRowid = 0;
    rc = merge_sentinel_only_insert(db, tab->pExtData, tblInfo, unpackedPks,
                                     numUnpackedPks, key, insertCl,
                                     insertDbVrsn, insertSiteId,
                                     insertSiteIdLen, insertSeq,
                                     &sentinelRowid);
    if (rc != SQLITE_OK) {
      crsql_free_column_values(unpackedPks, numUnpackedPks);
      return rc;
    }
    tab->pExtData->rowsImpacted += 1;
  }

  // Determine if this cid wins
  int doesCidWin = 0;
  if (needsResurrect || !rowExistsLocally) {
    doesCidWin = 1;
  } else {
    rc = did_cid_win(db, tab->pExtData, insertTbl, tblInfo, unpackedPks,
                     numUnpackedPks, key, insertVal, insertSiteId,
                     insertSiteIdLen, insertCol, insertColVrsn, errmsg,
                     &doesCidWin);
    if (rc != SQLITE_OK) {
      crsql_free_column_values(unpackedPks, numUnpackedPks);
      return rc;
    }
  }

  if (!doesCidWin) {
    crsql_free_column_values(unpackedPks, numUnpackedPks);
    return SQLITE_OK;
  }

  // Apply the winning change
  sqlite3_stmt *mergeStmt = 0;
  rc = crsql_get_merge_insert_col_stmt(db, tblInfo, insertCol, &mergeStmt);
  if (rc != SQLITE_OK || !mergeStmt) {
    crsql_free_column_values(unpackedPks, numUnpackedPks);
    return SQLITE_ERROR;
  }

  rc = crsql_bind_package_to_stmt(mergeStmt, unpackedPks, numUnpackedPks, 0);
  if (rc != SQLITE_OK) {
    resetCachedStmt(mergeStmt);
    crsql_free_column_values(unpackedPks, numUnpackedPks);
    return rc;
  }
  // Bind the insert value twice (INSERT OR REPLACE pattern: value + ON CONFLICT UPDATE)
  rc = sqlite3_bind_value(mergeStmt, numUnpackedPks + 1, insertVal);
  if (rc != SQLITE_OK) {
    resetCachedStmt(mergeStmt);
    crsql_free_column_values(unpackedPks, numUnpackedPks);
    return rc;
  }
  rc = sqlite3_bind_value(mergeStmt, numUnpackedPks + 2, insertVal);
  if (rc != SQLITE_OK) {
    resetCachedStmt(mergeStmt);
    crsql_free_column_values(unpackedPks, numUnpackedPks);
    return rc;
  }

  // Set sync bit, merge, clear sync bit
  rc = sqlite3_step(tab->pExtData->pSetSyncBitStmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    sqlite3_reset(tab->pExtData->pSetSyncBitStmt);
    resetCachedStmt(mergeStmt);
    crsql_free_column_values(unpackedPks, numUnpackedPks);
    return rc;
  }
  sqlite3_reset(tab->pExtData->pSetSyncBitStmt);

  rc = sqlite3_step(mergeStmt);
  resetCachedStmt(mergeStmt);

  int syncRc = sqlite3_step(tab->pExtData->pClearSyncBitStmt);
  sqlite3_reset(tab->pExtData->pClearSyncBitStmt);

  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    crsql_free_column_values(unpackedPks, numUnpackedPks);
    return rc;
  }
  if (syncRc != SQLITE_ROW && syncRc != SQLITE_DONE) {
    crsql_free_column_values(unpackedPks, numUnpackedPks);
    return syncRc;
  }

  crsql_free_column_values(unpackedPks, numUnpackedPks);

  sqlite3_int64 innerRowid = 0;
  rc = set_winner_clock(db, tab->pExtData, tblInfo, key, insertCol,
                        insertColVrsn, insertDbVrsn, insertSiteId,
                        insertSiteIdLen, insertSeq, &innerRowid);
  if (rc != SQLITE_OK) return rc;

  tab->pExtData->rowsImpacted += 1;
  *rowid = crsql_slab_rowid(tblInfoIdx, innerRowid);
  return SQLITE_OK;
}

// ---------- xBegin / xCommit ----------

int crsql_changes_begin(sqlite3_vtab *pVTab) {
  (void)pVTab;
  return SQLITE_OK;
}

int crsql_changes_commit(sqlite3_vtab *pVTab) {
  crsql_Changes_vtab *tab = (crsql_Changes_vtab *)pVTab;
  tab->pExtData->rowsImpacted = 0;
  return SQLITE_OK;
}
