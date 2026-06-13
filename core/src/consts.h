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

#define TBL_SITE_ID "crsql_site_id"
#define TBL_DB_VERSION "db_version"
#define TBL_SCHEMA "crsql_master"
#define UNION_ALL "UNION ALL"

#define MAX_TBL_NAME_LEN 2048
#define SITE_ID_LEN 16

// Sentinel column name used in clock tables for row create/delete events.
// The same cid value is used for both inserts and deletes; the distinction
// is made by causal length parity (odd = alive, even = deleted).
#define SENTINEL_CID "-1"

// Row types for changes virtual table
#define ROW_TYPE_UPDATE 0
#define ROW_TYPE_DELETE 1
#define ROW_TYPE_PKONLY 2

// Tracked-peer event kinds. Recorded in crsql_tracked_peers.event.
// RECEIVED is auto-bumped by merge_insert_impl when a change is processed
// from a remote peer. SENT is reserved for application use (e.g. recording
// the watermark of changes the local node has shipped to a peer).
#define TRACKED_EVENT_RECEIVED 0
#define TRACKED_EVENT_SENT 1

// Monotonic upsert into crsql_tracked_peers: only advances the watermark
// when the incoming (version, seq) is strictly greater than what is stored.
// Idempotent on equal values, safe under out-of-order arrival.
#define UPSERT_TRACKED_PEER                                                   \
  "INSERT INTO crsql_tracked_peers(site_id, version, seq, tag, event) "       \
  "VALUES (?1, ?2, ?3, ?4, ?5) "                                              \
  "ON CONFLICT(site_id, tag, event) DO UPDATE SET "                           \
  "  version = excluded.version, seq = excluded.seq "                         \
  "WHERE (excluded.version, excluded.seq) > "                                 \
  "      (crsql_tracked_peers.version, crsql_tracked_peers.seq)"

// Version int:
// MM.mm.pp.bb
// 00 00 00 00
// a 0.16.3 release is 16_03_00 -> 160300
#define CRSQLITE_VERSION 160300
#define CRSQLITE_VERSION_STR "0.16.3"
#define CRSQLITE_VERSION_0_15_0 150000

#endif
