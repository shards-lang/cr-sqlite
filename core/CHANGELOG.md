# @vlcn.io/crsqlite

## Unreleased

### Patch Changes

- Large speedup (~8x) of merging changes via `INSERT INTO crsql_changes`:
  - Removed `RETURNING` clauses from hot-path statements (winner clock, pk
    lookaside, site_id ordinal). `RETURNING` forced SQLite to materialize an
    ephemeral btree -- with its own pager and page cache -- on every executed
    row; values are now read from bound parameters or
    `sqlite3_last_insert_rowid`.
  - Unpacked primary keys are bound directly to the key lookaside statements
    instead of round-tripping through a per-row `SELECT ?,?,...` prepare. This
    per-row `prepare`/`finalize` was a C-port artifact (bridging packed PKs to
    a `sqlite3_value**` key API) absent from the original Rust, and was the
    reason the pre-port merge path was marginally slower than upstream Rust;
    binding the unpacked values directly -- as the Rust did -- restores parity
    before the other wins compound on top.
  - The sync bit is toggled through a direct pointer instead of executing
    `SELECT crsql_internal_sync_bit(x)` per merged change.
  - `(table, pk) -> (lookaside key, causal length)` of the last merged row is
    memoized, skipping redundant lookups for the N column changes of a row
    (changesets arrive ordered by `db_version, seq`). Invalidated on
    commit/rollback, savepoint rollback, local CRR writes, schema reloads and
    merge errors.
  - The last `site_id -> ordinal` resolution is memoized (changesets are
    virtually always single-site).
  - Equal-version changes originating from the site that authored the local
    clock entry are rejected as identical without fetching the local value
    for comparison (a site's col_version is monotonic per cell). Idempotent
    re-imports and own-changes echoes become pure clock-table reads; true
    concurrent edits (different sites) still tie-break on value.
  - `crsql_next_db_version()` is computed in C and skips its
    `PRAGMA data_version` probe when a transaction already established the
    pending version.

## 0.16.3

### Patch Changes

- fix windows breakage, fix wasm oom

## 0.16.1

### Patch Changes

- fix install script for nodejs based projects

## 0.16.0

### Minor Changes

- e4a0a42: v0.16.0-next

### Patch Changes

- d485812: prepare `tables_used` query, correctly unzip native library from pre-builds
- 6f0ccac: fix error where separate connections would not report the correct db version

## 0.16.0-next.2

### Patch Changes

- fix error where separate connections would not report the correct db version

## 0.16.0-next.1

### Patch Changes

- prepare `tables_used` query, correctly unzip native library from pre-builds

## 0.16.0-next.0

### Minor Changes

- v0.16.0-next

## 0.15.1

### Patch Changes

- c113d8c: ensure statements are finalized when closing db, allow automigrating fractindex tables, fractindex w/o list columns fix

## 0.15.1-next.0

### Patch Changes

- ensure statements are finalized when closing db, allow automigrating fractindex tables, fractindex w/o list columns fix

## 0.15.0

### Minor Changes

- 56df096: re-insertion, api naming consistencies, metadata size reduction, websocket server, websocket client, websocket demo

### Patch Changes

- 4022bd6: litefs support
- 08f13fb: react strict mode fiex, migrator fixes, typed-sql basic support, ws replication, db provider hooks
- f327068: rebuild

## 0.15.0-next.2

### Patch Changes

- litefs support

## 0.15.0-next.1

### Patch Changes

- react strict mode fiex, migrator fixes, typed-sql basic support, ws replication, db provider hooks

## 0.15.0-next.0

### Minor Changes

- re-insertion, api naming consistencies, metadata size reduction, websocket server, websocket client, websocket demo

## 0.14.0

### Minor Changes

- 68deb1c: binary encoded primary keys, no string encoding on values, cache prepared statements on merge, fix webkit JIT crash

## 0.14.0-next.0

### Minor Changes

- binary encoded primary keys, no string encoding on values, cache prepared statements on merge, fix webkit JIT crash

## 0.13.0

### Minor Changes

- 62912ad: split up large transactions, compact out unneeded delete records, coordinate dedicated workers for android, null merge fix

## 0.13.0-next.0

### Minor Changes

- split up large transactions, compact out unneeded delete records, coordinate dedicated workers for android, null merge fix

## 0.12.0

### Minor Changes

- 7885afd: 50x perf boost when pulling changesets

## 0.12.0-next.0

### Minor Changes

- 15c8e04: 50x perf boost when pulling changesets

## 0.11.0

### Minor Changes

- automigrate fixes for WASM, react fixes for referential equality, direct-connect networking implementations, sync in shared worker, dbProvider hooks for React

### Patch Changes

- 4e737a0: better error reporting on migration failure, handle schema swap

## 0.10.2-next.0

### Patch Changes

- better error reporting on migration failure, handle schema swap

## 0.10.1

### Patch Changes

- fts5, sqlite 3.42.1, direct-connect packages

## 0.10.0

### Minor Changes

- e0de95c: ANSI SQL compliance for crsql_changes, all filters available for crsql_changes, removal of tracked_peers, simplified crsql_master table

### Patch Changes

- 9b483aa: npm is not updating on package publish -- bump versions to try to force it

## 0.10.0-next.1

### Patch Changes

- npm is not updating on package publish -- bump versions to try to force it

## 0.10.0-next.0

### Minor Changes

- ANSI SQL compliance for crsql_changes, all filters available for crsql_changes, removal of tracked_peers, simplified crsql_master table

## 0.9.3

### Patch Changes

- release lock fix

## 0.9.2

### Patch Changes

- e5919ae: fix xcommit deadlock, bump versions on dependencies

## 0.9.2-next.0

### Patch Changes

- fix xcommit deadlock, bump versions on dependencies

## 0.9.1

### Patch Changes

- 419ee8f: include makefile in pkg
- aad733d: --

## 0.9.1-next.1

### Patch Changes

- include makefile in pkg

## 0.9.1-next.0

### Patch Changes

---

## 0.9.0

### Minor Changes

- 14c9f4e: useQuery perf updates, primary key only table fixes, sync in a background worker

## 0.9.0-next.0

### Minor Changes

- useQuery perf updates, primary key only table fixes, sync in a background worker

## 0.8.0

### Minor Changes

- 6316ec315: update to support prebuild binaries, include primary key only table fixes

### Patch Changes

- b7e0b21df: create dist dir on install
- 606060dbe: include install script

## 0.8.0-next.2

### Patch Changes

- create dist dir on install

## 0.8.0-next.1

### Patch Changes

- include install script

## 0.8.0-next.0

### Minor Changes

- update to support prebuild binaries, include primary key only table fixes

## 0.7.2

### Patch Changes

- 3d09cd595: preview all the hook improvements and multi db open fixes
- 567d8acba: auto-release prepared statements
- 54666261b: fractional indexing inclusion
- fractional indexing, better react hooks, many dbs opened concurrently
- fd9094220: fixup what is packed

## 0.7.2-next.3

### Patch Changes

- preview all the hook improvements and multi db open fixes

## 0.7.2-next.2

### Patch Changes

- auto-release prepared statements

## 0.7.2-next.1

### Patch Changes

- fixup what is packed

## 0.7.2-next.0

### Patch Changes

- fractional indexing inclusion

## 0.7.1

### Patch Changes

- 519bcfc2a: hooks, fixes to support examples, auto-determine tables queried
- hooks package, used_tables query, web only target for wa-sqlite

## 0.7.1-next.0

### Patch Changes

- hooks, fixes to support examples, auto-determine tables queried

## 0.7.0

### Minor Changes

- seen peers, binary encoding for network layer, retry on disconnect for server, auto-track peers

## 0.6.3

### Patch Changes

- deploy table validation fix

## 0.6.2

### Patch Changes

- cid winner selection bugfix

## 0.6.1

### Patch Changes

- rebuild all

## 0.6.0

### Minor Changes

- breaking change -- fix version recording problem that prevented convergence in p2p cases

## 0.5.2

### Patch Changes

- fix gh #108

## 0.5.1

### Patch Changes

- fix mem leak and cid win value selection bug

## 0.5.0

### Minor Changes

- fix tie breaking for merge, add example client-server sync

## 0.4.2

### Patch Changes

- fix bigint overflow in wasm, fix site_id not being returned with changesets

## 0.4.1

### Patch Changes

- fix memory leak when applying changesets

## 0.4.0

### Minor Changes

- fix multi-way merge

## 0.3.0

### Minor Changes

- incorporate schema fitness checks

## 0.2.0

### Minor Changes

- update to use `wa-sqlite`, fix site id forwarding, fix scientific notation replication, etc.

## 0.1.8

### Patch Changes

- fix linking issues on linux distros

## 0.1.7

### Patch Changes

- fixes site id not being passed during replication
