#include "util.h"

#include <string.h>

#include "consts.h"
#include "crsqlite.h"

char *crsql_escape_ident(const char *ident) {
  int len = (int)strlen(ident);
  // Count double-quotes to size the output buffer
  int numQuotes = 0;
  for (int i = 0; i < len; i++) {
    if (ident[i] == '"') {
      numQuotes++;
    }
  }

  char *result = sqlite3_malloc(len + numQuotes + 1);
  if (result == 0) {
    return 0;
  }

  int j = 0;
  for (int i = 0; i < len; i++) {
    result[j++] = ident[i];
    if (ident[i] == '"') {
      result[j++] = '"';
    }
  }
  result[j] = '\0';
  return result;
}

char *crsql_escape_ident_as_value(const char *ident) {
  int len = (int)strlen(ident);
  int numQuotes = 0;
  for (int i = 0; i < len; i++) {
    if (ident[i] == '\'') {
      numQuotes++;
    }
  }

  char *result = sqlite3_malloc(len + numQuotes + 1);
  if (result == 0) {
    return 0;
  }

  int j = 0;
  for (int i = 0; i < len; i++) {
    result[j++] = ident[i];
    if (ident[i] == '\'') {
      result[j++] = '\'';
    }
  }
  result[j] = '\0';
  return result;
}

char *crsql_binding_list(int numSlots) {
  if (numSlots <= 0) {
    char *empty = sqlite3_malloc(1);
    if (empty) {
      empty[0] = '\0';
    }
    return empty;
  }

  // "?, " per slot except last which is just "?"
  // 3 chars per slot except last = 2, plus null
  int bufLen = numSlots * 3;
  char *result = sqlite3_malloc(bufLen);
  if (result == 0) {
    return 0;
  }

  int pos = 0;
  for (int i = 0; i < numSlots; i++) {
    if (i > 0) {
      result[pos++] = ',';
      result[pos++] = ' ';
    }
    result[pos++] = '?';
  }
  result[pos] = '\0';
  return result;
}

// tableinfo.h is included via util.h

char *crsql_where_list(crsql_ColumnInfo *cols, int numCols,
                       const char *prefix) {
  // Build: "prefix\"escaped_name\" IS ? AND ..."
  char *result = sqlite3_malloc(1);
  if (result == 0) {
    return 0;
  }
  result[0] = '\0';

  for (int i = 0; i < numCols; i++) {
    char *escaped = crsql_escape_ident(cols[i].name);
    if (escaped == 0) {
      sqlite3_free(result);
      return 0;
    }

    char *part;
    if (prefix) {
      part = sqlite3_mprintf("%s\"%s\" IS ?", prefix, escaped);
    } else {
      part = sqlite3_mprintf("\"%s\" IS ?", escaped);
    }
    sqlite3_free(escaped);

    if (part == 0) {
      sqlite3_free(result);
      return 0;
    }

    char *newResult;
    if (i == 0) {
      newResult = sqlite3_mprintf("%s", part);
    } else {
      newResult = sqlite3_mprintf("%s AND %s", result, part);
    }
    sqlite3_free(result);
    sqlite3_free(part);

    if (newResult == 0) {
      return 0;
    }
    result = newResult;
  }

  return result;
}

char *crsql_as_identifier_list(crsql_ColumnInfo *cols, int numCols,
                               const char *prefix) {
  char *result = sqlite3_malloc(1);
  if (result == 0) {
    return 0;
  }
  result[0] = '\0';

  for (int i = 0; i < numCols; i++) {
    char *escaped = crsql_escape_ident(cols[i].name);
    if (escaped == 0) {
      sqlite3_free(result);
      return 0;
    }

    char *part;
    if (prefix) {
      part = sqlite3_mprintf("%s\"%s\"", prefix, escaped);
    } else {
      part = sqlite3_mprintf("\"%s\"", escaped);
    }
    sqlite3_free(escaped);

    if (part == 0) {
      sqlite3_free(result);
      return 0;
    }

    char *newResult;
    if (i == 0) {
      newResult = sqlite3_mprintf("%s", part);
    } else {
      newResult = sqlite3_mprintf("%s,%s", result, part);
    }
    sqlite3_free(result);
    sqlite3_free(part);

    if (newResult == 0) {
      return 0;
    }
    result = newResult;
  }

  return result;
}

sqlite3_int64 crsql_slab_rowid(int idx, sqlite3_int64 rowid) {
  if (idx < 0) {
    return -1;
  }
  sqlite3_int64 modulo = rowid % ROWID_SLAB_SIZE;
  return (sqlite3_int64)idx * ROWID_SLAB_SIZE + modulo;
}

