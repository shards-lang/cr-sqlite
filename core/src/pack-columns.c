#include "pack-columns.h"

#include <string.h>

#include "consts.h"

// Pack format:
// [num_columns:u8, ...[(type(0-2)|num_bytes(3-7)):u8, length?:big-endian-int,
// ...bytes]]
//
// Type is in lower 3 bits, matching SQLITE_* type constants:
//   SQLITE_INTEGER = 1, SQLITE_FLOAT = 2, SQLITE_TEXT = 3,
//   SQLITE_BLOB = 4, SQLITE_NULL = 5
// Upper 5 bits encode how many bytes follow for the integer (length or value).
// For SQLITE_FLOAT, no length prefix — always 8 bytes.
// For SQLITE_NULL, no payload.

// Determine minimum bytes needed to represent a signed integer in big-endian.
// Must account for sign bit: if the high bit of the most significant encoded
// byte would be set, we need one more byte to preserve the sign.
static int num_bytes_needed_i64(sqlite3_int64 val) {
  if (val == 0) return 0;

  // For negative values, count bytes needed for the two's complement
  // representation. For positive values, ensure the high bit isn't set
  // in the most significant byte (which would make it negative on decode).
  if (val > 0) {
    // If positive, we check if adding a sign bit would require more bytes
    if (val <= 0x7F) return 1;
    if (val <= 0x7FFF) return 2;
    if (val <= 0x7FFFFF) return 3;
    if (val <= 0x7FFFFFFF) return 4;
    if (val <= 0x7FFFFFFFFFLL) return 5;
    if (val <= 0x7FFFFFFFFFFFLL) return 6;
    if (val <= 0x7FFFFFFFFFFFFFLL) return 7;
    return 8;
  } else {
    // Negative: check how many bytes needed for sign-extended representation
    if (val >= -0x80) return 1;
    if (val >= -0x8000) return 2;
    if (val >= -0x800000) return 3;
    if (val >= -0x80000000LL) return 4;
    if (val >= -0x8000000000LL) return 5;
    if (val >= -0x800000000000LL) return 6;
    if (val >= -0x80000000000000LL) return 7;
    return 8;
  }
}

static int num_bytes_needed_i32(int val) {
  return num_bytes_needed_i64((sqlite3_int64)val);
}

// Write a big-endian integer of numBytes length to buf
static void put_int(unsigned char *buf, sqlite3_int64 val, int numBytes) {
  for (int i = numBytes - 1; i >= 0; i--) {
    buf[i] = (unsigned char)(val & 0xFF);
    val >>= 8;
  }
}

// Read a big-endian signed integer of numBytes length from buf
static sqlite3_int64 get_int(const unsigned char *buf, int numBytes) {
  if (numBytes == 0) {
    return 0;
  }
  // Sign-extend from the most significant byte
  sqlite3_int64 result = (signed char)buf[0];
  for (int i = 1; i < numBytes; i++) {
    result = (result << 8) | buf[i];
  }
  return result;
}

// Read a big-endian float (8 bytes) from buf
static double get_f64(const unsigned char *buf) {
  double result;
  unsigned char swapped[8];
  // Network byte order (big-endian) to host
  for (int i = 0; i < 8; i++) {
    swapped[7 - i] = buf[i];
  }
  memcpy(&result, swapped, 8);
  return result;
}

// Write a big-endian float (8 bytes) to buf
static void put_f64(unsigned char *buf, double val) {
  unsigned char raw[8];
  memcpy(raw, &val, 8);
  for (int i = 0; i < 8; i++) {
    buf[7 - i] = raw[i];
  }
}

