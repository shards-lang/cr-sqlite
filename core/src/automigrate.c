#include "automigrate.h"

#include <string.h>

#include "consts.h"
#include "tableinfo.h"
#include "util.h"

// ---------- strip_crr_statements ----------
// Remove lines containing crsql_as_crr or crsql_fract_as_ordered
// Returns sqlite3_malloc'd string. Caller must sqlite3_free.
static char *strip_crr_statements(const char *schema) {
  int len = (int)strlen(schema);
  char *result = sqlite3_malloc(len + 1);
  if (!result) return 0;

  int outPos = 0;
  const char *lineStart = schema;
  while (*lineStart) {
    const char *lineEnd = lineStart;
    while (*lineEnd && *lineEnd != '\n') lineEnd++;

    int lineLen = (int)(lineEnd - lineStart);

    // Check if line contains crsql_as_crr or crsql_fract_as_ordered (case-insensitive)
    int skip = 0;
    for (int i = 0; i <= lineLen - 12; i++) {
      // Check "crsql_as_crr" (12 chars)
      if (lineLen - i >= 12) {
        int match = 1;
        const char *pat = "crsql_as_crr";
        for (int j = 0; j < 12; j++) {
          char c = lineStart[i + j];
          if (c >= 'A' && c <= 'Z') c += 32;
          if (c != pat[j]) { match = 0; break; }
        }
        if (match) { skip = 1; break; }
      }
      // Check "crsql_fract_as_ordered" (22 chars)
      if (lineLen - i >= 22) {
        int match = 1;
        const char *pat2 = "crsql_fract_as_ordered";
        for (int j = 0; j < 22; j++) {
          char c = lineStart[i + j];
          if (c >= 'A' && c <= 'Z') c += 32;
          if (c != pat2[j]) { match = 0; break; }
        }
        if (match) { skip = 1; break; }
      }
    }

    if (!skip) {
      memcpy(result + outPos, lineStart, lineLen);
      outPos += lineLen;
      if (*lineEnd == '\n') {
        result[outPos++] = '\n';
      }
    }

    lineStart = *lineEnd ? lineEnd + 1 : lineEnd;
  }
  result[outPos] = '\0';
  return result;
}

// ---------- helpers ----------

static int is_crr_check(sqlite3 *db, const char *table) {
  // Check if the table has the __crsql_itrig trigger
  sqlite3_stmt *pStmt = 0;
  char *sql = sqlite3_mprintf(
      "SELECT 1 FROM sqlite_master WHERE type = 'trigger' AND name = '%q__crsql_itrig'",
      table);
  if (!sql) return 0;
  int rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return 0;
  rc = sqlite3_step(pStmt);
  sqlite3_finalize(pStmt);
  return rc == SQLITE_ROW ? 1 : 0;
}

static int drop_tables(sqlite3 *db, char **tables, int numTables) {
  for (int i = 0; i < numTables; i++) {
    char *esc = crsql_escape_ident(tables[i]);
    char *sql = sqlite3_mprintf("DROP TABLE \"%s\"", esc);
    sqlite3_free(esc);
    if (!sql) return SQLITE_NOMEM;
    int rc = sqlite3_exec(db, sql, 0, 0, 0);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return rc;
  }
  return SQLITE_OK;
}

static int drop_indices(sqlite3 *db, char **indices, int numIndices) {
  for (int i = 0; i < numIndices; i++) {
    char *esc = crsql_escape_ident(indices[i]);
    char *sql = sqlite3_mprintf("DROP INDEX IF EXISTS \"%s\"", esc);
    sqlite3_free(esc);
    if (!sql) return SQLITE_NOMEM;
    int rc = sqlite3_exec(db, sql, 0, 0, 0);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return rc;
  }
  return SQLITE_OK;
}

