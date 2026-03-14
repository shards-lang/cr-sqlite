#include "fractindex.h"

#include <string.h>
#include <math.h>

/*
 * Fractional indexing module for cr-sqlite.
 *
 * Provides ordered list CRDTs via fractional index keys using a base-95
 * digit alphabet (ASCII 32-126). Keys have a variable-length integer part
 * (encoded by the first character a-z / A-Z) followed by a fractional part.
 *
 * Ported from core/rs/fractindex-core/src/
 */

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

static const char *BASE_95_DIGITS =
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
static const int BASE_95_LEN = 95;

static const char *SMALLEST_INTEGER = "A                          ";
static const int SMALLEST_INTEGER_LEN = 28;
static const char *INTEGER_ZERO = "a ";

static const unsigned char a_charcode = 97;
static const unsigned char z_charcode = 122;
static const unsigned char A_charcode = 65;
static const unsigned char Z_charcode = 90;
static const unsigned char min_charcode = 32;

/* --------------------------------------------------------------------------
 * Local string helpers (self-contained, not from util.h)
 * -------------------------------------------------------------------------- */

/**
 * Escape single-quotes in an argument: ' -> ''
 * Returns a sqlite3_malloc'd string. Caller must sqlite3_free.
 */
static char *fract_escape_arg(const char *arg) {
  if (!arg) return NULL;
  int len = (int)strlen(arg);
  char *out = sqlite3_malloc(len * 2 + 1);
  if (!out) return NULL;
  int j = 0;
  for (int i = 0; i < len; i++) {
    if (arg[i] == '\'') {
      out[j++] = '\'';
      out[j++] = '\'';
    } else {
      out[j++] = arg[i];
    }
  }
  out[j] = '\0';
  return out;
}

/* --------------------------------------------------------------------------
 * Core fractional index algorithm
 * -------------------------------------------------------------------------- */

/**
 * Given the first byte of a key, return the length of the integer part.
 * Returns 0 on error.
 */
static int get_integer_len(unsigned char head) {
  if (head >= a_charcode && head <= z_charcode) {
    return head - a_charcode + 2;
  } else if (head >= A_charcode && head <= Z_charcode) {
    return Z_charcode - head + 2;
  }
  return 0; /* error */
}

/**
 * Validate that the integer part has the correct length.
 * Returns 0 on success, -1 on error.
 */
static int validate_integer(const char *x, int xlen) {
  if (xlen < 1) return -1;
  int expected = get_integer_len((unsigned char)x[0]);
  if (expected == 0 || expected != xlen) return -1;
  return 0;
}

/**
 * Extract the integer part of a key.
 * Sets *int_len to the length of the integer part.
 * Returns 0 on success, -1 on error.
 */
static int get_integer_part(const char *key, int key_len, int *int_len) {
  if (key_len < 1) return -1;
  int ilen = get_integer_len((unsigned char)key[0]);
  if (ilen == 0) return -1;
  if (ilen > key_len) return -1;
  *int_len = ilen;
  return 0;
}

/**
 * Validate an order key.
 * Returns 0 on success, -1 on error, and sets *err to an error string.
 */
static int validate_order_key(const char *key, int key_len, const char **err) {
  if (key_len == SMALLEST_INTEGER_LEN &&
      memcmp(key, SMALLEST_INTEGER, SMALLEST_INTEGER_LEN) == 0) {
    *err = "Key is too small";
    return -1;
  }
  int int_len;
  if (get_integer_part(key, key_len, &int_len) != 0) {
    *err = "head is out of range";
    return -1;
  }
  /* fractional part must not end with space */
  int frac_len = key_len - int_len;
  if (frac_len > 0 && (unsigned char)key[key_len - 1] == min_charcode) {
    *err = "Fractional part should not end with ' ' (space)";
    return -1;
  }
  return 0;
}

static int fract_round(double d) {
  int tenx = (int)(d * 10.0);
  int truncated = (int)d;
  if (tenx - (truncated * 10) >= 5) {
    return truncated + 1;
  }
  return truncated;
}

/**
 * Find a digit position in BASE_95_DIGITS.
 * Returns -1 if not found.
 */
static int digit_index(unsigned char ch) {
  const char *p = memchr(BASE_95_DIGITS, ch, BASE_95_LEN);
  if (!p) return -1;
  return (int)(p - BASE_95_DIGITS);
}

/**
 * Compute the midpoint between two fractional parts.
 * a, a_len: left fractional part (can be "" with a_len=0)
 * b, b_len: right fractional part (NULL means no upper bound)
 * Returns a sqlite3_malloc'd string on success, NULL on error.
 * Sets *err on error.
 */
static char *midpoint(const char *a, int a_len, const char *b, int b_len,
                      const char **err) {
  /* validation */
  if (b && a_len > 0 && b_len > 0) {
    int cmp = memcmp(a, b, a_len < b_len ? a_len : b_len);
    if (cmp > 0 || (cmp == 0 && a_len >= b_len)) {
      *err = "midpoint - a must be before b";
      return NULL;
    }
  }

  if (a_len > 0 && (unsigned char)a[a_len - 1] == min_charcode) {
    *err = "midpoint - a or b must not end with ' ' (space)";
    return NULL;
  }
  if (b && b_len > 0 && (unsigned char)b[b_len - 1] == min_charcode) {
    *err = "midpoint - a or b must not end with ' ' (space)";
    return NULL;
  }

  if (b) {
    int n = 0;
    while (n < b_len) {
      unsigned char ac = (n < a_len) ? (unsigned char)a[n] : min_charcode;
      unsigned char bc = (unsigned char)b[n];
      if (ac != bc) break;
      n++;
    }

    if (n > 0) {
      /* recurse with common prefix stripped */
      const char *sub_a = (n >= a_len) ? "" : a + n;
      int sub_a_len = (n >= a_len) ? 0 : a_len - n;
      const char *sub_b = (n >= b_len) ? NULL : b + n;
      int sub_b_len = (n >= b_len) ? 0 : b_len - n;
      char *suffix = midpoint(sub_a, sub_a_len, sub_b, sub_b_len, err);
      if (!suffix) return NULL;
      int suffix_len = (int)strlen(suffix);
      char *result = sqlite3_malloc(n + suffix_len + 1);
      if (!result) {
        sqlite3_free(suffix);
        *err = "out of memory";
        return NULL;
      }
      memcpy(result, b, n);
      memcpy(result + n, suffix, suffix_len);
      result[n + suffix_len] = '\0';
      sqlite3_free(suffix);
      return result;
    }
  }

  int da = (a_len > 0) ? digit_index((unsigned char)a[0]) : 0;
  if (da < 0) {
    *err = "midpoint - a has invalid digits";
    return NULL;
  }

  int db_val;
  if (b) {
    db_val = digit_index((unsigned char)b[0]);
    if (db_val < 0) {
      *err = "midpoint - b has invalid digits";
      return NULL;
    }
  } else {
    db_val = BASE_95_LEN;
  }

  if (db_val - da > 1) {
    int mid = fract_round(0.5 * (da + db_val));
    char *result = sqlite3_malloc(2);
    if (!result) {
      *err = "out of memory";
      return NULL;
    }
    result[0] = BASE_95_DIGITS[mid];
    result[1] = '\0';
    return result;
  } else {
    if (b && b_len > 1) {
      /* return first char of b */
      char *result = sqlite3_malloc(2);
      if (!result) {
        *err = "out of memory";
        return NULL;
      }
      result[0] = b[0];
      result[1] = '\0';
      return result;
    } else {
      /* recurse: digit_a + midpoint(a[1:], None) */
      const char *sub_a;
      int sub_a_len;
      if (a_len == 0) {
        sub_a = "";
        sub_a_len = 0;
      } else {
        sub_a = a + 1;
        sub_a_len = a_len - 1;
      }
      char *suffix = midpoint(sub_a, sub_a_len, NULL, 0, err);
      if (!suffix) return NULL;
      int suffix_len = (int)strlen(suffix);
      char *result = sqlite3_malloc(1 + suffix_len + 1);
      if (!result) {
        sqlite3_free(suffix);
        *err = "out of memory";
        return NULL;
      }
      result[0] = BASE_95_DIGITS[da];
      memcpy(result + 1, suffix, suffix_len);
      result[1 + suffix_len] = '\0';
      sqlite3_free(suffix);
      return result;
    }
  }
}