unsigned char *crsql_pack_columns(sqlite3_value **values, int numValues,
                                  int *outLen) {
  if (numValues < 0 || numValues > 255) {
    return 0;
  }

  // First pass: compute total size
  int totalSize = 1;  // num_columns byte
  for (int i = 0; i < numValues; i++) {
    int type = sqlite3_value_type(values[i]);
    switch (type) {
      case SQLITE_BLOB: {
        int blobLen = sqlite3_value_bytes(values[i]);
        int nbLen = num_bytes_needed_i32(blobLen);
        totalSize += 1 + nbLen + blobLen;  // type_byte + length + data
        break;
      }
      case SQLITE_NULL:
        totalSize += 1;  // just type_byte
        break;
      case SQLITE_FLOAT:
        totalSize += 1 + 8;  // type_byte + 8 bytes
        break;
      case SQLITE_INTEGER: {
        sqlite3_int64 val = sqlite3_value_int64(values[i]);
        int nb = num_bytes_needed_i64(val);
        totalSize += 1 + nb;  // type_byte + value
        break;
      }
      case SQLITE_TEXT: {
        int textLen = sqlite3_value_bytes(values[i]);
        int nbLen = num_bytes_needed_i32(textLen);
        totalSize += 1 + nbLen + textLen;  // type_byte + length + data
        break;
      }
    }
  }

  unsigned char *buf = sqlite3_malloc(totalSize);
  if (buf == 0) {
    return 0;
  }

  int pos = 0;
  buf[pos++] = (unsigned char)numValues;

  for (int i = 0; i < numValues; i++) {
    int type = sqlite3_value_type(values[i]);
    switch (type) {
      case SQLITE_BLOB: {
        int blobLen = sqlite3_value_bytes(values[i]);
        int nbLen = num_bytes_needed_i32(blobLen);
        buf[pos++] = (unsigned char)((nbLen << 3) | type);
        put_int(buf + pos, blobLen, nbLen);
        pos += nbLen;
        memcpy(buf + pos, sqlite3_value_blob(values[i]), blobLen);
        pos += blobLen;
        break;
      }
      case SQLITE_NULL:
        buf[pos++] = (unsigned char)type;
        break;
      case SQLITE_FLOAT:
        buf[pos++] = (unsigned char)type;
        put_f64(buf + pos, sqlite3_value_double(values[i]));
        pos += 8;
        break;
      case SQLITE_INTEGER: {
        sqlite3_int64 val = sqlite3_value_int64(values[i]);
        int nb = num_bytes_needed_i64(val);
        buf[pos++] = (unsigned char)((nb << 3) | type);
        put_int(buf + pos, val, nb);
        pos += nb;
        break;
      }
      case SQLITE_TEXT: {
        int textLen = sqlite3_value_bytes(values[i]);
        int nbLen = num_bytes_needed_i32(textLen);
        buf[pos++] = (unsigned char)((nbLen << 3) | type);
        put_int(buf + pos, textLen, nbLen);
        pos += nbLen;
        // Use blob accessor to get raw bytes (same as Rust impl)
        memcpy(buf + pos, sqlite3_value_blob(values[i]), textLen);
        pos += textLen;
        break;
      }
    }
  }

  *outLen = totalSize;
  return buf;
}

