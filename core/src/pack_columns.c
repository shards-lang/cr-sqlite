#include "pack_columns.h"
#include <string.h>
#include <stdint.h>

// Helper: Calculate minimum bytes needed for i32
static unsigned char num_bytes_needed_i32(int32_t val) {
  if ((val & 0xFF000000) != 0) return 4;
  if ((val & 0x00FF0000) != 0) return 3;
  if ((val & 0x0000FF00) != 0) return 2;
  if ((val & 0x000000FF) != 0) return 1;
  return 0;
}

// Helper: Calculate minimum bytes needed for i64
static unsigned char num_bytes_needed_i64(int64_t val) {
  if ((val & 0xFF00000000000000LL) != 0) return 8;
  if ((val & 0x00FF000000000000LL) != 0) return 7;
  if ((val & 0x0000FF0000000000LL) != 0) return 6;
  if ((val & 0x000000FF00000000LL) != 0) return 5;
  return num_bytes_needed_i32((int32_t)val);
}

// Helper: Write integer in specific number of bytes (little-endian)
static void put_int(unsigned char **buf, int64_t val, int num_bytes) {
  for (int i = 0; i < num_bytes; i++) {
    **buf = (unsigned char)((val >> (i * 8)) & 0xFF);
    (*buf)++;
  }
}

// Helper: Read integer in specific number of bytes (little-endian)
static int64_t get_int(const unsigned char **buf, int num_bytes) {
  int64_t result = 0;
  for (int i = 0; i < num_bytes; i++) {
    result |= ((int64_t)(**buf)) << (i * 8);
    (*buf)++;
  }
  return result;
}

// Helper: Write double (8 bytes, little-endian)
static void put_double(unsigned char **buf, double val) {
  uint64_t bits;
  memcpy(&bits, &val, sizeof(double));
  for (int i = 0; i < 8; i++) {
    **buf = (unsigned char)((bits >> (i * 8)) & 0xFF);
    (*buf)++;
  }
}

// Helper: Read double (8 bytes, little-endian)
static double get_double(const unsigned char **buf) {
  uint64_t bits = 0;
  for (int i = 0; i < 8; i++) {
    bits |= ((uint64_t)(**buf)) << (i * 8);
    (*buf)++;
  }
  double result;
  memcpy(&result, &bits, sizeof(double));
  return result;
}

// Pack columns into binary blob
void crsql_pack_columns(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  if (argc > 255) {
    sqlite3_result_error(ctx, "Too many columns to pack (max 255)", -1);
    return;
  }

  // Calculate required buffer size
  int buf_size = 1; // 1 byte for column count
  for (int i = 0; i < argc; i++) {
    int type = sqlite3_value_type(argv[i]);
    switch (type) {
      case SQLITE_NULL:
        buf_size += 1; // type byte only
        break;
      case SQLITE_FLOAT:
        buf_size += 1 + 8; // type byte + 8 bytes for double
        break;
      case SQLITE_INTEGER: {
        sqlite3_int64 val = sqlite3_value_int64(argv[i]);
        int num_bytes = num_bytes_needed_i64(val);
        buf_size += 1 + num_bytes; // type byte + integer bytes
        break;
      }
      case SQLITE_TEXT:
      case SQLITE_BLOB: {
        int len = sqlite3_value_bytes(argv[i]);
        int num_bytes_for_len = num_bytes_needed_i32(len);
        buf_size += 1 + num_bytes_for_len + len; // type byte + length + data
        break;
      }
    }
  }

  // Allocate buffer
  unsigned char *buffer = sqlite3_malloc(buf_size);
  if (!buffer) {
    sqlite3_result_error_nomem(ctx);
    return;
  }

  unsigned char *ptr = buffer;

  // Write column count
  *ptr++ = (unsigned char)argc;

  // Pack each column
  for (int i = 0; i < argc; i++) {
    int type = sqlite3_value_type(argv[i]);

    switch (type) {
      case SQLITE_NULL:
        *ptr++ = SQLITE_NULL;
        break;

      case SQLITE_FLOAT:
        *ptr++ = SQLITE_FLOAT;
        put_double(&ptr, sqlite3_value_double(argv[i]));
        break;

      case SQLITE_INTEGER: {
        sqlite3_int64 val = sqlite3_value_int64(argv[i]);
        unsigned char num_bytes = num_bytes_needed_i64(val);
        unsigned char type_byte = (num_bytes << 3) | SQLITE_INTEGER;
        *ptr++ = type_byte;
        put_int(&ptr, val, num_bytes);
        break;
      }

      case SQLITE_BLOB: {
        int len = sqlite3_value_bytes(argv[i]);
        unsigned char num_bytes_for_len = num_bytes_needed_i32(len);
        unsigned char type_byte = (num_bytes_for_len << 3) | SQLITE_BLOB;
        *ptr++ = type_byte;
        put_int(&ptr, len, num_bytes_for_len);
        memcpy(ptr, sqlite3_value_blob(argv[i]), len);
        ptr += len;
        break;
      }

      case SQLITE_TEXT: {
        int len = sqlite3_value_bytes(argv[i]);
        unsigned char num_bytes_for_len = num_bytes_needed_i32(len);
        unsigned char type_byte = (num_bytes_for_len << 3) | SQLITE_TEXT;
        *ptr++ = type_byte;
        put_int(&ptr, len, num_bytes_for_len);
        memcpy(ptr, sqlite3_value_blob(argv[i]), len);
        ptr += len;
        break;
      }
    }
  }

  sqlite3_result_blob(ctx, buffer, buf_size, sqlite3_free);
}