static int add_column(sqlite3 *db, const char *table, const char *name,
                      const char *colType, int notnull,
                      sqlite3_value *dfltVal) {
  char *escTbl = crsql_escape_ident(table);
  char *escCol = crsql_escape_ident(name);
  const char *dfltStr = "";
  char *dfltBuf = 0;
  if (sqlite3_value_type(dfltVal) != SQLITE_NULL) {
    dfltBuf = sqlite3_mprintf("DEFAULT %s",
                              (const char *)sqlite3_value_text(dfltVal));
    dfltStr = dfltBuf;
  }
  char *sql = sqlite3_mprintf(
      "ALTER TABLE \"%s\" ADD COLUMN \"%s\" %s %s%s",
      escTbl, escCol, colType, notnull ? "NOT NULL " : "", dfltStr);
  sqlite3_free(escTbl);
  sqlite3_free(escCol);
  sqlite3_free(dfltBuf);
  if (!sql) return SQLITE_NOMEM;
  int rc = sqlite3_exec(db, sql, 0, 0, 0);
  sqlite3_free(sql);
  return rc;
}

static int add_columns(sqlite3 *db, const char *table, char **cols,
                       int numCols, sqlite3 *memDb) {
  if (numCols == 0) return SQLITE_OK;

  // Build query with N binding slots
  char *bindings = crsql_binding_list(numCols);
  char *sql = sqlite3_mprintf(
      "SELECT name, type, \"notnull\", dflt_value, pk FROM pragma_table_info(?) WHERE name IN (%s)",
      bindings);
  sqlite3_free(bindings);
  if (!sql) return SQLITE_NOMEM;

  sqlite3_stmt *pStmt = 0;
  int rc = sqlite3_prepare_v2(memDb, sql, -1, &pStmt, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return rc;

  sqlite3_bind_text(pStmt, 1, table, -1, SQLITE_STATIC);
  for (int i = 0; i < numCols; i++) {
    sqlite3_bind_text(pStmt, i + 2, cols[i], -1, SQLITE_STATIC);
  }

  int processed = 0;
  while (sqlite3_step(pStmt) == SQLITE_ROW) {
    int isPk = sqlite3_column_int(pStmt, 4);
    if (isPk) {
      sqlite3_finalize(pStmt);
      return SQLITE_MISUSE;
    }
    const char *colName = (const char *)sqlite3_column_text(pStmt, 0);
    const char *colType = (const char *)sqlite3_column_text(pStmt, 1);
    int notnull = sqlite3_column_int(pStmt, 2);
    sqlite3_value *dfltVal = sqlite3_column_value(pStmt, 3);
    rc = add_column(db, table, colName, colType, notnull, dfltVal);
    if (rc != SQLITE_OK) {
      sqlite3_finalize(pStmt);
      return rc;
    }
    processed++;
  }
  sqlite3_finalize(pStmt);
  if (processed != numCols) return SQLITE_ERROR;
  return SQLITE_OK;
}

static int drop_columns_fn(sqlite3 *db, const char *table, char **cols,
                            int numCols) {
  // Drop the fractindex view first
  char *esc = crsql_escape_ident(table);
  char *sql = sqlite3_mprintf("DROP VIEW IF EXISTS \"%s_fractindex\"", esc);
  sqlite3_free(esc);
  if (!sql) return SQLITE_NOMEM;
  int rc = sqlite3_exec(db, sql, 0, 0, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return rc;

  for (int i = 0; i < numCols; i++) {
    char *escTbl = crsql_escape_ident(table);
    char *escCol = crsql_escape_ident(cols[i]);
    sql = sqlite3_mprintf("ALTER TABLE \"%s\" DROP \"%s\"", escTbl, escCol);
    sqlite3_free(escTbl);
    sqlite3_free(escCol);
    if (!sql) return SQLITE_NOMEM;
    rc = sqlite3_exec(db, sql, 0, 0, 0);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return rc;
  }
  return SQLITE_OK;
}

static int maybe_recreate_index(sqlite3 *db, const char *table,
                                const char *idx, sqlite3 *memDb) {
  // Compare uniqueness
  const char *isUniqueSql =
      "SELECT \"unique\" FROM pragma_index_list(?) WHERE name = ?";
  sqlite3_stmt *memStmt = 0, *localStmt = 0;
  int rc;

  rc = sqlite3_prepare_v2(memDb, isUniqueSql, -1, &memStmt, 0);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_text(memStmt, 1, table, -1, SQLITE_STATIC);
  sqlite3_bind_text(memStmt, 2, idx, -1, SQLITE_STATIC);

  rc = sqlite3_prepare_v2(db, isUniqueSql, -1, &localStmt, 0);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(memStmt);
    return rc;
  }
  sqlite3_bind_text(localStmt, 1, table, -1, SQLITE_STATIC);
  sqlite3_bind_text(localStmt, 2, idx, -1, SQLITE_STATIC);

  if (sqlite3_step(memStmt) != SQLITE_ROW ||
      sqlite3_step(localStmt) != SQLITE_ROW) {
    sqlite3_finalize(memStmt);
    sqlite3_finalize(localStmt);
    return SQLITE_ERROR;
  }

  if (sqlite3_column_int(memStmt, 0) != sqlite3_column_int(localStmt, 0)) {
    sqlite3_finalize(memStmt);
    sqlite3_finalize(localStmt);
    // Drop and let schema reapplication recreate
    char *escIdx = crsql_escape_ident(idx);
    char *dropSql = sqlite3_mprintf("DROP INDEX IF EXISTS \"%s\"", escIdx);
    sqlite3_free(escIdx);
    if (!dropSql) return SQLITE_NOMEM;
    rc = sqlite3_exec(db, dropSql, 0, 0, 0);
    sqlite3_free(dropSql);
    return rc;
  }
  sqlite3_finalize(memStmt);
  sqlite3_finalize(localStmt);

  // Compare columns
  const char *idxColsSql =
      "SELECT name FROM pragma_index_info(?) ORDER BY seqno ASC";
  rc = sqlite3_prepare_v2(memDb, idxColsSql, -1, &memStmt, 0);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_text(memStmt, 1, idx, -1, SQLITE_STATIC);

  rc = sqlite3_prepare_v2(db, idxColsSql, -1, &localStmt, 0);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(memStmt);
    return rc;
  }
  sqlite3_bind_text(localStmt, 1, idx, -1, SQLITE_STATIC);

  int needRecreate = 0;
  int memRc = sqlite3_step(memStmt);
  int localRc = sqlite3_step(localStmt);
  while (memRc == SQLITE_ROW && localRc == SQLITE_ROW) {
    const char *memCol = (const char *)sqlite3_column_text(memStmt, 0);
    const char *localCol = (const char *)sqlite3_column_text(localStmt, 0);
    if (strcmp(memCol, localCol) != 0) {
      needRecreate = 1;
      break;
    }
    memRc = sqlite3_step(memStmt);
    localRc = sqlite3_step(localStmt);
  }
  if (memRc != localRc) needRecreate = 1;

  sqlite3_finalize(memStmt);
  sqlite3_finalize(localStmt);

  if (needRecreate) {
    char *escIdx = crsql_escape_ident(idx);
    char *dropSql = sqlite3_mprintf("DROP INDEX IF EXISTS \"%s\"", escIdx);
    sqlite3_free(escIdx);
    if (!dropSql) return SQLITE_NOMEM;
    rc = sqlite3_exec(db, dropSql, 0, 0, 0);
    sqlite3_free(dropSql);
    return rc;
  }

  return SQLITE_OK;
}