crsql_ColumnValue *crsql_unpack_columns(const unsigned char *data, int dataLen,
                                        int *outLen) {
  if (dataLen < 1) {
    return 0;
  }

  int pos = 0;
  int numColumns = data[pos++];
  *outLen = numColumns;

  if (numColumns == 0) {
    return 0;
  }

  crsql_ColumnValue *values = sqlite3_malloc(sizeof(crsql_ColumnValue) * numColumns);
  if (values == 0) {
    return 0;
  }
  memset(values, 0, sizeof(crsql_ColumnValue) * numColumns);

  for (int i = 0; i < numColumns; i++) {
    if (pos >= dataLen) {
      goto error;
    }

    unsigned char typeAndLen = data[pos++];
    int colType = typeAndLen & 0x07;
    int intLen = (typeAndLen >> 3) & 0x1F;

    switch (colType) {
      case SQLITE_BLOB: {
        if (pos + intLen > dataLen) goto error;
        int blobLen = (int)get_int(data + pos, intLen);
        pos += intLen;
        if (blobLen < 0 || pos + blobLen > dataLen) goto error;
        values[i].type = CRSQL_CV_BLOB;
        values[i].v.blob.data = sqlite3_malloc(blobLen);
        if (values[i].v.blob.data == 0) goto error;
        memcpy(values[i].v.blob.data, data + pos, blobLen);
        values[i].v.blob.len = blobLen;
        pos += blobLen;
        break;
      }
      case SQLITE_FLOAT: {
        if (pos + 8 > dataLen) goto error;
        values[i].type = CRSQL_CV_FLOAT;
        values[i].v.floatVal = get_f64(data + pos);
        pos += 8;
        break;
      }
      case SQLITE_INTEGER: {
        if (pos + intLen > dataLen) goto error;
        values[i].type = CRSQL_CV_INTEGER;
        values[i].v.integer = get_int(data + pos, intLen);
        pos += intLen;
        break;
      }
      case SQLITE_NULL: {
        values[i].type = CRSQL_CV_NULL;
        break;
      }
      case SQLITE_TEXT: {
        if (pos + intLen > dataLen) goto error;
        int textLen = (int)get_int(data + pos, intLen);
        pos += intLen;
        if (textLen < 0 || pos + textLen > dataLen) goto error;
        values[i].type = CRSQL_CV_TEXT;
        values[i].v.text.data = sqlite3_malloc(textLen + 1);
        if (values[i].v.text.data == 0) goto error;
        memcpy(values[i].v.text.data, data + pos, textLen);
        values[i].v.text.data[textLen] = '\0';
        values[i].v.text.len = textLen;
        pos += textLen;
        break;
      }
      default:
        goto error;
    }
  }

  return values;

error:
  crsql_free_column_values(values, numColumns);
  *outLen = 0;
  return 0;
}

void crsql_free_column_values(crsql_ColumnValue *values, int numValues) {
  if (values == 0) {
    return;
  }
  for (int i = 0; i < numValues; i++) {
    if (values[i].type == CRSQL_CV_BLOB) {
      sqlite3_free(values[i].v.blob.data);
    } else if (values[i].type == CRSQL_CV_TEXT) {
      sqlite3_free(values[i].v.text.data);
    }
  }
  sqlite3_free(values);
}

int crsql_bind_package_to_stmt(sqlite3_stmt *pStmt, crsql_ColumnValue *values,
                               int numValues, int offset) {
  for (int i = 0; i < numValues; i++) {
    int slot = i + 1 + offset;
    int rc;
    switch (values[i].type) {
      case CRSQL_CV_BLOB:
        rc = sqlite3_bind_blob(pStmt, slot, values[i].v.blob.data,
                               values[i].v.blob.len, SQLITE_TRANSIENT);
        break;
      case CRSQL_CV_FLOAT:
        rc = sqlite3_bind_double(pStmt, slot, values[i].v.floatVal);
        break;
      case CRSQL_CV_INTEGER:
        rc = sqlite3_bind_int64(pStmt, slot, values[i].v.integer);
        break;
      case CRSQL_CV_NULL:
        rc = sqlite3_bind_null(pStmt, slot);
        break;
      case CRSQL_CV_TEXT:
        rc = sqlite3_bind_text(pStmt, slot, values[i].v.text.data,
                               values[i].v.text.len, SQLITE_TRANSIENT);
        break;
      default:
        return SQLITE_MISUSE;
    }
    if (rc != SQLITE_OK) {
      return rc;
    }
  }
  return SQLITE_OK;
}

void crsql_pack_columns_fn(sqlite3_context *ctx, int argc,
                            sqlite3_value **argv) {
  int blobLen = 0;
  unsigned char *blob = crsql_pack_columns(argv, argc, &blobLen);
  if (blob == 0) {
    sqlite3_result_error(ctx, "Failed to pack columns", -1);
    return;
  }
  sqlite3_result_blob(ctx, blob, blobLen, sqlite3_free);
}
