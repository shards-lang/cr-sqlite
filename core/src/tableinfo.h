#ifndef CRSQLITE_TABLEINFO_H
#define CRSQLITE_TABLEINFO_H

#include "sqlite3ext.h"

// Column information
typedef struct crsql_ColumnInfo {
  char *name;
  char *type;
  int notnull;
  int pk_index;  // 0 if not a pk, otherwise the index in the pk
} crsql_ColumnInfo;

// Table information and metadata
typedef struct crsql_TableInfo {
  char *tbl_name;
  crsql_ColumnInfo **pks;
  int pks_len;
  crsql_ColumnInfo **non_pks;
  int non_pks_len;

  // TODO: Add cached statements as we port more functionality
  // sqlite3_stmt *select_key_stmt;
  // etc.
} crsql_TableInfo;

#endif
