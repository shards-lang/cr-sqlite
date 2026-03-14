# CLAUDE.md

## Project Overview

cr-sqlite is a SQLite extension that adds CRDT-based multi-master replication. It is implemented as a pure C loadable extension (no Rust).

## Build

```bash
cd core
make loadable    # builds dist/crsqlite.[dylib|so|dll]
make sqlite3     # builds dist/sqlite3 with extension built in
make test        # builds and runs C unit tests
make asan        # runs with AddressSanitizer
make valgrind    # runs under valgrind (Linux)
```

No Rust toolchain, no Cargo, no special dependencies. Just a C compiler and make.

## Test

```bash
# C unit tests (fast, covers core functionality)
cd core && make test

# Python correctness tests (148 tests, comprehensive sync/merge coverage)
cd py/correctness
pip install pytest hypothesis
pip install -e .
pytest
```

Always run `make test` after changes. Run `pytest` for anything touching merge logic, sync, or the changes virtual table.

## Source Layout

All extension source is in `core/src/`:

| File | Purpose |
|------|---------|
| `crsqlite.c` | Extension entry point, registers all SQL functions |
| `changes-vtab.c` | Changes virtual table module definition and cursor management |
| `changes-vtab-impl.c` | Virtual table implementation: filter, next, column, merge logic |
| `ext-data.c` | ExtData lifecycle (per-connection state) |
| `tableinfo.c` | Table metadata, column info, 15+ cached prepared statements |
| `bootstrap.c` | Site ID init, schema table, DB migrations |
| `db-version.c` | Database version tracking (lamport clock) |
| `triggers.c` | AFTER trigger creation/removal on CRR tables |
| `crr.c` | CRR creation orchestration, clock tables, backfill, config |
| `local-writes.c` | Trigger handler functions (after_insert/update/delete) |
| `pack-columns.c` | Binary serialization of column values |
| `automigrate.c` | Schema migration, compact_post_alter |
| `fractindex.c` | Fractional indexing for ordered lists |
| `util.c` | SQL generation helpers, value comparison |
| `unpack-columns-vtab.c` | Virtual table to decompose packed column blobs |
| `cl-set-vtab.c` | CL-set virtual table (alternative CRR creation) |

## Key Concepts

- **CRR** (Conflict-free Replicated Relation): A regular table upgraded with clock tables and triggers
- **Clock table** (`__crsql_clock`): Tracks (key, col_name, col_version, db_version, site_id, seq) per column change
- **PK lookaside** (`__crsql_pks`): Maps primary keys to internal integer keys
- **Causal length** (CL): Odd = row alive, even = row deleted. Tracks insert/delete cycles
- **Sentinel** (`cid = '-1'`): Special clock entry marking row creation/deletion events
- **Site ID**: 16-byte UUID v4 identifying each database instance
- **db_version**: Lamport clock incremented on each transaction

## Architecture Notes

- All memory allocation uses `sqlite3_malloc`/`sqlite3_free`
- String building uses `sqlite3_mprintf` (auto-escapes with `%q` and `%w`)
- Prepared statements are lazily cached in `crsql_TableInfo` structs (NULL = not yet prepared)
- The `crsql_ExtData` struct holds per-connection state, freed via the `crsql_db_version` function destructor
- The sync bit (`crsql_internal_sync_bit`) prevents trigger recursion during merge operations

## Common Pitfalls

- When binding unpacked `ColumnValue` data to statements, use `SQLITE_TRANSIENT` not `SQLITE_STATIC` if the values will be freed before the statement is stepped
- The `pack_columns` format uses signed big-endian integers. `num_bytes_needed` must account for the sign bit (values >= 128 need 2 bytes, not 1)
- `crsql_TableInfo` structs in `TableInfoVec` are inline (not heap-allocated pointers), so use `free_table_info_contents()` not `crsql_free_table_info()` when cleaning up the vec
- Always call `crsql_finalize()` before closing a connection to avoid "unfinalized statements" warnings