/**
 * Increment an integer key.
 * x, xlen: the integer part of a key.
 * Returns a sqlite3_malloc'd string on success.
 * Returns NULL if we cannot increment further (overflow from 'z'),
 * or on error (sets *err).
 * Sets *is_null = 1 if result is intentionally NULL (z overflow).
 */
static char *increment_integer(const char *x, int xlen, const char **err,
                               int *is_null) {
  *is_null = 0;
  if (validate_integer(x, xlen) != 0) {
    *err = "invalid integer part of order key";
    return NULL;
  }

  unsigned char head = (unsigned char)x[0];
  int digs_len = xlen - 1;

  /* copy digits, +2 for potential grow + NUL */
  char *digs = sqlite3_malloc(digs_len + 2);
  if (!digs) {
    *err = "out of memory";
    return NULL;
  }
  memcpy(digs, x + 1, digs_len);
  digs[digs_len] = '\0';

  int carry = 1;
  for (int i = digs_len - 1; carry && i >= 0; i--) {
    int idx = digit_index((unsigned char)digs[i]);
    if (idx < 0) {
      sqlite3_free(digs);
      *err = "invalid digit";
      return NULL;
    }
    int d = idx + 1;
    if (d == BASE_95_LEN) {
      digs[i] = BASE_95_DIGITS[0];
    } else {
      digs[i] = BASE_95_DIGITS[d];
      carry = 0;
    }
  }

  if (carry) {
    if (head == 'Z') {
      sqlite3_free(digs);
      int len = (int)strlen(INTEGER_ZERO);
      char *result = sqlite3_malloc(len + 1);
      if (!result) {
        *err = "out of memory";
        return NULL;
      }
      memcpy(result, INTEGER_ZERO, len + 1);
      return result;
    }
    if (head == 'z') {
      sqlite3_free(digs);
      *is_null = 1;
      return NULL;
    }
    unsigned char h = head + 1;
    int new_digs_len;
    if (h > a_charcode) {
      /* growing: append first digit */
      new_digs_len = digs_len + 1;
      digs[digs_len] = BASE_95_DIGITS[0];
      digs[digs_len + 1] = '\0';
    } else {
      /* shrinking: remove last digit */
      new_digs_len = digs_len - 1;
      digs[new_digs_len] = '\0';
    }
    char *result = sqlite3_malloc(1 + new_digs_len + 1);
    if (!result) {
      sqlite3_free(digs);
      *err = "out of memory";
      return NULL;
    }
    result[0] = (char)h;
    memcpy(result + 1, digs, new_digs_len);
    result[1 + new_digs_len] = '\0';
    sqlite3_free(digs);
    return result;
  } else {
    char *result = sqlite3_malloc(1 + digs_len + 1);
    if (!result) {
      sqlite3_free(digs);
      *err = "out of memory";
      return NULL;
    }
    result[0] = (char)head;
    memcpy(result + 1, digs, digs_len);
    result[1 + digs_len] = '\0';
    sqlite3_free(digs);
    return result;
  }
}

/**
 * Decrement an integer key.
 * Returns a sqlite3_malloc'd string on success.
 * Returns NULL on underflow (sets *is_null=1) or error (sets *err).
 */
static char *decrement_integer(const char *x, int xlen, const char **err,
                               int *is_null) {
  *is_null = 0;
  if (validate_integer(x, xlen) != 0) {
    *err = "invalid integer part of order key";
    return NULL;
  }

  unsigned char head = (unsigned char)x[0];
  int digs_len = xlen - 1;

  char *digs = sqlite3_malloc(digs_len + 2);
  if (!digs) {
    *err = "out of memory";
    return NULL;
  }
  memcpy(digs, x + 1, digs_len);
  digs[digs_len] = '\0';

  int borrow = 1;
  for (int i = digs_len - 1; borrow && i >= 0; i--) {
    int idx = digit_index((unsigned char)digs[i]);
    if (idx < 0) {
      sqlite3_free(digs);
      *err = "invalid digit";
      return NULL;
    }
    int d = idx - 1;
    if (d == -1) {
      digs[i] = BASE_95_DIGITS[BASE_95_LEN - 1];
    } else {
      digs[i] = BASE_95_DIGITS[d];
      borrow = 0;
    }
  }

  if (borrow) {
    if (head == 'a') {
      sqlite3_free(digs);
      /* Z + last digit */
      char *result = sqlite3_malloc(3);
      if (!result) {
        *err = "out of memory";
        return NULL;
      }
      result[0] = 'Z';
      result[1] = BASE_95_DIGITS[BASE_95_LEN - 1];
      result[2] = '\0';
      return result;
    }
    if (head == 'A') {
      sqlite3_free(digs);
      *is_null = 1;
      return NULL;
    }
    unsigned char h = head - 1;
    int new_digs_len;
    if (h < Z_charcode) {
      /* uppercase shrinking => growing: append last digit */
      new_digs_len = digs_len + 1;
      digs[digs_len] = BASE_95_DIGITS[BASE_95_LEN - 1];
      digs[digs_len + 1] = '\0';
    } else {
      /* shrinking */
      new_digs_len = digs_len - 1;
      digs[new_digs_len] = '\0';
    }
    char *result = sqlite3_malloc(1 + new_digs_len + 1);
    if (!result) {
      sqlite3_free(digs);
      *err = "out of memory";
      return NULL;
    }
    result[0] = (char)h;
    memcpy(result + 1, digs, new_digs_len);
    result[1 + new_digs_len] = '\0';
    sqlite3_free(digs);
    return result;
  } else {
    char *result = sqlite3_malloc(1 + digs_len + 1);
    if (!result) {
      sqlite3_free(digs);
      *err = "out of memory";
      return NULL;
    }
    result[0] = (char)head;
    memcpy(result + 1, digs, digs_len);
    result[1 + digs_len] = '\0';
    sqlite3_free(digs);
    return result;
  }
}

/**
 * Compute a key between a and b.
 * a and/or b may be NULL.
 * Returns a sqlite3_malloc'd string on success, NULL on error.
 * Sets *err on error.
 */