static int maybe_update_indices(sqlite3 *db, const char *table,
                                sqlite3 *memDb) {
  const char *sql = "SELECT name FROM pragma_index_list(?) WHERE origin != 'pk'";
  sqlite3_stmt *localFetch = 0, *memFetch = 0;
  int rc;

  rc = sqlite3_prepare_v2(db, sql, -1, &localFetch, 0);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_text(localFetch, 1, table, -1, SQLITE_STATIC);

  rc = sqlite3_prepare_v2(memDb, sql, -1, &memFetch, 0);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(localFetch);
    return rc;
  }
  sqlite3_bind_text(memFetch, 1, table, -1, SQLITE_STATIC);

  // Collect mem indices
  int memCap = 8, memLen = 0;
  char **memIndices = sqlite3_malloc(sizeof(char *) * memCap);
  while (sqlite3_step(memFetch) == SQLITE_ROW) {
    if (memLen >= memCap) {
      memCap *= 2;
      memIndices = sqlite3_realloc(memIndices, sizeof(char *) * memCap);
    }
    memIndices[memLen++] =
        sqlite3_mprintf("%s", sqlite3_column_text(memFetch, 0));
  }
  sqlite3_finalize(memFetch);

  // Collect local, partition into removed and maybe_modified
  int remCap = 8, remLen = 0;
  char **removed = sqlite3_malloc(sizeof(char *) * remCap);
  int modCap = 8, modLen = 0;
  char **modified = sqlite3_malloc(sizeof(char *) * modCap);

  while (sqlite3_step(localFetch) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(localFetch, 0);
    int found = 0;
    for (int i = 0; i < memLen; i++) {
      if (strcmp(memIndices[i], name) == 0) {
        found = 1;
        break;
      }
    }
    if (!found) {
      if (remLen >= remCap) {
        remCap *= 2;
        removed = sqlite3_realloc(removed, sizeof(char *) * remCap);
      }
      removed[remLen++] = sqlite3_mprintf("%s", name);
    } else {
      if (modLen >= modCap) {
        modCap *= 2;
        modified = sqlite3_realloc(modified, sizeof(char *) * modCap);
      }
      modified[modLen++] = sqlite3_mprintf("%s", name);
    }
  }
  sqlite3_finalize(localFetch);

  rc = drop_indices(db, removed, remLen);
  if (rc == SQLITE_OK) {
    for (int i = 0; i < modLen; i++) {
      rc = maybe_recreate_index(db, table, modified[i], memDb);
      if (rc != SQLITE_OK) break;
    }
  }

  for (int i = 0; i < memLen; i++) sqlite3_free(memIndices[i]);
  sqlite3_free(memIndices);
  for (int i = 0; i < remLen; i++) sqlite3_free(removed[i]);
  sqlite3_free(removed);
  for (int i = 0; i < modLen; i++) sqlite3_free(modified[i]);
  sqlite3_free(modified);

  return rc;
}

