#include "str_util.h"
#include "consts.h"
#include <string.h>
#include <stdio.h>

// Count occurrences of character in string
static int count_char(const char *str, char ch) {
  int count = 0;
  while (*str) {
    if (*str == ch) count++;
    str++;
  }
  return count;
}

// Escape double quotes in SQL identifiers (foo"bar -> foo""bar)
char *crsql_escape_ident(const char *ident) {
  if (!ident) return NULL;

  int quote_count = count_char(ident, '"');
  int len = strlen(ident);
  char *result = sqlite3_malloc(len + quote_count + 1);
  if (!result) return NULL;

  char *dst = result;
  const char *src = ident;
  while (*src) {
    *dst++ = *src;
    if (*src == '"') {
      *dst++ = '"';  // Double the quote
    }
    src++;
  }
  *dst = '\0';

  return result;
}

// Escape single quotes in SQL string values (foo'bar -> foo''bar)
char *crsql_escape_ident_as_value(const char *ident) {
  if (!ident) return NULL;

  int quote_count = count_char(ident, '\'');
  int len = strlen(ident);
  char *result = sqlite3_malloc(len + quote_count + 1);
  if (!result) return NULL;

  char *dst = result;
  const char *src = ident;
  while (*src) {
    *dst++ = *src;
    if (*src == '\'') {
      *dst++ = '\'';  // Double the quote
    }
    src++;
  }
  *dst = '\0';

  return result;
}

// Build comma-separated identifier list
// Example: columns=["id", "name"], prefix="NEW." -> 'NEW."id", NEW."name"'
char *crsql_as_identifier_list(crsql_ColumnInfo **columns, int columns_len, const char *prefix) {
  if (!columns || columns_len == 0) return sqlite3_malloc(1); // Return empty string

  // Calculate needed size
  int total_size = 0;
  for (int i = 0; i < columns_len; i++) {
    if (!columns[i] || !columns[i]->name) continue;

    int prefix_len = prefix ? strlen(prefix) : 0;
    int name_len = strlen(columns[i]->name);
    int quote_count = count_char(columns[i]->name, '"');

    // prefix + " + escaped_name + " + , (if not last)
    total_size += prefix_len + 1 + name_len + quote_count + 1;
    if (i < columns_len - 1) total_size += 2;  // ", "
  }
  total_size++;  // null terminator

  char *result = sqlite3_malloc(total_size);
  if (!result) return NULL;

  char *dst = result;
  for (int i = 0; i < columns_len; i++) {
    if (!columns[i] || !columns[i]->name) continue;

    // Add prefix if provided
    if (prefix) {
      strcpy(dst, prefix);
      dst += strlen(prefix);
    }

    // Add opening quote
    *dst++ = '"';

    // Add escaped column name
    const char *src = columns[i]->name;
    while (*src) {
      *dst++ = *src;
      if (*src == '"') {
        *dst++ = '"';  // Double quotes
      }
      src++;
    }

    // Add closing quote
    *dst++ = '"';

    // Add comma separator if not last
    if (i < columns_len - 1) {
      *dst++ = ',';
      *dst++ = ' ';
    }
  }
  *dst = '\0';

  return result;
}

// Build WHERE clause with IS ? for each column
char *crsql_where_list(crsql_ColumnInfo **columns, int columns_len, const char *prefix) {
  if (!columns || columns_len == 0) return sqlite3_mprintf("");

  // Calculate needed size
  int total_size = 0;
  for (int i = 0; i < columns_len; i++) {
    if (!columns[i] || !columns[i]->name) continue;

    int prefix_len = prefix ? strlen(prefix) : 0;
    int name_len = strlen(columns[i]->name);
    int quote_count = count_char(columns[i]->name, '"');

    // prefix + " + escaped_name + " IS ? + " AND " (if not last)
    total_size += prefix_len + 1 + name_len + quote_count + 1 + 4;  // 4 for " IS ?"
    if (i < columns_len - 1) total_size += 5;  // " AND "
  }
  total_size++;

  char *result = sqlite3_malloc(total_size);
  if (!result) return NULL;

  char *dst = result;
  for (int i = 0; i < columns_len; i++) {
    if (!columns[i] || !columns[i]->name) continue;

    // Add prefix if provided
    if (prefix) {
      strcpy(dst, prefix);
      dst += strlen(prefix);
    }

    // Add opening quote
    *dst++ = '"';

    // Add escaped column name
    const char *src = columns[i]->name;
    while (*src) {
      *dst++ = *src;
      if (*src == '"') {
        *dst++ = '"';
      }
      src++;
    }

    // Add closing quote and IS ?
    *dst++ = '"';
    *dst++ = ' ';
    *dst++ = 'I';
    *dst++ = 'S';
    *dst++ = ' ';
    *dst++ = '?';

    // Add AND separator if not last
    if (i < columns_len - 1) {
      *dst++ = ' ';
      *dst++ = 'A';
      *dst++ = 'N';
      *dst++ = 'D';
      *dst++ = ' ';
    }
  }
  *dst = '\0';

  return result;
}