static char *key_between(const char *a, const char *b, const char **err) {
  int a_len = a ? (int)strlen(a) : 0;
  int b_len = b ? (int)strlen(b) : 0;

  /* validate inputs */
  if (a) {
    if (validate_order_key(a, a_len, err) != 0) return NULL;
  }
  if (b) {
    if (validate_order_key(b, b_len, err) != 0) return NULL;
  }

  /* (None, None) */
  if (!a && !b) {
    int len = (int)strlen(INTEGER_ZERO);
    char *result = sqlite3_malloc(len + 1);
    if (!result) {
      *err = "out of memory";
      return NULL;
    }
    memcpy(result, INTEGER_ZERO, len + 1);
    return result;
  }

  /* (Some(a), Some(b)) */
  if (a && b) {
    if (strcmp(a, b) >= 0) {
      *err = "key_between - a must be before b";
      return NULL;
    }
    int ia_len, ib_len;
    if (get_integer_part(a, a_len, &ia_len) != 0 ||
        get_integer_part(b, b_len, &ib_len) != 0) {
      *err = "head is out of range";
      return NULL;
    }

    const char *fa = a + ia_len;
    int fa_len = a_len - ia_len;
    const char *fb = b + ib_len;
    int fb_len = b_len - ib_len;

    /* same integer part */
    if (ia_len == ib_len && memcmp(a, b, ia_len) == 0) {
      char *mid = midpoint(fa, fa_len, fb, fb_len, err);
      if (!mid) return NULL;
      int mid_len = (int)strlen(mid);
      char *result = sqlite3_malloc(ia_len + mid_len + 1);
      if (!result) {
        sqlite3_free(mid);
        *err = "out of memory";
        return NULL;
      }
      memcpy(result, a, ia_len);
      memcpy(result + ia_len, mid, mid_len);
      result[ia_len + mid_len] = '\0';
      sqlite3_free(mid);
      return result;
    }

    /* different integer parts */
    int is_null = 0;
    char *inc = increment_integer(a, ia_len, err, &is_null);
    if (inc) {
      if (strcmp(inc, b) < 0) {
        return inc;
      }
      sqlite3_free(inc);
      /* fall through: use midpoint(fa, None) */
      char *mid = midpoint(fa, fa_len, NULL, 0, err);
      if (!mid) return NULL;
      int mid_len = (int)strlen(mid);
      char *result = sqlite3_malloc(ia_len + mid_len + 1);
      if (!result) {
        sqlite3_free(mid);
        *err = "out of memory";
        return NULL;
      }
      memcpy(result, a, ia_len);
      memcpy(result + ia_len, mid, mid_len);
      result[ia_len + mid_len] = '\0';
      sqlite3_free(mid);
      return result;
    } else if (is_null) {
      *err = "Cannot increment anymore";
      return NULL;
    } else {
      /* err is already set */
      return NULL;
    }
  }

  /* (None, Some(b)) */
  if (!a && b) {
    int ib_len;
    if (get_integer_part(b, b_len, &ib_len) != 0) {
      *err = "head is out of range";
      return NULL;
    }
    const char *fb = b + ib_len;
    int fb_len = b_len - ib_len;

    if (ib_len == SMALLEST_INTEGER_LEN &&
        memcmp(b, SMALLEST_INTEGER, ib_len) == 0) {
      char *mid = midpoint("", 0, fb, fb_len, err);
      if (!mid) return NULL;
      int mid_len = (int)strlen(mid);
      char *result = sqlite3_malloc(ib_len + mid_len + 1);
      if (!result) {
        sqlite3_free(mid);
        *err = "out of memory";
        return NULL;
      }
      memcpy(result, b, ib_len);
      memcpy(result + ib_len, mid, mid_len);
      result[ib_len + mid_len] = '\0';
      sqlite3_free(mid);
      return result;
    }

    /* if integer part < b (i.e., b has fractional part), return integer part */
    if (ib_len < b_len) {
      char *result = sqlite3_malloc(ib_len + 1);
      if (!result) {
        *err = "out of memory";
        return NULL;
      }
      memcpy(result, b, ib_len);
      result[ib_len] = '\0';
      return result;
    }

    int is_null = 0;
    char *dec = decrement_integer(b, ib_len, err, &is_null);
    if (dec) {
      return dec;
    } else if (is_null) {
      *err = "cannot decrement anymore";
      return NULL;
    } else {
      return NULL;
    }
  }

  /* (Some(a), None) */
  if (a && !b) {
    int ia_len;
    if (get_integer_part(a, a_len, &ia_len) != 0) {
      *err = "head is out of range";
      return NULL;
    }
    const char *fa = a + ia_len;
    int fa_len = a_len - ia_len;

    int is_null = 0;
    char *inc = increment_integer(a, ia_len, err, &is_null);
    if (!inc && !is_null) {
      /* error already set */
      return NULL;
    }
    if (inc) {
      return inc;
    }
    /* is_null: cannot increment, use midpoint */
    char *mid = midpoint(fa, fa_len, NULL, 0, err);
    if (!mid) return NULL;
    int mid_len = (int)strlen(mid);
    char *result = sqlite3_malloc(ia_len + mid_len + 1);
    if (!result) {
      sqlite3_free(mid);
      *err = "out of memory";
      return NULL;
    }
    memcpy(result, a, ia_len);
    memcpy(result + ia_len, mid, mid_len);
    result[ia_len + mid_len] = '\0';
    sqlite3_free(mid);
    return result;
  }

  *err = "unreachable";
  return NULL;
}

/* --------------------------------------------------------------------------
 * SQL function: crsql_fract_key_between(left, right)
 * -------------------------------------------------------------------------- */

void crsql_fract_key_between_fn(sqlite3_context *ctx, int argc,
                                sqlite3_value **argv) {
  (void)argc;
  const char *left = NULL;
  const char *right = NULL;

  if (sqlite3_value_type(argv[0]) != SQLITE_NULL) {
    left = (const char *)sqlite3_value_text(argv[0]);
  }
  if (sqlite3_value_type(argv[1]) != SQLITE_NULL) {
    right = (const char *)sqlite3_value_text(argv[1]);
  }

  const char *err = NULL;
  char *result = key_between(left, right, &err);
  if (result) {
    sqlite3_result_text(ctx, result, -1, sqlite3_free);
  } else if (err) {
    sqlite3_result_error(ctx, err, -1);
  } else {
    sqlite3_result_null(ctx);
  }
}

/* --------------------------------------------------------------------------
 * Utility functions for as_ordered / fractindex_view
 * -------------------------------------------------------------------------- */

/**
 * Build WHERE predicates: "col1" = NEW."col1" AND "col2" = NEW."col2" ...
 * If num_cols == 0, returns "1".
 * Returns a sqlite3_malloc'd string. Caller must sqlite3_free.
 */
static char *fract_where_predicates(const char **col_names, int num_cols) {
  if (num_cols == 0) {
    return sqlite3_mprintf("1");
  }
  char *result = sqlite3_mprintf("%s", "");
  for (int i = 0; i < num_cols; i++) {
    char *old = result;
    if (i == 0) {
      result = sqlite3_mprintf("\"%w\" = NEW.\"%w\"", col_names[i],
                               col_names[i]);
    } else {
      result = sqlite3_mprintf("%s AND \"%w\" = NEW.\"%w\"", old,
                               col_names[i], col_names[i]);
    }
    sqlite3_free(old);
  }
  return result;
}

/**
 * Build: SELECT MIN("order_col") FROM "table" WHERE <list_preds>
 *        AND "order_col" != -1 AND "order_col" != 1
 * Returns a sqlite3_malloc'd string. Caller must sqlite3_free.
 */
static char *fract_collection_min_select(const char *table,
                                         const char *order_col,
                                         const char **coll_cols,
                                         int num_coll_cols) {
  char *preds = fract_where_predicates(coll_cols, num_coll_cols);
  if (!preds) return NULL;
  char *result = sqlite3_mprintf(
      "SELECT MIN(\"%w\") FROM \"%w\" WHERE %s AND \"%w\" != -1 AND \"%w\" != 1",
      order_col, table, preds, order_col, order_col);
  sqlite3_free(preds);
  return result;
}

/**
 * Build: SELECT MAX("order_col") FROM "table" WHERE <list_preds>
 *        AND "order_col" != -1 AND "order_col" != 1
 */
static char *fract_collection_max_select(const char *table,
                                         const char *order_col,
                                         const char **coll_cols,
                                         int num_coll_cols) {
  char *preds = fract_where_predicates(coll_cols, num_coll_cols);
  if (!preds) return NULL;
  char *result = sqlite3_mprintf(
      "SELECT MAX(\"%w\") FROM \"%w\" WHERE %s AND \"%w\" != -1 AND \"%w\" != 1",
      order_col, table, preds, order_col, order_col);
  sqlite3_free(preds);
  return result;
}

