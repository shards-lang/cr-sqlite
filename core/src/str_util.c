#include "str_util.h"
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