char *crsql_get_db_version_union_query(char **tblNames, int numTables) {
  // Build inner union of SELECT max(db_version) from each clock table
  char *inner = sqlite3_malloc(1);
  if (inner == 0) {
    return 0;
  }
  inner[0] = '\0';

  for (int i = 0; i < numTables; i++) {
    char *escaped = crsql_escape_ident(tblNames[i]);
    if (escaped == 0) {
      sqlite3_free(inner);
      return 0;
    }

    char *part = sqlite3_mprintf(
        "SELECT max(db_version) as version FROM \"%s\"", escaped);
    sqlite3_free(escaped);

    if (part == 0) {
      sqlite3_free(inner);
      return 0;
    }

    char *newInner;
    if (i == 0) {
      newInner = sqlite3_mprintf("%s", part);
    } else {
      newInner = sqlite3_mprintf("%s UNION ALL %s", inner, part);
    }
    sqlite3_free(inner);
    sqlite3_free(part);

    if (newInner == 0) {
      return 0;
    }
    inner = newInner;
  }

  char *result = sqlite3_mprintf(
      "SELECT max(version) as version FROM (%s UNION SELECT value as\n"
      "        version FROM crsql_master WHERE key = 'pre_compact_dbversion')",
      inner);
  sqlite3_free(inner);

  return result;
}

int crsql_get_dflt_value(sqlite3 *db, const char *table, const char *col,
                         char **outDflt) {
  sqlite3_stmt *pStmt = 0;
  int rc = sqlite3_prepare_v2(
      db,
      "SELECT [dflt_value], [notnull] FROM pragma_table_info(?) WHERE name = ?",
      -1, &pStmt, 0);
  if (rc != SQLITE_OK) {
    return rc;
  }

  sqlite3_bind_text(pStmt, 1, table, -1, SQLITE_STATIC);
  sqlite3_bind_text(pStmt, 2, col, -1, SQLITE_STATIC);
  rc = sqlite3_step(pStmt);

  if (rc == SQLITE_DONE) {
    sqlite3_finalize(pStmt);
    *outDflt = 0;
    return SQLITE_DONE;
  }

  if (rc != SQLITE_ROW) {
    sqlite3_finalize(pStmt);
    *outDflt = 0;
    return rc;
  }

  int notnull = sqlite3_column_int(pStmt, 1);
  int dfltType = sqlite3_column_type(pStmt, 0);

  if (notnull == 0 && dfltType == SQLITE_NULL) {
    // nullable column with no default -> default is NULL
    *outDflt = sqlite3_mprintf("NULL");
    sqlite3_finalize(pStmt);
    return SQLITE_OK;
  }

  if (dfltType == SQLITE_NULL) {
    // NOT NULL with no default
    *outDflt = 0;
    sqlite3_finalize(pStmt);
    return SQLITE_OK;
  }

  const char *text = (const char *)sqlite3_column_text(pStmt, 0);
  *outDflt = sqlite3_mprintf("%s", text);
  sqlite3_finalize(pStmt);
  return SQLITE_OK;
}

int crsql_compare_sqlite_values(sqlite3_value *l, sqlite3_value *r) {
  int lType = sqlite3_value_type(l);
  int rType = sqlite3_value_type(r);

  if (lType != rType) {
    // Swap comparison so NULL (type 5) sorts less than all other types
    return rType - lType;
  }

  switch (lType) {
    case SQLITE_BLOB: {
      int lLen = sqlite3_value_bytes(l);
      int rLen = sqlite3_value_bytes(r);
      const void *lBlob = sqlite3_value_blob(l);
      const void *rBlob = sqlite3_value_blob(r);
      int minLen = lLen < rLen ? lLen : rLen;
      int cmp = memcmp(lBlob, rBlob, minLen);
      if (cmp != 0) {
        return cmp;
      }
      if (lLen < rLen) return -1;
      if (lLen > rLen) return 1;
      return 0;
    }
    case SQLITE_FLOAT: {
      double lD = sqlite3_value_double(l);
      double rD = sqlite3_value_double(r);
      if (lD < rD) return -1;
      if (lD > rD) return 1;
      return 0;
    }
    case SQLITE_INTEGER: {
      sqlite3_int64 lI = sqlite3_value_int64(l);
      sqlite3_int64 rI = sqlite3_value_int64(r);
      if (lI < rI) return -1;
      if (lI > rI) return 1;
      return 0;
    }
    case SQLITE_NULL:
      return 0;
    case SQLITE_TEXT: {
      const char *lT = (const char *)sqlite3_value_text(l);
      const char *rT = (const char *)sqlite3_value_text(r);
      return strcmp(lT, rT);
    }
  }
  return 0;
}

int crsql_any_value_changed(sqlite3_value **left, sqlite3_value **right,
                            int numValues) {
  for (int i = 0; i < numValues; i++) {
    if (crsql_compare_sqlite_values(left[i], right[i]) != 0) {
      return 1;
    }
  }
  return 0;
}