/**
 * Extract primary key column names from pragma_table_info.
 * Sets *out_names to an array of sqlite3_malloc'd strings.
 * Sets *out_count to the number of PK columns.
 * Returns SQLITE_OK on success.
 * Caller must free each string and the array with sqlite3_free.
 */
static int fract_extract_pk_columns(sqlite3 *db, const char *table,
                                    char ***out_names, int *out_count) {
  *out_names = NULL;
  *out_count = 0;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(
      db,
      "SELECT \"name\" FROM pragma_table_info(?) WHERE \"pk\" > 0 ORDER BY "
      "\"pk\" ASC",
      -1, &stmt, NULL);
  if (rc != SQLITE_OK) return rc;
  rc = sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return rc;
  }

  int capacity = 4;
  int count = 0;
  char **names = sqlite3_malloc(capacity * (int)sizeof(char *));
  if (!names) {
    sqlite3_finalize(stmt);
    return SQLITE_NOMEM;
  }

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    if (count >= capacity) {
      capacity *= 2;
      char **tmp = sqlite3_realloc(names, capacity * (int)sizeof(char *));
      if (!tmp) {
        for (int i = 0; i < count; i++) sqlite3_free(names[i]);
        sqlite3_free(names);
        sqlite3_finalize(stmt);
        return SQLITE_NOMEM;
      }
      names = tmp;
    }
    int len = (int)strlen(name);
    names[count] = sqlite3_malloc(len + 1);
    if (!names[count]) {
      for (int i = 0; i < count; i++) sqlite3_free(names[i]);
      sqlite3_free(names);
      sqlite3_finalize(stmt);
      return SQLITE_NOMEM;
    }
    memcpy(names[count], name, len + 1);
    count++;
  }
  sqlite3_finalize(stmt);
  *out_names = names;
  *out_count = count;
  return SQLITE_OK;
}

/**
 * Extract all column names from pragma_table_info.
 * Same interface as fract_extract_pk_columns.
 */
static int fract_extract_columns(sqlite3 *db, const char *table,
                                 char ***out_names, int *out_count) {
  *out_names = NULL;
  *out_count = 0;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, "SELECT \"name\" FROM pragma_table_info(?)",
                              -1, &stmt, NULL);
  if (rc != SQLITE_OK) return rc;
  rc = sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return rc;
  }

  int capacity = 8;
  int count = 0;
  char **names = sqlite3_malloc(capacity * (int)sizeof(char *));
  if (!names) {
    sqlite3_finalize(stmt);
    return SQLITE_NOMEM;
  }

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    if (count >= capacity) {
      capacity *= 2;
      char **tmp = sqlite3_realloc(names, capacity * (int)sizeof(char *));
      if (!tmp) {
        for (int i = 0; i < count; i++) sqlite3_free(names[i]);
        sqlite3_free(names);
        sqlite3_finalize(stmt);
        return SQLITE_NOMEM;
      }
      names = tmp;
    }
    int len = (int)strlen(name);
    names[count] = sqlite3_malloc(len + 1);
    if (!names[count]) {
      for (int i = 0; i < count; i++) sqlite3_free(names[i]);
      sqlite3_free(names);
      sqlite3_finalize(stmt);
      return SQLITE_NOMEM;
    }
    memcpy(names[count], name, len + 1);
    count++;
  }
  sqlite3_finalize(stmt);
  *out_names = names;
  *out_count = count;
  return SQLITE_OK;
}

static void fract_free_string_array(char **arr, int count) {
  if (!arr) return;
  for (int i = 0; i < count; i++) {
    sqlite3_free(arr[i]);
  }
  sqlite3_free(arr);
}

/**
 * Check that all provided columns exist in the table.
 * Returns SQLITE_OK with *all_present = 1 if all exist, 0 if not.
 */
static int fract_table_has_all_columns(sqlite3 *db, const char *table,
                                       const char **col_names, int num_cols,
                                       int *all_present) {
  *all_present = 0;
  /* Build binding list: ?,?,? */
  char *bindings = sqlite3_malloc(num_cols * 3 + 1);
  if (!bindings) return SQLITE_NOMEM;
  int pos = 0;
  for (int i = 0; i < num_cols; i++) {
    if (i > 0) {
      bindings[pos++] = ',';
      bindings[pos++] = ' ';
    }
    bindings[pos++] = '?';
  }
  bindings[pos] = '\0';

  char *sql = sqlite3_mprintf(
      "SELECT count(*) FROM pragma_table_info(?) WHERE \"name\" IN (%s)",
      bindings);
  sqlite3_free(bindings);
  if (!sql) return SQLITE_NOMEM;

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return rc;

  rc = sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return rc;
  }

  for (int i = 0; i < num_cols; i++) {
    rc = sqlite3_bind_text(stmt, i + 2, col_names[i], -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
      sqlite3_finalize(stmt);
      return rc;
    }
  }

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    int count = sqlite3_column_int(stmt, 0);
    *all_present = (count == num_cols) ? 1 : 0;
  }
  sqlite3_finalize(stmt);
  return SQLITE_OK;
}

/* --------------------------------------------------------------------------
 * Trigger / view creation helpers
 * -------------------------------------------------------------------------- */

static int fract_create_pend_trigger(sqlite3 *db, const char *table,
                                     const char *order_col,
                                     const char **coll_cols,
                                     int num_coll_cols) {
  char *min_sel =
      fract_collection_min_select(table, order_col, coll_cols, num_coll_cols);
  char *max_sel =
      fract_collection_max_select(table, order_col, coll_cols, num_coll_cols);
  if (!min_sel || !max_sel) {
    sqlite3_free(min_sel);
    sqlite3_free(max_sel);
    return SQLITE_ERROR;
  }

  char *sql = sqlite3_mprintf(
      "CREATE TRIGGER IF NOT EXISTS \"__crsql_%w_fractindex_pend_trig\" "
      "AFTER INSERT ON \"%w\" "
      "WHEN CAST(NEW.\"%w\" AS INTEGER) = -1 "
      "OR CAST(NEW.\"%w\" AS INTEGER) = 1 BEGIN "
      "UPDATE \"%w\" SET \"%w\" = CASE CAST(NEW.\"%w\" AS INTEGER) "
      "WHEN -1 THEN crsql_fract_key_between(NULL, (%s)) "
      "WHEN 1 THEN crsql_fract_key_between((%s), NULL) "
      "END "
      "WHERE _rowid_ = NEW._rowid_; "
      "END;",
      table, table, order_col, order_col, table, order_col, order_col, min_sel,
      max_sel);
  sqlite3_free(min_sel);
  sqlite3_free(max_sel);
  if (!sql) return SQLITE_NOMEM;

  int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  sqlite3_free(sql);
  return rc;
}