// Build binding list: "?, ?, ?"
char *crsql_binding_list(int num_slots) {
  if (num_slots <= 0) return sqlite3_mprintf("");

  // "?" = 1 char, ", " = 2 chars, so (num_slots * 3) - 2 (no trailing ", ")
  int size = (num_slots * 3) - 2 + 1;  // +1 for null terminator
  char *result = sqlite3_malloc(size);
  if (!result) return NULL;

  char *dst = result;
  for (int i = 0; i < num_slots; i++) {
    *dst++ = '?';
    if (i < num_slots - 1) {
      *dst++ = ',';
      *dst++ = ' ';
    }
  }
  *dst = '\0';

  return result;
}

// Get default value for a column
char *crsql_get_dflt_value(sqlite3 *db, const char *table, const char *col) {
  sqlite3_stmt *stmt = NULL;
  const char *sql = "SELECT [dflt_value], [notnull] FROM pragma_table_info(?) WHERE name = ?";

  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return NULL;

  rc = sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return NULL;
  }

  rc = sqlite3_bind_text(stmt, 2, col, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return NULL;
  }

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) {
    // Column not found - shouldn't happen
    sqlite3_finalize(stmt);
    return NULL;
  }

  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return NULL;
  }

  int notnull = sqlite3_column_int(stmt, 1);
  int dflt_type = sqlite3_column_type(stmt, 0);

  char *result = NULL;

  // If column is nullable and no default specified, return "NULL"
  if (notnull == 0 && dflt_type == SQLITE_NULL) {
    result = sqlite3_mprintf("NULL");
  }
  // If not nullable and no default, return NULL
  else if (dflt_type == SQLITE_NULL) {
    result = NULL;
  }
  // Otherwise return the default value
  else {
    const char *dflt_text = (const char *)sqlite3_column_text(stmt, 0);
    result = sqlite3_mprintf("%s", dflt_text);
  }

  sqlite3_finalize(stmt);
  return result;
}

// Build UNION query for getting max db_version from all clock tables
char *crsql_get_db_version_union_query(char **table_names, int num_tables) {
  if (!table_names || num_tables == 0) {
    return sqlite3_mprintf(
      "SELECT max(version) as version FROM "
      "(SELECT value as version FROM crsql_master WHERE key = 'pre_compact_dbversion')"
    );
  }

  // Build the UNION ALL query
  char *unions = NULL;
  for (int i = 0; i < num_tables; i++) {
    char *escaped = crsql_escape_ident(table_names[i]);
    if (!escaped) {
      if (unions) sqlite3_free(unions);
      return NULL;
    }

    char *new_unions;
    if (unions) {
      new_unions = sqlite3_mprintf("%s UNION ALL SELECT max(db_version) as version FROM \"%s\"",
                                   unions, escaped);
      sqlite3_free(unions);
    } else {
      new_unions = sqlite3_mprintf("SELECT max(db_version) as version FROM \"%s\"", escaped);
    }
    sqlite3_free(escaped);

    if (!new_unions) {
      return NULL;
    }
    unions = new_unions;
  }

  char *result = sqlite3_mprintf(
    "SELECT max(version) as version FROM (%s UNION SELECT value as version FROM crsql_master WHERE key = 'pre_compact_dbversion')",
    unions
  );
  sqlite3_free(unions);

  return result;
}

// Calculate slab rowid
sqlite3_int64 crsql_slab_rowid(int idx, sqlite3_int64 rowid) {
  if (idx < 0) {
    return -1;
  }

  sqlite3_int64 modulo = rowid % ROWID_SLAB_SIZE;
  return ((sqlite3_int64)idx * ROWID_SLAB_SIZE) + modulo;
}