static int maybe_modify_table(sqlite3 *db, const char *table,
                              sqlite3 *memDb) {
  const char *sql = "SELECT name FROM pragma_table_info(?)";
  sqlite3_stmt *localStmt = 0, *memStmt = 0;
  int rc;

  rc = sqlite3_prepare_v2(db, sql, -1, &localStmt, 0);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_text(localStmt, 1, table, -1, SQLITE_STATIC);

  rc = sqlite3_prepare_v2(memDb, sql, -1, &memStmt, 0);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(localStmt);
    return rc;
  }
  sqlite3_bind_text(memStmt, 1, table, -1, SQLITE_STATIC);

  // Collect mem columns
  int memCap = 16, memLen = 0;
  char **memCols = sqlite3_malloc(sizeof(char *) * memCap);

  while (sqlite3_step(memStmt) == SQLITE_ROW) {
    if (memLen >= memCap) {
      memCap *= 2;
      memCols = sqlite3_realloc(memCols, sizeof(char *) * memCap);
    }
    memCols[memLen++] =
        sqlite3_mprintf("%s", sqlite3_column_text(memStmt, 0));
  }
  sqlite3_finalize(memStmt);

  // Collect local columns, find removed and added
  int localCap = 16, localLen = 0;
  char **localCols = sqlite3_malloc(sizeof(char *) * localCap);
  int remCap = 8, remLen = 0;
  char **removedCols = sqlite3_malloc(sizeof(char *) * remCap);

  while (sqlite3_step(localStmt) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(localStmt, 0);
    if (localLen >= localCap) {
      localCap *= 2;
      localCols = sqlite3_realloc(localCols, sizeof(char *) * localCap);
    }
    localCols[localLen++] = sqlite3_mprintf("%s", name);

    int found = 0;
    for (int i = 0; i < memLen; i++) {
      if (strcmp(memCols[i], name) == 0) { found = 1; break; }
    }
    if (!found) {
      if (remLen >= remCap) {
        remCap *= 2;
        removedCols = sqlite3_realloc(removedCols, sizeof(char *) * remCap);
      }
      removedCols[remLen++] = sqlite3_mprintf("%s", name);
    }
  }
  sqlite3_finalize(localStmt);

  // Find added columns (in mem but not local)
  int addCap = 8, addLen = 0;
  char **addedCols = sqlite3_malloc(sizeof(char *) * addCap);
  for (int i = 0; i < memLen; i++) {
    int found = 0;
    for (int j = 0; j < localLen; j++) {
      if (strcmp(localCols[j], memCols[i]) == 0) { found = 1; break; }
    }
    if (!found) {
      if (addLen >= addCap) {
        addCap *= 2;
        addedCols = sqlite3_realloc(addedCols, sizeof(char *) * addCap);
      }
      addedCols[addLen++] = sqlite3_mprintf("%s", memCols[i]);
    }
  }

  // If this is a CRR, begin alter
  int isCrr = is_crr_check(db, table);
  if (isCrr) {
    sqlite3_stmt *s = 0;
    sqlite3_prepare_v2(db, "SELECT crsql_begin_alter(?)", -1, &s, 0);
    sqlite3_bind_text(s, 1, table, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
  }

  rc = drop_columns_fn(db, table, removedCols, remLen);
  if (rc == SQLITE_OK) {
    rc = add_columns(db, table, addedCols, addLen, memDb);
  }
  if (rc == SQLITE_OK) {
    rc = maybe_update_indices(db, table, memDb);
  }

  if (isCrr && rc == SQLITE_OK) {
    sqlite3_stmt *s = 0;
    sqlite3_prepare_v2(db, "SELECT crsql_commit_alter(?)", -1, &s, 0);
    sqlite3_bind_text(s, 1, table, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
  }

  // Cleanup
  for (int i = 0; i < memLen; i++) sqlite3_free(memCols[i]);
  sqlite3_free(memCols);
  for (int i = 0; i < localLen; i++) sqlite3_free(localCols[i]);
  sqlite3_free(localCols);
  for (int i = 0; i < remLen; i++) sqlite3_free(removedCols[i]);
  sqlite3_free(removedCols);
  for (int i = 0; i < addLen; i++) sqlite3_free(addedCols[i]);
  sqlite3_free(addedCols);

  return rc;
}

static int migrate_to(sqlite3 *localDb, sqlite3 *memDb) {
  const char *sql =
      "SELECT name FROM sqlite_master WHERE type = 'table'"
      " AND name NOT LIKE 'sqlite_%%'"
      " AND name NOT LIKE 'crsql_%%'"
      " AND name NOT LIKE '__crsql_%%'"
      " AND name NOT LIKE '%%__crsql_%%'";

  sqlite3_stmt *memFetch = 0, *localFetch = 0;
  int rc;

  rc = sqlite3_prepare_v2(memDb, sql, -1, &memFetch, 0);
  if (rc != SQLITE_OK) return rc;
  rc = sqlite3_prepare_v2(localDb, sql, -1, &localFetch, 0);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(memFetch);
    return rc;
  }

  // Collect mem tables
  int memCap = 16, memLen = 0;
  char **memTables = sqlite3_malloc(sizeof(char *) * memCap);
  while (sqlite3_step(memFetch) == SQLITE_ROW) {
    if (memLen >= memCap) {
      memCap *= 2;
      memTables = sqlite3_realloc(memTables, sizeof(char *) * memCap);
    }
    memTables[memLen++] =
        sqlite3_mprintf("%s", sqlite3_column_text(memFetch, 0));
  }
  sqlite3_finalize(memFetch);

  // Partition local tables
  int remCap = 8, remLen = 0;
  char **removedTables = sqlite3_malloc(sizeof(char *) * remCap);
  int modCap = 8, modLen = 0;
  char **modifiedTables = sqlite3_malloc(sizeof(char *) * modCap);

  while (sqlite3_step(localFetch) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(localFetch, 0);
    int found = 0;
    for (int i = 0; i < memLen; i++) {
      if (strcmp(memTables[i], name) == 0) { found = 1; break; }
    }
    if (found) {
      if (modLen >= modCap) {
        modCap *= 2;
        modifiedTables = sqlite3_realloc(modifiedTables, sizeof(char *) * modCap);
      }
      modifiedTables[modLen++] = sqlite3_mprintf("%s", name);
    } else {
      if (remLen >= remCap) {
        remCap *= 2;
        removedTables = sqlite3_realloc(removedTables, sizeof(char *) * remCap);
      }
      removedTables[remLen++] = sqlite3_mprintf("%s", name);
    }
  }
  sqlite3_finalize(localFetch);

  rc = drop_tables(localDb, removedTables, remLen);
  if (rc == SQLITE_OK) {
    for (int i = 0; i < modLen; i++) {
      rc = maybe_modify_table(localDb, modifiedTables[i], memDb);
      if (rc != SQLITE_OK) break;
    }
  }

  for (int i = 0; i < memLen; i++) sqlite3_free(memTables[i]);
  sqlite3_free(memTables);
  for (int i = 0; i < remLen; i++) sqlite3_free(removedTables[i]);
  sqlite3_free(removedTables);
  for (int i = 0; i < modLen; i++) sqlite3_free(modifiedTables[i]);
  sqlite3_free(modifiedTables);

  return rc;
}

