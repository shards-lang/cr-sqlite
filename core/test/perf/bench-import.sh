#!/bin/bash
# Benchmark: import a crsql_changes diff into a fresh database.
# Usage: ./bench-import.sh [num_rows] [path-to-sqlite3-cli]
set -e

ROWS="${1:-20000}"
SQLITE="${2:-$(dirname "$0")/../../build/sqlite3}"
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

# ---- 1. Build a source db with ROWS rows (4 data cols => 5 changes/row) ----
"$SQLITE" "$DIR/source.db" >/dev/null <<EOF
PRAGMA journal_mode = WAL;
CREATE TABLE foo (id INTEGER PRIMARY KEY NOT NULL, a TEXT, b TEXT, c INTEGER, d REAL);
SELECT crsql_as_crr('foo');
BEGIN;
WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < $ROWS)
INSERT INTO foo (id, a, b, c, d)
SELECT x, 'name-' || x, 'description for row ' || x, x * 7, x * 1.5 FROM cnt;
COMMIT;
-- Stage the changeset like a sync peer would ship it
CREATE TABLE staged AS
  SELECT "table" AS tbl, pk, cid, val, col_version, db_version,
         COALESCE(site_id, crsql_site_id()) AS site_id, cl, seq
  FROM crsql_changes;
SELECT 'staged changes: ' || COUNT(*) FROM staged;
SELECT crsql_finalize();
EOF

# ---- 2. Import into a fresh db, timed ----
"$SQLITE" "$DIR/target.db" <<EOF
PRAGMA journal_mode = WAL;
CREATE TABLE foo (id INTEGER PRIMARY KEY NOT NULL, a TEXT, b TEXT, c INTEGER, d REAL);
SELECT crsql_as_crr('foo');
ATTACH '$DIR/source.db' AS src;
.timer on
BEGIN;
INSERT INTO crsql_changes ("table", pk, cid, val, col_version, db_version, site_id, cl, seq)
  SELECT tbl, pk, cid, val, col_version, db_version, site_id, cl, seq FROM src.staged;
COMMIT;
.timer off
SELECT 'imported rows: ' || COUNT(*) FROM foo;
SELECT crsql_finalize();
EOF

# ---- 3. Re-import same diff (idempotent merge path: all changes lose) ----
"$SQLITE" "$DIR/target.db" <<EOF
ATTACH '$DIR/source.db' AS src;
.timer on
BEGIN;
INSERT INTO crsql_changes ("table", pk, cid, val, col_version, db_version, site_id, cl, seq)
  SELECT tbl, pk, cid, val, col_version, db_version, site_id, cl, seq FROM src.staged;
COMMIT;
.timer off
SELECT 'rows after re-import: ' || COUNT(*) FROM foo;
SELECT crsql_finalize();
EOF