static int fract_create_simple_move_trigger(sqlite3 *db, const char *table,
                                            const char *order_col,
                                            const char **coll_cols,
                                            int num_coll_cols) {
  char *min_sel =
      fract_collection_min_select(table, order_col, coll_cols, num_coll_cols);
  char *max_sel =
      fract_collection_max_select(table, order_col, coll_cols, num_coll_cols);
  if (!min_sel || !max_sel) {
    sqlite3_free(min_sel);
    sqlite3_free(max_sel);
    return SQLITE_ERROR;
  }

  char *sql = sqlite3_mprintf(
      "CREATE TRIGGER IF NOT EXISTS \"__crsql_%w_fractindex_ezmove\" "
      "AFTER UPDATE OF \"%w\" ON \"%w\" "
      "WHEN NEW.\"%w\" = -1 OR NEW.\"%w\" = 1 BEGIN "
      "UPDATE \"%w\" SET \"%w\" = CASE NEW.\"%w\" "
      "WHEN -1 THEN crsql_fract_key_between(NULL, (%s)) "
      "WHEN 1 THEN crsql_fract_key_between((%s), NULL) "
      "END "
      "WHERE _rowid_ = NEW._rowid_; "
      "END;",
      table, order_col, table, order_col, order_col, table, order_col,
      order_col, min_sel, max_sel);
  sqlite3_free(min_sel);
  sqlite3_free(max_sel);
  if (!sql) return SQLITE_NOMEM;

  int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  sqlite3_free(sql);
  return rc;
}

/**
 * Build the "common inputs" used by both insert and update view triggers.
 * Outputs are sqlite3_malloc'd strings. Caller must sqlite3_free each.
 * Returns SQLITE_OK on success.
 */
static int fract_create_common_inputs(sqlite3 *db, const char *table,
                                      const char **coll_cols, int num_coll_cols,
                                      char **out_after_pk_values,
                                      char **out_list_predicates,
                                      char **out_after_predicates,
                                      char **out_list_name_args,
                                      char **out_pk_name_args) {
  char **pks = NULL;
  int pk_count = 0;
  int rc = fract_extract_pk_columns(db, table, &pks, &pk_count);
  if (rc != SQLITE_OK) return rc;

  /* after_pk_values: NEW."after_pk1", NEW."after_pk2", ... */
  char *after_pk_values = sqlite3_mprintf("%s", "");
  for (int i = 0; i < pk_count; i++) {
    char *old = after_pk_values;
    if (i == 0) {
      after_pk_values = sqlite3_mprintf("NEW.\"after_%w\"", pks[i]);
    } else {
      after_pk_values =
          sqlite3_mprintf("%s, NEW.\"after_%w\"", old, pks[i]);
    }
    sqlite3_free(old);
  }

  /* pk_name_args: 'pk1', 'pk2', ... (single-quote escaped) */
  char *pk_name_args = sqlite3_mprintf("%s", "");
  for (int i = 0; i < pk_count; i++) {
    char *esc = fract_escape_arg(pks[i]);
    char *old = pk_name_args;
    if (i == 0) {
      pk_name_args = sqlite3_mprintf("'%s'", esc);
    } else {
      pk_name_args = sqlite3_mprintf("%s, '%s'", old, esc);
    }
    sqlite3_free(esc);
    sqlite3_free(old);
  }

  /* list_predicates: "col" = NEW."col" AND ... or "1" if empty */
  char *list_predicates = fract_where_predicates(coll_cols, num_coll_cols);

  /* after_predicates: "pk" = NEW."after_pk" AND ... */
  char *after_predicates = sqlite3_mprintf("%s", "");
  for (int i = 0; i < pk_count; i++) {
    char *old = after_predicates;
    if (i == 0) {
      after_predicates =
          sqlite3_mprintf("\"%w\" = NEW.\"after_%w\"", pks[i], pks[i]);
    } else {
      after_predicates = sqlite3_mprintf("%s AND \"%w\" = NEW.\"after_%w\"",
                                         old, pks[i], pks[i]);
    }
    sqlite3_free(old);
  }

  /* list_name_args: 'col1', 'col2', ... */
  char *list_name_args = sqlite3_mprintf("%s", "");
  for (int i = 0; i < num_coll_cols; i++) {
    char *esc = fract_escape_arg(coll_cols[i]);
    char *old = list_name_args;
    if (i == 0) {
      list_name_args = sqlite3_mprintf("'%s'", esc);
    } else {
      list_name_args = sqlite3_mprintf("%s, '%s'", old, esc);
    }
    sqlite3_free(esc);
    sqlite3_free(old);
  }

  fract_free_string_array(pks, pk_count);

  *out_after_pk_values = after_pk_values;
  *out_list_predicates = list_predicates;
  *out_after_predicates = after_predicates;
  *out_list_name_args = list_name_args;
  *out_pk_name_args = pk_name_args;
  return SQLITE_OK;
}

static int fract_create_instead_of_insert_trigger(sqlite3 *db,
                                                  const char *table,
                                                  const char *order_col,
                                                  const char **coll_cols,
                                                  int num_coll_cols) {
  /* Extract columns */
  char **columns = NULL;
  int num_columns = 0;
  int rc = fract_extract_columns(db, table, &columns, &num_columns);
  if (rc != SQLITE_OK) return rc;

  /* Build col_names_ex_order and col_values_ex_order */
  char *col_names_ex_order = sqlite3_mprintf("%s", "");
  char *col_values_ex_order = sqlite3_mprintf("%s", "");
  int first = 1;
  for (int i = 0; i < num_columns; i++) {
    if (strcmp(columns[i], order_col) == 0) continue;
    char *old_names = col_names_ex_order;
    char *old_values = col_values_ex_order;
    if (first) {
      col_names_ex_order = sqlite3_mprintf("\"%w\"", columns[i]);
      col_values_ex_order = sqlite3_mprintf("NEW.\"%w\"", columns[i]);
      first = 0;
    } else {
      col_names_ex_order =
          sqlite3_mprintf("%s, \"%w\"", old_names, columns[i]);
      col_values_ex_order =
          sqlite3_mprintf("%s, NEW.\"%w\"", old_values, columns[i]);
    }
    sqlite3_free(old_names);
    sqlite3_free(old_values);
  }
  fract_free_string_array(columns, num_columns);

  /* Common inputs */
  char *after_pk_values = NULL, *list_predicates = NULL;
  char *after_predicates = NULL, *list_name_args = NULL, *pk_name_args = NULL;
  rc = fract_create_common_inputs(db, table, coll_cols, num_coll_cols,
                                  &after_pk_values, &list_predicates,
                                  &after_predicates, &list_name_args,
                                  &pk_name_args);
  if (rc != SQLITE_OK) {
    sqlite3_free(col_names_ex_order);
    sqlite3_free(col_values_ex_order);
    return rc;
  }

  char *table_arg = fract_escape_arg(table);
  char *order_col_arg = fract_escape_arg(order_col);
  int has_list_args = (num_coll_cols > 0);
  const char *maybe_comma = has_list_args ? ", " : "";

  char *sql = sqlite3_mprintf(
      "CREATE TRIGGER IF NOT EXISTS \"%w_fractindex_insert_trig\" "
      "INSTEAD OF INSERT ON \"%w_fractindex\" "
      "BEGIN "
      "INSERT INTO \"%w\" "
      "(%s, \"%w\") "
      "VALUES "
      "(%s, "
      "CASE ("
      "SELECT count(*) FROM \"%w\" WHERE %s AND \"%w\" = "
      "(SELECT \"%w\" FROM \"%w\" WHERE %s)"
      ") "
      "WHEN 1 THEN crsql_fract_key_between("
      "(SELECT \"%w\" FROM \"%w\" WHERE %s), "
      "(SELECT \"%w\" FROM \"%w\" WHERE %s AND \"%w\" > "
      "(SELECT \"%w\" FROM \"%w\" WHERE %s) "
      "ORDER BY \"%w\" ASC LIMIT 1)"
      ") "
      "WHEN 0 THEN -1 "
      "ELSE crsql_fract_fix_conflict_return_old_key("
      "'%s', '%s', %s%s -1, %s, %s"
      ") "
      "END"
      "); "
      "END;",
      table, table, table, col_names_ex_order, order_col, col_values_ex_order,
      /* CASE subquery */
      table, list_predicates, order_col, order_col, table, after_predicates,
      /* WHEN 1 */
      order_col, table, after_predicates, order_col, table, list_predicates,
      order_col, order_col, table, after_predicates, order_col,
      /* ELSE */
      table_arg, order_col_arg, list_name_args, maybe_comma, pk_name_args,
      after_pk_values);

  sqlite3_free(col_names_ex_order);
  sqlite3_free(col_values_ex_order);
  sqlite3_free(after_pk_values);
  sqlite3_free(list_predicates);
  sqlite3_free(after_predicates);
  sqlite3_free(list_name_args);
  sqlite3_free(pk_name_args);
  sqlite3_free(table_arg);
  sqlite3_free(order_col_arg);

  if (!sql) return SQLITE_NOMEM;

  sqlite3_stmt *pStmt = NULL;
  rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return rc;
  rc = sqlite3_step(pStmt);
  sqlite3_finalize(pStmt);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) return rc;
  return SQLITE_OK;
}