// ---------- crsql_automigrate ----------

void crsql_automigrate(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  if (argc < 1) {
    sqlite3_result_error(ctx, "Expected a schema to migrate to", -1);
    return;
  }

  sqlite3 *localDb = sqlite3_context_db_handle(ctx);
  const char *desiredSchema = (const char *)sqlite3_value_text(argv[0]);
  char *strippedSchema = strip_crr_statements(desiredSchema);
  if (!strippedSchema) {
    sqlite3_result_error(ctx, "out of memory", -1);
    return;
  }

  sqlite3 *memDb = 0;
  int rc = sqlite3_open(":memory:", &memDb);
  if (rc != SQLITE_OK) {
    sqlite3_free(strippedSchema);
    sqlite3_result_error(ctx, "could not open the temporary migration db", -1);
    return;
  }

  rc = sqlite3_exec(memDb, strippedSchema, 0, 0, 0);
  sqlite3_free(strippedSchema);
  if (rc != SQLITE_OK) {
    const char *memErr = sqlite3_errmsg(memDb);
    sqlite3_result_error(ctx, memErr, -1);
    if (argc == 2) {
      const char *cleanup = (const char *)sqlite3_value_text(argv[1]);
      if (cleanup) sqlite3_exec(memDb, cleanup, 0, 0, 0);
    }
    sqlite3_close(memDb);
    return;
  }

  rc = sqlite3_exec(localDb, "SAVEPOINT automigrate_tables", 0, 0, 0);
  if (rc != SQLITE_OK) {
    sqlite3_close(memDb);
    sqlite3_result_error(ctx, "failed to start savepoint", -1);
    return;
  }

  rc = migrate_to(localDb, memDb);
  if (rc != SQLITE_OK) {
    sqlite3_exec(localDb, "ROLLBACK", 0, 0, 0);
    const char *memErr = sqlite3_errmsg(memDb);
    sqlite3_result_error(ctx, memErr, -1);
    if (argc == 2) {
      const char *cleanup = (const char *)sqlite3_value_text(argv[1]);
      if (cleanup) sqlite3_exec(memDb, cleanup, 0, 0, 0);
    }
    sqlite3_close(memDb);
    return;
  }

  if (argc == 2) {
    const char *cleanup = (const char *)sqlite3_value_text(argv[1]);
    if (cleanup) sqlite3_exec(memDb, cleanup, 0, 0, 0);
  }
  sqlite3_close(memDb);

  // Apply the full schema (including crsql_as_crr statements)
  if (desiredSchema && strlen(desiredSchema) > 0) {
    rc = sqlite3_exec(localDb, desiredSchema, 0, 0, 0);
    if (rc != SQLITE_OK) {
      sqlite3_exec(localDb, "ROLLBACK", 0, 0, 0);
      sqlite3_result_error(ctx, sqlite3_errmsg(localDb), -1);
      return;
    }
  }

  sqlite3_exec(localDb, "RELEASE automigrate_tables", 0, 0, 0);
  sqlite3_result_text(ctx, "migration complete", -1, SQLITE_STATIC);
}

