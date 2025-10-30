#include "compare_values.h"
#include <string.h>

// Compare two SQLite values
int crsql_compare_sqlite_values(sqlite3_value *l, sqlite3_value *r) {
  int l_type = sqlite3_value_type(l);
  int r_type = sqlite3_value_type(r);

  // Different types: NULL is less than everything
  // SQLite types: NULL=5, INTEGER=1, FLOAT=2, TEXT=3, BLOB=4
  // We want NULL < all, so we swap the comparison
  if (l_type != r_type) {
    return r_type - l_type;
  }

  switch (l_type) {
    case SQLITE_NULL:
      return 0;

    case SQLITE_INTEGER: {
      sqlite3_int64 l_int = sqlite3_value_int64(l);
      sqlite3_int64 r_int = sqlite3_value_int64(r);
      if (l_int < r_int) return -1;
      if (l_int > r_int) return 1;
      return 0;
    }

    case SQLITE_FLOAT: {
      double l_double = sqlite3_value_double(l);
      double r_double = sqlite3_value_double(r);
      if (l_double < r_double) return -1;
      if (l_double > r_double) return 1;
      return 0;
    }

    case SQLITE_TEXT: {
      const unsigned char *l_text = sqlite3_value_text(l);
      const unsigned char *r_text = sqlite3_value_text(r);
      return strcmp((const char *)l_text, (const char *)r_text);
    }

    case SQLITE_BLOB: {
      const void *l_blob = sqlite3_value_blob(l);
      const void *r_blob = sqlite3_value_blob(r);
      int l_len = sqlite3_value_bytes(l);
      int r_len = sqlite3_value_bytes(r);

      if (l_len != r_len) {
        return (l_len < r_len) ? -1 : 1;
      }

      return memcmp(l_blob, r_blob, l_len);
    }

    default:
      return 0;
  }
}

// Check if any value changed
int crsql_any_value_changed(sqlite3_value **left, sqlite3_value **right, int count) {
  for (int i = 0; i < count; i++) {
    if (crsql_compare_sqlite_values(left[i], right[i]) != 0) {
      return 1;
    }
  }
  return 0;
}
