#ifndef CRSQLITE_CONSTS_H
#define CRSQLITE_CONSTS_H

// db version is a signed 64bit int since sqlite doesn't support saving and
// retrieving unsigned 64bit ints. (2^64 / 2) is a big enough number to write 1
// million entries per second for 3,000 centuries.
#define MIN_POSSIBLE_DB_VERSION 0L

#define __CRSQL_CLOCK_LEN 13

#define CRR_SPACE 0
#define USER_SPACE 1
#define ROWID_SLAB_SIZE 10000000000000LL

#define CLOCK_TABLES_SELECT                                                  \
  "SELECT tbl_name FROM sqlite_master WHERE type='table' AND tbl_name LIKE " \
  "'%__crsql_clock'"

#define SET_SYNC_BIT "SELECT crsql_internal_sync_bit(1)"
#define CLEAR_SYNC_BIT "SELECT crsql_internal_sync_bit(0)"

#define TBL_SITE_ID "crsql_site_id"
#define TBL_DB_VERSION "db_version"
#define TBL_SCHEMA "crsql_master"
#define UNION_ALL "UNION ALL"

#define MAX_TBL_NAME_LEN 2048
#define SITE_ID_LEN 16

// Sentinel column names used in clock tables
#define INSERT_SENTINEL "-1"
#define DELETE_SENTINEL "-1"

// Row types for changes virtual table
#define ROW_TYPE_UPDATE 0
#define ROW_TYPE_DELETE 1
#define ROW_TYPE_PKONLY 2

// Version int:
// MM.mm.pp.bb
// 00 00 00 00
// a 0.16.3 release is 16_03_00 -> 160300
#define CRSQLITE_VERSION 160300
#define CRSQLITE_VERSION_STR "0.16.3"
#define CRSQLITE_VERSION_0_15_0 150000

#endif