// ---------- compact_post_alter ----------

int crsql_compact_post_alter(sqlite3 *db, const char *tblName,
                             crsql_ExtData *pExtData, char **errmsg) {
  // Fill db version
  // (forward declaration - implemented in db-version.c)
  extern int crsql_fill_db_version_if_needed(sqlite3 *, crsql_ExtData *,
                                             char **);
  int rc = crsql_fill_db_version_if_needed(db, pExtData, errmsg);
  if (rc != SQLITE_OK) return rc;

  sqlite3_int64 currentDbVersion = pExtData->dbVersion;

  // Check if PK columns changed
  char *escVal = crsql_escape_ident_as_value(tblName);
  char *checkSql = sqlite3_mprintf(
      "SELECT count(name) FROM ("
      "SELECT name FROM pragma_table_info('%s')"
      " WHERE pk > 0 AND name NOT IN"
      " (SELECT name FROM pragma_index_info('%s__crsql_pks_pks'))"
      " UNION SELECT name FROM pragma_index_info('%s__crsql_pks_pks')"
      " WHERE name NOT IN"
      " (SELECT name FROM pragma_table_info('%s') WHERE pk > 0)"
      " AND name != 'col_name'"
      ");",
      escVal, escVal, escVal, escVal);
  sqlite3_free(escVal);
  if (!checkSql) return SQLITE_NOMEM;

  sqlite3_stmt *pStmt = 0;
  rc = sqlite3_prepare_v2(db, checkSql, -1, &pStmt, 0);
  sqlite3_free(checkSql);
  if (rc != SQLITE_OK) return rc;
  sqlite3_step(pStmt);
  int pkDiff = sqlite3_column_int(pStmt, 0);
  sqlite3_finalize(pStmt);

  if (pkDiff > 0) {
    char *escIdent = crsql_escape_ident(tblName);
    char *dropSql = sqlite3_mprintf(
        "DROP TABLE \"%s__crsql_clock\";"
        "DROP TABLE \"%s__crsql_pks\";",
        escIdent, escIdent);
    sqlite3_free(escIdent);
    if (!dropSql) return SQLITE_NOMEM;
    rc = sqlite3_exec(db, dropSql, 0, 0, 0);
    sqlite3_free(dropSql);
    if (rc != SQLITE_OK) return rc;
  } else {
    // Compact: remove entries for dropped columns
    char *escIdent = crsql_escape_ident(tblName);
    escVal = crsql_escape_ident_as_value(tblName);
    char *sql = sqlite3_mprintf(
        "DELETE FROM \"%s__crsql_clock\" WHERE \"col_name\" NOT IN ("
        "SELECT name FROM pragma_table_info('%s') UNION SELECT '" DELETE_SENTINEL "'"
        ")",
        escIdent, escVal);
    if (!sql) {
      sqlite3_free(escIdent);
      sqlite3_free(escVal);
      return SQLITE_NOMEM;
    }
    rc = sqlite3_exec(db, sql, 0, 0, 0);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) {
      sqlite3_free(escIdent);
      sqlite3_free(escVal);
      return rc;
    }

    // Ensure table infos are up to date
    rc = crsql_ensure_table_infos_are_up_to_date(db, pExtData, errmsg);
    if (rc != SQLITE_OK) {
      sqlite3_free(escIdent);
      sqlite3_free(escVal);
      return rc;
    }

    // Find the table info
    crsql_TableInfoVec *vec = (crsql_TableInfoVec *)pExtData->tableInfos;
    int idx = crsql_find_table_info(vec, tblName);
    if (idx < 0) {
      sqlite3_free(escIdent);
      sqlite3_free(escVal);
      return SQLITE_ERROR;
    }
    crsql_TableInfo *tblInfo = &vec->aInfos[idx];

    // Delete orphaned clock entries (no matching row in source table)
    // Build: DELETE FROM clock WHERE (not sentinel or odd sentinel)
    //        AND NOT EXISTS (SELECT 1 FROM tbl JOIN pks ON ...)
    char *joinConds = sqlite3_malloc(1);
    joinConds[0] = '\0';
    for (int i = 0; i < tblInfo->pksLen; i++) {
      char *cond;
      if (i == 0) {
        cond = sqlite3_mprintf(
            "\"%s\".\"%s\" = \"%s__crsql_pks\".\"%s\"",
            escIdent, tblInfo->pks[i].name, escIdent, tblInfo->pks[i].name);
      } else {
        cond = sqlite3_mprintf(
            "%s AND \"%s\".\"%s\" = \"%s__crsql_pks\".\"%s\"",
            joinConds, escIdent, tblInfo->pks[i].name, escIdent,
            tblInfo->pks[i].name);
      }
      sqlite3_free(joinConds);
      joinConds = cond;
    }

    sql = sqlite3_mprintf(
        "DELETE FROM \"%s__crsql_clock\" WHERE "
        "(col_name != '-1' OR (col_name = '-1' AND col_version %% 2 != 0))"
        " AND NOT EXISTS (SELECT 1 FROM \"%s\" JOIN \"%s__crsql_pks\" ON %s"
        " WHERE \"%s__crsql_clock\".key = \"%s__crsql_pks\".__crsql_key LIMIT 1)",
        escIdent, escIdent, escIdent, joinConds, escIdent, escIdent);
    sqlite3_free(joinConds);
    if (!sql) {
      sqlite3_free(escIdent);
      sqlite3_free(escVal);
      return SQLITE_NOMEM;
    }
    rc = sqlite3_exec(db, sql, 0, 0, 0);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) {
      sqlite3_free(escIdent);
      sqlite3_free(escVal);
      return rc;
    }

    // Delete orphaned pk lookasides
    sql = sqlite3_mprintf(
        "DELETE FROM \"%s__crsql_pks\" WHERE __crsql_key NOT IN ("
        "SELECT key FROM \"%s__crsql_clock\")",
        escIdent, escIdent);
    sqlite3_free(escIdent);
    sqlite3_free(escVal);
    if (!sql) return SQLITE_NOMEM;
    rc = sqlite3_exec(db, sql, 0, 0, 0);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return rc;
  }

  // Record pre_compact_dbversion
  sqlite3_stmt *s = 0;
  rc = sqlite3_prepare_v2(
      db,
      "INSERT OR REPLACE INTO crsql_master (key, value) VALUES ('pre_compact_dbversion', ?)",
      -1, &s, 0);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_int64(s, 1, currentDbVersion);
  sqlite3_step(s);
  sqlite3_finalize(s);

  return SQLITE_OK;
}