static int fract_create_instead_of_update_trigger(sqlite3 *db,
                                                  const char *table,
                                                  const char *order_col,
                                                  const char **coll_cols,
                                                  int num_coll_cols) {
  char **columns = NULL;
  int num_columns = 0;
  int rc = fract_extract_columns(db, table, &columns, &num_columns);
  if (rc != SQLITE_OK) return rc;

  /* base_sets_ex_order: "col" = NEW."col", ... */
  char *base_sets = sqlite3_mprintf("%s", "");
  int first = 1;
  for (int i = 0; i < num_columns; i++) {
    if (strcmp(columns[i], order_col) == 0) continue;
    char *old = base_sets;
    if (first) {
      base_sets =
          sqlite3_mprintf("\"%w\" = NEW.\"%w\"", columns[i], columns[i]);
      first = 0;
    } else {
      base_sets = sqlite3_mprintf("%s,\n\"%w\" = NEW.\"%w\"", old, columns[i],
                                  columns[i]);
    }
    sqlite3_free(old);
  }
  fract_free_string_array(columns, num_columns);

  /* PK predicates for WHERE clause: "pk" = NEW."pk" */
  char **pks = NULL;
  int pk_count = 0;
  rc = fract_extract_pk_columns(db, table, &pks, &pk_count);
  if (rc != SQLITE_OK) {
    sqlite3_free(base_sets);
    return rc;
  }
  char *pk_predicates = sqlite3_mprintf("%s", "");
  for (int i = 0; i < pk_count; i++) {
    char *old = pk_predicates;
    if (i == 0) {
      pk_predicates =
          sqlite3_mprintf("\"%w\" = NEW.\"%w\"", pks[i], pks[i]);
    } else {
      pk_predicates = sqlite3_mprintf("%s AND \"%w\" = NEW.\"%w\"", old,
                                      pks[i], pks[i]);
    }
    sqlite3_free(old);
  }
  fract_free_string_array(pks, pk_count);

  /* Common inputs */
  char *after_pk_values = NULL, *list_predicates = NULL;
  char *after_predicates = NULL, *list_name_args = NULL, *pk_name_args = NULL;
  rc = fract_create_common_inputs(db, table, coll_cols, num_coll_cols,
                                  &after_pk_values, &list_predicates,
                                  &after_predicates, &list_name_args,
                                  &pk_name_args);
  if (rc != SQLITE_OK) {
    sqlite3_free(base_sets);
    sqlite3_free(pk_predicates);
    return rc;
  }

  char *table_arg = fract_escape_arg(table);
  char *order_col_arg = fract_escape_arg(order_col);
  const char *maybe_comma = (num_coll_cols > 0) ? ", " : "";

  char *sql = sqlite3_mprintf(
      "CREATE TRIGGER IF NOT EXISTS \"%w_fractindex_update_trig\" "
      "INSTEAD OF UPDATE ON \"%w_fractindex\" "
      "BEGIN "
      "UPDATE \"%w\" SET "
      "%s, "
      "\"%w\" = CASE ("
      "SELECT count(*) FROM \"%w\" WHERE %s AND \"%w\" = ("
      "SELECT \"%w\" FROM \"%w\" WHERE %s"
      ")"
      ") "
      "WHEN 1 THEN crsql_fract_key_between("
      "(SELECT \"%w\" FROM \"%w\" WHERE %s), "
      "(SELECT \"%w\" FROM \"%w\" WHERE %s AND \"%w\" > ("
      "SELECT \"%w\" FROM \"%w\" WHERE %s"
      ") ORDER BY \"%w\" ASC LIMIT 1)"
      ") "
      "WHEN 0 THEN -1 "
      "ELSE crsql_fract_fix_conflict_return_old_key("
      "'%s', '%s', %s%s -1, %s, %s"
      ") "
      "END "
      "WHERE %s; "
      "END;",
      table, table, table, base_sets, order_col,
      /* CASE subquery */
      table, list_predicates, order_col, order_col, table, after_predicates,
      /* WHEN 1 */
      order_col, table, after_predicates, order_col, table, list_predicates,
      order_col, order_col, table, after_predicates, order_col,
      /* ELSE */
      table_arg, order_col_arg, list_name_args, maybe_comma, pk_name_args,
      after_pk_values,
      /* WHERE */
      pk_predicates);

  sqlite3_free(base_sets);
  sqlite3_free(pk_predicates);
  sqlite3_free(after_pk_values);
  sqlite3_free(list_predicates);
  sqlite3_free(after_predicates);
  sqlite3_free(list_name_args);
  sqlite3_free(pk_name_args);
  sqlite3_free(table_arg);
  sqlite3_free(order_col_arg);

  if (!sql) return SQLITE_NOMEM;

  sqlite3_stmt *pStmt = NULL;
  rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return rc;
  rc = sqlite3_step(pStmt);
  sqlite3_finalize(pStmt);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) return rc;
  return SQLITE_OK;
}

static int fract_create_view_and_triggers(sqlite3 *db, const char *table,
                                          const char *order_col,
                                          const char **coll_cols,
                                          int num_coll_cols) {
  char **pks = NULL;
  int pk_count = 0;
  int rc = fract_extract_pk_columns(db, table, &pks, &pk_count);
  if (rc != SQLITE_OK) return rc;

  /* Build after_pk_defs: NULL AS "after_pk1", NULL AS "after_pk2", ... */
  char *after_pk_defs = sqlite3_mprintf("%s", "");
  for (int i = 0; i < pk_count; i++) {
    char *old = after_pk_defs;
    if (i == 0) {
      after_pk_defs = sqlite3_mprintf("NULL AS \"after_%w\"", pks[i]);
    } else {
      after_pk_defs =
          sqlite3_mprintf("%s, NULL AS \"after_%w\"", old, pks[i]);
    }
    sqlite3_free(old);
  }
  fract_free_string_array(pks, pk_count);

  char *sql = sqlite3_mprintf(
      "CREATE VIEW IF NOT EXISTS \"%w_fractindex\" AS "
      "SELECT *, %s FROM \"%w\"",
      table, after_pk_defs, table);
  sqlite3_free(after_pk_defs);
  if (!sql) return SQLITE_NOMEM;

  rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return rc;

  rc = fract_create_instead_of_insert_trigger(db, table, order_col, coll_cols,
                                              num_coll_cols);
  if (rc != SQLITE_OK) return rc;

  rc = fract_create_instead_of_update_trigger(db, table, order_col, coll_cols,
                                              num_coll_cols);
  return rc;
}

/* --------------------------------------------------------------------------
 * SQL function: crsql_fract_as_ordered(table, order_col, coll_col1, ...)
 * -------------------------------------------------------------------------- */