// Unpack binary blob into column values
crsql_ColumnValue *crsql_unpack_columns(const unsigned char *data, int data_len,
                                        int *num_cols) {
  if (data_len < 1) {
    *num_cols = 0;
    return NULL;
  }

  const unsigned char *ptr = data;
  const unsigned char *end = data + data_len;

  unsigned char count = *ptr++;
  *num_cols = count;

  if (count == 0) return NULL;

  crsql_ColumnValue *values = sqlite3_malloc(sizeof(crsql_ColumnValue) * count);
  if (!values) {
    *num_cols = 0;
    return NULL;
  }

  for (int i = 0; i < count; i++) {
    if (ptr >= end) {
      // Error: truncated data
      crsql_free_unpacked_columns(values, i);
      *num_cols = 0;
      return NULL;
    }

    unsigned char type_and_intlen = *ptr++;
    unsigned char type = type_and_intlen & 0x07;
    unsigned char intlen = (type_and_intlen >> 3) & 0xFF;

    switch (type) {
      case SQLITE_NULL:
        values[i].type = CRSQL_COLUMN_NULL;
        break;

      case SQLITE_FLOAT:
        if (ptr + 8 > end) goto error;
        values[i].type = CRSQL_COLUMN_FLOAT;
        values[i].val.f64 = get_double(&ptr);
        break;

      case SQLITE_INTEGER:
        if (ptr + intlen > end) goto error;
        values[i].type = CRSQL_COLUMN_INTEGER;
        values[i].val.i64 = get_int(&ptr, intlen);
        break;

      case SQLITE_BLOB: {
        if (ptr + intlen > end) goto error;
        int len = (int)get_int(&ptr, intlen);
        if (ptr + len > end) goto error;
        values[i].type = CRSQL_COLUMN_BLOB;
        values[i].val.blob.len = len;
        values[i].val.blob.data = sqlite3_malloc(len);
        if (!values[i].val.blob.data) goto error;
        memcpy(values[i].val.blob.data, ptr, len);
        ptr += len;
        break;
      }

      case SQLITE_TEXT: {
        if (ptr + intlen > end) goto error;
        int len = (int)get_int(&ptr, intlen);
        if (ptr + len > end) goto error;
        values[i].type = CRSQL_COLUMN_TEXT;
        values[i].val.text.len = len;
        values[i].val.text.data = sqlite3_malloc(len + 1);
        if (!values[i].val.text.data) goto error;
        memcpy(values[i].val.text.data, ptr, len);
        values[i].val.text.data[len] = '\0';
        ptr += len;
        break;
      }

      default:
        goto error;
    }
  }

  return values;

error:
  crsql_free_unpacked_columns(values, i);
  *num_cols = 0;
  return NULL;
}

// Free unpacked column values
void crsql_free_unpacked_columns(crsql_ColumnValue *values, int num_cols) {
  if (!values) return;

  for (int i = 0; i < num_cols; i++) {
    if (values[i].type == CRSQL_COLUMN_BLOB && values[i].val.blob.data) {
      sqlite3_free(values[i].val.blob.data);
    } else if (values[i].type == CRSQL_COLUMN_TEXT && values[i].val.text.data) {
      sqlite3_free(values[i].val.text.data);
    }
  }
  sqlite3_free(values);
}

// Bind unpacked values to a statement
int crsql_bind_package_to_stmt(sqlite3_stmt *stmt, crsql_ColumnValue *values,
                               int num_cols, int offset) {
  for (int i = 0; i < num_cols; i++) {
    int slot = i + 1 + offset;
    int rc;

    switch (values[i].type) {
      case CRSQL_COLUMN_NULL:
        rc = sqlite3_bind_null(stmt, slot);
        break;

      case CRSQL_COLUMN_INTEGER:
        rc = sqlite3_bind_int64(stmt, slot, values[i].val.i64);
        break;

      case CRSQL_COLUMN_FLOAT:
        rc = sqlite3_bind_double(stmt, slot, values[i].val.f64);
        break;

      case CRSQL_COLUMN_BLOB:
        rc = sqlite3_bind_blob(stmt, slot, values[i].val.blob.data,
                              values[i].val.blob.len, SQLITE_STATIC);
        break;

      case CRSQL_COLUMN_TEXT:
        rc = sqlite3_bind_text(stmt, slot, values[i].val.text.data,
                              values[i].val.text.len, SQLITE_STATIC);
        break;

      default:
        return SQLITE_MISUSE;
    }

    if (rc != SQLITE_OK) return rc;
  }

  return SQLITE_OK;
}
