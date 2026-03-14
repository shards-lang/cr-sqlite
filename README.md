# cr-sqlite - Convergent, Replicated SQLite

[![c-tests](https://github.com/shards-lang/cr-sqlite/actions/workflows/c-tests.yaml/badge.svg)](https://github.com/shards-lang/cr-sqlite/actions/workflows/c-tests.yaml)
[![c-valgrind](https://github.com/shards-lang/cr-sqlite/actions/workflows/c-valgrind.yaml/badge.svg)](https://github.com/shards-lang/cr-sqlite/actions/workflows/c-valgrind.yaml)
[![c-asan](https://github.com/shards-lang/cr-sqlite/actions/workflows/c-asan.yaml/badge.svg)](https://github.com/shards-lang/cr-sqlite/actions/workflows/c-asan.yaml)
[![py-tests](https://github.com/shards-lang/cr-sqlite/actions/workflows/py-tests.yaml/badge.svg)](https://github.com/shards-lang/cr-sqlite/actions/workflows/py-tests.yaml)

A [run-time loadable extension](https://www.sqlite.org/loadext.html) for [SQLite](https://www.sqlite.org/) that adds multi-master replication and partition tolerance via [CRDTs](https://en.wikipedia.org/wiki/Conflict-free_replicated_data_type).

Write to your SQLite database while offline. Others write to theirs. Come online, merge, no conflicts.

This is a fork of [vlcn-io/cr-sqlite](https://github.com/vlcn-io/cr-sqlite), rewritten as a pure C extension. The original project used a hybrid C/Rust architecture; this fork eliminates the Rust dependency entirely.

## Building

Requires only a C compiler and CMake (>= 3.16). No Rust, no Cargo, no nightly toolchains.

```bash
git clone git@github.com:shards-lang/cr-sqlite.git
cd cr-sqlite/core
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces:
- `build/crsqlite.[dylib|so|dll]` — loadable SQLite extension
- `build/sqlite3` — SQLite CLI with cr-sqlite built in
- `build/libcrsqlite_static.a` — static library

Or using Make directly:

```bash
cd cr-sqlite/core
make loadable
```

## Usage

```sql
-- load the extension
.load crsqlite

-- create tables as normal
CREATE TABLE foo (a PRIMARY KEY NOT NULL, b);
CREATE TABLE baz (a PRIMARY KEY NOT NULL, b, c, d);

-- upgrade to CRRs (conflict-free replicated relations)
SELECT crsql_as_crr('foo');
SELECT crsql_as_crr('baz');

-- write data as normal
INSERT INTO foo (a, b) VALUES (1, 2);
INSERT INTO baz (a, b, c, d) VALUES ('a', 'woo', 'doo', 'daa');

-- get changes to sync to other nodes
SELECT * FROM crsql_changes WHERE db_version > 0;

-- apply changes received from another node
INSERT INTO crsql_changes VALUES (...);

-- clean up before closing
SELECT crsql_finalize();
```

## How It Works

Tables upgraded with `crsql_as_crr()` get:

- **Clock tables** (`__crsql_clock`) tracking per-column version vectors
- **PK lookaside tables** (`__crsql_pks`) mapping primary keys to internal IDs
- **AFTER triggers** capturing inserts, updates, and deletes

The `crsql_changes` virtual table exposes changesets for sync and accepts incoming changes for merge. Conflict resolution uses **Last-Write-Wins** with causal length tracking for delete/resurrect semantics.

### Sync Protocol

```
Node A                              Node B
  |                                   |
  |-- SELECT * FROM crsql_changes --> |
  |   WHERE db_version > last_seen    |
  |                                   |
  | <-- INSERT INTO crsql_changes --- |
  |     (changes from B)              |
```

Both nodes converge to the same state regardless of sync order.

## API Reference

### Functions

| Function | Description |
|----------|-------------|
| `crsql_as_crr('table')` | Upgrade a table to a CRR |
| `crsql_as_table('table')` | Downgrade a CRR back to a regular table |
| `crsql_site_id()` | Returns this node's unique 16-byte site ID |
| `crsql_db_version()` | Returns the current database version (lamport clock) |
| `crsql_next_db_version()` | Returns the next version for writes |
| `crsql_begin_alter('table')` | Start altering a CRR schema |
| `crsql_commit_alter('table')` | Finish altering a CRR schema |
| `crsql_automigrate(schema)` | Auto-migrate schema to match provided SQL |
| `crsql_finalize()` | Clean up before closing the connection |
| `crsql_config_set(key, val)` | Set a config option (e.g., `'merge-equal-values'`) |
| `crsql_config_get(key)` | Get a config option |

### Changes Virtual Table

The `crsql_changes` virtual table has these columns:

| Column | Type | Description |
|--------|------|-------------|
| `table` | TEXT | Source table name |
| `pk` | BLOB | Packed primary key values |
| `cid` | TEXT | Column name (or `'-1'` for sentinel) |
| `val` | ANY | Column value |
| `col_version` | INTEGER | Per-column version counter |
| `db_version` | INTEGER | Database-wide lamport clock |
| `site_id` | BLOB | 16-byte site ID of the writer |
| `cl` | INTEGER | Causal length (odd = alive, even = deleted) |
| `seq` | INTEGER | Sequence within a transaction |

### Fractional Indexing

For ordered lists (e.g., drag-and-drop reorderable items):

```sql
CREATE TABLE items (id PRIMARY KEY NOT NULL, list TEXT, spot TEXT, content TEXT);
SELECT crsql_as_crr('items');
SELECT crsql_fract_as_ordered('items', 'spot', 'list');

-- insert at the end (spot = 1) or beginning (spot = -1)
INSERT INTO items VALUES ('a', 'todo', 1, 'first item');
INSERT INTO items VALUES ('b', 'todo', 1, 'second item');

-- insert after a specific item via the fractindex view
INSERT INTO items_fractindex (id, list, content, after_id) VALUES ('c', 'todo', 'between', 'a');
```

### Altering CRR Tables

```sql
SELECT crsql_begin_alter('table_name');
ALTER TABLE table_name ADD COLUMN new_col TEXT;
SELECT crsql_commit_alter('table_name');
```

## CRR Compatibility Requirements

Tables upgraded to CRRs must:

- Have a non-nullable primary key
- Not use `AUTOINCREMENT`
- Not have unique indices (besides the primary key)
- Not have checked foreign key constraints
- Have default values for all `NOT NULL` non-PK columns

## Tests

```bash
# C unit tests (via CMake)
cd core
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest --output-on-failure

# Python correctness tests (148 tests)
cd core
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target crsqlite
mkdir -p dist && cp build/crsqlite.* dist/
cd ../py/correctness
pip install pytest hypothesis
pip install -e .
pytest

# Memory checks (via Make)
cd core && make valgrind
cd core && make asan
```

## Performance

- Inserts into CRRs are ~2.5x slower than regular SQLite tables (due to trigger overhead and clock table writes)
- Reads are the same speed as regular SQLite

## Research & Prior Art

cr-sqlite was inspired by:

- [Towards a General Database Management System of Conflict-Free Replicated Relations](https://munin.uit.no/bitstream/handle/10037/22344/thesis.pdf?sequence=2)
- [Conflict-Free Replicated Relations for Multi-Synchronous Database Management at Edge](https://hal.inria.fr/hal-02983557/document)
- [Time, Clocks, and the Ordering of Events in a Distributed System](https://lamport.azurewebsites.net/pubs/time-clocks.pdf)
- [CRDTs for Brrr](https://josephg.com/blog/crdts-go-brrr/)

## License

Same as the original project.