void crsql_fract_as_ordered_fn(sqlite3_context *ctx, int argc,
                               sqlite3_value **argv) {
  if (argc < 2) {
    sqlite3_result_error(
        ctx,
        "Must provide at least 2 arguments -- the table name and the column "
        "to order by",
        -1);
    return;
  }

  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const char *table = (const char *)sqlite3_value_text(argv[0]);
  const char *order_col = (const char *)sqlite3_value_text(argv[1]);
  if (!table || !order_col) {
    sqlite3_result_error(ctx, "table and order_col must be non-NULL", -1);
    return;
  }

  int num_coll_cols = argc - 2;
  const char **coll_cols = NULL;
  if (num_coll_cols > 0) {
    coll_cols = (const char **)sqlite3_malloc(num_coll_cols * (int)sizeof(char *));
    if (!coll_cols) {
      sqlite3_result_error_nomem(ctx);
      return;
    }
    for (int i = 0; i < num_coll_cols; i++) {
      coll_cols[i] = (const char *)sqlite3_value_text(argv[i + 2]);
      if (!coll_cols[i]) {
        sqlite3_free(coll_cols);
        sqlite3_result_error(ctx, "Collection column name must be non-NULL",
                             -1);
        return;
      }
    }
  }

  int rc;

  /* Drop prior triggers/views */
  char *drop_trig = sqlite3_mprintf(
      "DROP TRIGGER IF EXISTS \"__crsql_%w_fractindex_pend_trig\"", table);
  rc = sqlite3_exec(db, drop_trig, NULL, NULL, NULL);
  sqlite3_free(drop_trig);
  if (rc != SQLITE_OK) {
    sqlite3_free(coll_cols);
    sqlite3_result_error(
        ctx, "Failed dropping prior incarnation of fractindex triggers", -1);
    return;
  }

  char *drop_view =
      sqlite3_mprintf("DROP VIEW IF EXISTS \"%w_fractindex\"", table);
  rc = sqlite3_exec(db, drop_view, NULL, NULL, NULL);
  sqlite3_free(drop_view);
  if (rc != SQLITE_OK) {
    sqlite3_free(coll_cols);
    sqlite3_result_error(
        ctx, "Failed dropping prior incarnation of fractindex views", -1);
    return;
  }

  /* Validate columns exist */
  int total_cols = num_coll_cols + 1; /* collection + order */
  const char **all_cols =
      (const char **)sqlite3_malloc(total_cols * (int)sizeof(char *));
  if (!all_cols) {
    sqlite3_free(coll_cols);
    sqlite3_result_error_nomem(ctx);
    return;
  }
  for (int i = 0; i < num_coll_cols; i++) {
    all_cols[i] = coll_cols[i];
  }
  all_cols[num_coll_cols] = order_col;

  int all_present = 0;
  rc = fract_table_has_all_columns(db, table, all_cols, total_cols,
                                   &all_present);
  sqlite3_free(all_cols);
  if (rc != SQLITE_OK) {
    sqlite3_free(coll_cols);
    sqlite3_result_error(
        ctx, "Failed determining if all columns are present in the base table",
        -1);
    return;
  }
  if (!all_present) {
    sqlite3_free(coll_cols);
    sqlite3_result_error(ctx,
                         "all columns are not present in the base table", -1);
    return;
  }

  /* SAVEPOINT */
  rc = sqlite3_exec(db, "SAVEPOINT as_ordered;", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_free(coll_cols);
    return;
  }

  rc = fract_create_pend_trigger(db, table, order_col, coll_cols,
                                 num_coll_cols);
  if (rc != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK TO as_ordered;", NULL, NULL, NULL);
    sqlite3_exec(db, "RELEASE as_ordered;", NULL, NULL, NULL);
    sqlite3_free(coll_cols);
    sqlite3_result_error(ctx, "Failed creating triggers for the base table",
                         -1);
    return;
  }

  rc = fract_create_simple_move_trigger(db, table, order_col, coll_cols,
                                        num_coll_cols);
  if (rc != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK TO as_ordered;", NULL, NULL, NULL);
    sqlite3_exec(db, "RELEASE as_ordered;", NULL, NULL, NULL);
    sqlite3_free(coll_cols);
    sqlite3_result_error(ctx, "Failed creating simple move trigger", -1);
    return;
  }

  rc = fract_create_view_and_triggers(db, table, order_col, coll_cols,
                                      num_coll_cols);
  if (rc != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK TO as_ordered;", NULL, NULL, NULL);
    sqlite3_exec(db, "RELEASE as_ordered;", NULL, NULL, NULL);
    sqlite3_free(coll_cols);
    sqlite3_result_error(ctx, "Failed creating view for the base table", -1);
    return;
  }

  sqlite3_exec(db, "RELEASE as_ordered;", NULL, NULL, NULL);
  sqlite3_free(coll_cols);
}

/* --------------------------------------------------------------------------
 * fix_conflict_return_old_key implementation
 * -------------------------------------------------------------------------- */

/**
 * Internal implementation of fix_conflict_return_old_key.
 * Returns SQLITE_OK on success (sets result on ctx), or an error code.
 */
static int fract_fix_conflict_return_old_key(sqlite3_context *ctx,
                                             const char *table,
                                             const char *order_col,
                                             const char **coll_col_names,
                                             int num_coll_cols,
                                             const char **pk_names,
                                             sqlite3_value **pk_values,
                                             int pk_count) {
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  int rc;

  /* Build pk predicates: "pk1" = ?1, AND "pk2" = ?2 */
  char *pk_predicates = sqlite3_mprintf("%s", "");
  for (int i = 0; i < pk_count; i++) {
    char *old = pk_predicates;
    if (i == 0) {
      pk_predicates = sqlite3_mprintf("\"%w\" = ?%d", pk_names[i], i + 1);
    } else {
      pk_predicates = sqlite3_mprintf("%s AND \"%w\" = ?%d", old, pk_names[i],
                                      i + 1);
    }
    sqlite3_free(old);
  }

  /* SELECT order_col FROM table WHERE pk_predicates */
  char *sel_sql = sqlite3_mprintf("SELECT \"%w\" FROM \"%w\" WHERE %s",
                                  order_col, table, pk_predicates);
  if (!sel_sql) {
    sqlite3_free(pk_predicates);
    return SQLITE_NOMEM;
  }

  sqlite3_stmt *sel_stmt = NULL;
  rc = sqlite3_prepare_v2(db, sel_sql, -1, &sel_stmt, NULL);
  sqlite3_free(sel_sql);
  if (rc != SQLITE_OK) {
    sqlite3_free(pk_predicates);
    return rc;
  }
  for (int i = 0; i < pk_count; i++) {
    sqlite3_bind_value(sel_stmt, i + 1, pk_values[i]);
  }
  rc = sqlite3_step(sel_stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(sel_stmt);
    sqlite3_free(pk_predicates);
    return SQLITE_ERROR;
  }

  /* Copy target_order before finalizing */
  const char *target_order_raw =
      (const char *)sqlite3_column_text(sel_stmt, 0);
  char *target_order_copy = NULL;
  if (target_order_raw) {
    int tolen = (int)strlen(target_order_raw);
    target_order_copy = sqlite3_malloc(tolen + 1);
    if (target_order_copy)
      memcpy(target_order_copy, target_order_raw, tolen + 1);
  }
  sqlite3_finalize(sel_stmt);

  if (!target_order_copy) {
    sqlite3_free(pk_predicates);
    return SQLITE_ERROR;
  }

  /* Build JOIN clause for collection columns if any */
  char *maybe_join = sqlite3_mprintf("%s", "");
  if (num_coll_cols > 0) {
    char *list_columns = sqlite3_mprintf("%s", "");
    for (int i = 0; i < num_coll_cols; i++) {
      char *old = list_columns;
      if (i == 0) {
        list_columns = sqlite3_mprintf("\"%w\"", coll_col_names[i]);
      } else {
        list_columns =
            sqlite3_mprintf("%s, \"%w\"", old, coll_col_names[i]);
      }
      sqlite3_free(old);
    }
    char *list_join_preds = sqlite3_mprintf("%s", "");
    for (int i = 0; i < num_coll_cols; i++) {
      char *old = list_join_preds;
      if (i == 0) {
        list_join_preds = sqlite3_mprintf("\"%w\".\"%w\" = t.\"%w\"", table,
                                          coll_col_names[i],
                                          coll_col_names[i]);
      } else {
        list_join_preds = sqlite3_mprintf(
            "%s AND \"%w\".\"%w\" = t.\"%w\"", old, table, coll_col_names[i],
            coll_col_names[i]);
      }
      sqlite3_free(old);
    }
    sqlite3_free(maybe_join);
    maybe_join = sqlite3_mprintf(
        "JOIN (SELECT %s FROM \"%w\" WHERE %s) as t ON %s", list_columns,
        table, pk_predicates, list_join_preds);
    sqlite3_free(list_columns);
    sqlite3_free(list_join_preds);
  }

  int target_slot = pk_count + 1;

  /* UPDATE ... SET order_col = key_between(...)
   * WHERE pk_predicates RETURNING order_col */
  char *upd_sql = sqlite3_mprintf(
      "UPDATE \"%w\" SET \"%w\" = crsql_fract_key_between("
      "(SELECT \"%w\" FROM \"%w\" %s WHERE \"%w\" < ?%d "
      "ORDER BY \"%w\" DESC LIMIT 1), "
      "?%d"
      ") WHERE %s RETURNING \"%w\"",
      table, order_col, order_col, table, maybe_join, order_col, target_slot,
      order_col, target_slot, pk_predicates, order_col);

  sqlite3_free(maybe_join);
  sqlite3_free(pk_predicates);

  if (!upd_sql) {
    sqlite3_free(target_order_copy);
    return SQLITE_NOMEM;
  }

  sqlite3_stmt *upd_stmt = NULL;
  rc = sqlite3_prepare_v2(db, upd_sql, -1, &upd_stmt, NULL);
  sqlite3_free(upd_sql);
  if (rc != SQLITE_OK) {
    sqlite3_free(target_order_copy);
    return rc;
  }

  for (int i = 0; i < pk_count; i++) {
    sqlite3_bind_value(upd_stmt, i + 1, pk_values[i]);
  }
  sqlite3_bind_text(upd_stmt, target_slot, target_order_copy, -1,
                    SQLITE_TRANSIENT);

  rc = sqlite3_step(upd_stmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    sqlite3_finalize(upd_stmt);
    sqlite3_free(target_order_copy);
    return SQLITE_ERROR;
  }

  const char *new_target_order =
      (const char *)sqlite3_column_text(upd_stmt, 0);

  /* key_between(new_target_order, target_order_copy) */
  const char *kb_err = NULL;
  char *ret = key_between(new_target_order, target_order_copy, &kb_err);
  sqlite3_finalize(upd_stmt);
  sqlite3_free(target_order_copy);

  if (ret) {
    sqlite3_result_text(ctx, ret, -1, sqlite3_free);
    return SQLITE_OK;
  }
  return SQLITE_ERROR;
}

/* --------------------------------------------------------------------------
 * SQL function: crsql_fract_fix_conflict_return_old_key(...)
 * -------------------------------------------------------------------------- */

/**
 * Count collection column names from args starting at `from`.
 * Collection columns are text args until we hit an integer (-1 sentinel).
 */
static int pull_collection_column_count(int from, int argc,
                                        sqlite3_value **argv) {
  int i = from;
  while (i < argc) {
    if (sqlite3_value_type(argv[i]) == SQLITE_INTEGER) {
      break;
    }
    i++;
  }
  return i - from;
}

void crsql_fract_fix_conflict_return_old_key_fn(sqlite3_context *ctx, int argc,
                                                sqlite3_value **argv) {
  if (argc < 4) {
    sqlite3_result_error(
        ctx, "Too few arguments to fix_conflict_return_old_key", -1);
    return;
  }

  const char *table = (const char *)sqlite3_value_text(argv[0]);
  const char *order_col = (const char *)sqlite3_value_text(argv[1]);
  if (!table || !order_col) {
    sqlite3_result_error(ctx, "table and order_col must be non-NULL", -1);
    return;
  }

  int coll_count = pull_collection_column_count(2, argc, argv);
  /* next_index = 2 + coll_count + 1 (for the -1 separator) */
  int next_index = 2 + coll_count + 1;

  int pk_and_val_count = argc - next_index;
  if (pk_and_val_count <= 0 || pk_and_val_count % 2 != 0) {
    sqlite3_result_error(
        ctx,
        "Incorrect number of primary keys and values provided. "
        "Must have at least 1 primary key.",
        -1);
    return;
  }

  int pk_count = pk_and_val_count / 2;

  /* Gather collection column names */
  const char **coll_col_names = NULL;
  if (coll_count > 0) {
    coll_col_names =
        (const char **)sqlite3_malloc(coll_count * (int)sizeof(char *));
    if (!coll_col_names) {
      sqlite3_result_error_nomem(ctx);
      return;
    }
    for (int i = 0; i < coll_count; i++) {
      coll_col_names[i] = (const char *)sqlite3_value_text(argv[2 + i]);
    }
  }

  /* PK names and values */
  const char **pk_names =
      (const char **)sqlite3_malloc(pk_count * (int)sizeof(char *));
  sqlite3_value **pk_values_arr =
      (sqlite3_value **)sqlite3_malloc(pk_count * (int)sizeof(sqlite3_value *));
  if (!pk_names || !pk_values_arr) {
    sqlite3_free(coll_col_names);
    sqlite3_free(pk_names);
    sqlite3_free(pk_values_arr);
    sqlite3_result_error_nomem(ctx);
    return;
  }
  for (int i = 0; i < pk_count; i++) {
    pk_names[i] = (const char *)sqlite3_value_text(argv[next_index + i]);
    pk_values_arr[i] = argv[next_index + pk_count + i];
  }

  int rc = fract_fix_conflict_return_old_key(ctx, table, order_col,
                                             coll_col_names, coll_count,
                                             pk_names, pk_values_arr,
                                             pk_count);
  sqlite3_free(coll_col_names);
  sqlite3_free(pk_names);
  sqlite3_free(pk_values_arr);

  if (rc != SQLITE_OK) {
    sqlite3_result_error(ctx,
                         "Failed fixing up ordering conflicts on insert", -1);
  }
}

/* --------------------------------------------------------------------------
 * Registration
 * -------------------------------------------------------------------------- */

int crsql_init_fractional_index(sqlite3 *db) {
  int rc;

  rc = sqlite3_create_function_v2(db, "crsql_fract_as_ordered", -1,
                                  SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL,
                                  crsql_fract_as_ordered_fn, NULL, NULL, NULL);
  if (rc != SQLITE_OK) return rc;

  rc = sqlite3_create_function_v2(db, "crsql_fract_key_between", 2,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC |
                                      SQLITE_INNOCUOUS,
                                  NULL, crsql_fract_key_between_fn, NULL, NULL,
                                  NULL);
  if (rc != SQLITE_OK) return rc;

  rc = sqlite3_create_function_v2(
      db, "crsql_fract_fix_conflict_return_old_key", -1,
      SQLITE_UTF8 | SQLITE_INNOCUOUS, NULL,
      crsql_fract_fix_conflict_return_old_key_fn, NULL, NULL, NULL);
  return rc;
}
