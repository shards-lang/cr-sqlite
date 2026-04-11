// Integration tests for cr-sqlite with node:sqlite.
// Requires Node >= 22.5 and a built crsqlite extension in build/ or dist/.
import { describe, it, before, after } from "node:test";
import assert from "node:assert/strict";
import { DatabaseSync } from "node:sqlite";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { existsSync } from "node:fs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const root = join(__dirname, "..");

// Prefer build/ (dev) over dist/ (packaged).
const buildPath = join(root, "build", "crsqlite");
const distPath = join(root, "dist", "crsqlite");
const extensionPath = existsSync(buildPath + ".so") || existsSync(buildPath + ".dylib")
  ? buildPath
  : distPath;

function open() {
  const db = new DatabaseSync(":memory:", { allowExtension: true });
  db.loadExtension(extensionPath);
  return db;
}

describe("cr-sqlite extension loading", () => {
  it("registers crsql functions", () => {
    const db = open();
    const funcs = db.prepare(
      "SELECT name FROM pragma_function_list WHERE name LIKE 'crsql%'"
    ).all();
    const names = funcs.map((r) => r.name);
    assert.ok(names.includes("crsql_as_crr"), "crsql_as_crr should be registered");
    assert.ok(names.includes("crsql_site_id"), "crsql_site_id should be registered");
    assert.ok(names.includes("crsql_db_version"), "crsql_db_version should be registered");
    assert.ok(names.includes("crsql_finalize"), "crsql_finalize should be registered");
    db.exec("SELECT crsql_finalize()");
    db.close();
  });

  it("returns a 16-byte site id", () => {
    const db = open();
    const row = db.prepare("SELECT crsql_site_id() AS sid").get();
    // site_id is a 16-byte blob (UUID v4)
    assert.equal(row.sid.byteLength, 16);
    db.exec("SELECT crsql_finalize()");
    db.close();
  });

  it("returns a commit sha", () => {
    const db = open();
    const row = db.prepare("SELECT crsql_sha() AS sha").get();
    assert.ok(typeof row.sha === "string");
    assert.ok(row.sha.length > 0);
    db.exec("SELECT crsql_finalize()");
    db.close();
  });
});

describe("CRR creation and basic operations", () => {
  let db;

  before(() => {
    db = open();
    db.exec("CREATE TABLE todos (id INTEGER PRIMARY KEY NOT NULL, title TEXT, done INTEGER)");
    db.exec("SELECT crsql_as_crr('todos')");
  });

  after(() => {
    db.exec("SELECT crsql_finalize()");
    db.close();
  });

  it("creates clock table", () => {
    const tables = db.prepare(
      "SELECT name FROM sqlite_master WHERE name LIKE '%__crsql_clock'"
    ).all();
    assert.equal(tables.length, 1);
    assert.equal(tables[0].name, "todos__crsql_clock");
  });

  it("tracks inserts in crsql_changes", () => {
    db.exec("INSERT INTO todos VALUES (1, 'buy milk', 0)");
    const changes = db.prepare("SELECT * FROM crsql_changes").all();
    // Should have changes for 'title' and 'done' columns
    assert.ok(changes.length >= 2, `expected >= 2 changes, got ${changes.length}`);
    const tables = new Set(changes.map((c) => c.table));
    assert.ok(tables.has("todos"));
  });

  it("tracks db_version as lamport clock", () => {
    const v1 = db.prepare("SELECT crsql_db_version() AS v").get().v;
    db.exec("INSERT INTO todos VALUES (2, 'walk dog', 1)");
    const v2 = db.prepare("SELECT crsql_db_version() AS v").get().v;
    assert.ok(v2 > v1, `db_version should increase: ${v1} -> ${v2}`);
  });

  it("tracks updates in crsql_changes", () => {
    db.exec("UPDATE todos SET done = 1 WHERE id = 1");
    const changes = db.prepare(
      "SELECT * FROM crsql_changes WHERE [table] = 'todos' AND cid = 'done'"
    ).all();
    const row1Change = changes.find((c) => c.val === 1 || c.val === "1");
    assert.ok(row1Change, "should have a change for done = 1");
  });

  it("tracks deletes with causal length", () => {
    db.exec("DELETE FROM todos WHERE id = 2");
    // After delete, there should be a sentinel entry with even cl (deleted)
    const sentinel = db.prepare(
      "SELECT cl FROM crsql_changes WHERE [table] = 'todos' AND cid = '-1'"
    ).all();
    const deleted = sentinel.filter((r) => r.cl % 2 === 0);
    assert.ok(deleted.length > 0, "deleted row should have even causal length");
  });
});

describe("merge / sync", () => {
  it("merges changes between two databases", () => {
    const db1 = open();
    const db2 = open();

    // Set up same schema on both
    for (const db of [db1, db2]) {
      db.exec("CREATE TABLE items (id INTEGER PRIMARY KEY NOT NULL, name TEXT)");
      db.exec("SELECT crsql_as_crr('items')");
    }

    // Insert different rows
    db1.exec("INSERT INTO items VALUES (1, 'from-db1')");
    db2.exec("INSERT INTO items VALUES (2, 'from-db2')");

    // Get changes from db1 and apply to db2
    const changes1 = db1.prepare("SELECT * FROM crsql_changes").all();
    const insertStmt = db2.prepare(
      "INSERT INTO crsql_changes ([table], pk, cid, val, col_version, db_version, site_id, cl, seq) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
    );
    for (const c of changes1) {
      insertStmt.run(c.table, c.pk, c.cid, c.val, c.col_version, c.db_version, c.site_id, c.cl, c.seq);
    }

    // Get changes from db2 and apply to db1
    const changes2 = db2.prepare("SELECT * FROM crsql_changes WHERE db_version > 0").all();
    const insertStmt1 = db1.prepare(
      "INSERT INTO crsql_changes ([table], pk, cid, val, col_version, db_version, site_id, cl, seq) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
    );
    for (const c of changes2) {
      // Only apply changes from db2's site
      const db2SiteId = db2.prepare("SELECT crsql_site_id() AS sid").get().sid;
      const changeSiteId = c.site_id;
      // site_id comparison: apply non-local changes
      insertStmt1.run(c.table, c.pk, c.cid, c.val, c.col_version, c.db_version, c.site_id, c.cl, c.seq);
    }

    // Both should now have both rows
    const rows1 = db1.prepare("SELECT * FROM items ORDER BY id").all();
    const rows2 = db2.prepare("SELECT * FROM items ORDER BY id").all();

    assert.equal(rows1.length, 2);
    assert.equal(rows2.length, 2);
    assert.equal(rows1[0].name, "from-db1");
    assert.equal(rows1[1].name, "from-db2");
    assert.equal(rows2[0].name, "from-db1");
    assert.equal(rows2[1].name, "from-db2");

    for (const db of [db1, db2]) {
      db.exec("SELECT crsql_finalize()");
      db.close();
    }
  });

  it("resolves concurrent updates with last-write-wins", () => {
    const db1 = open();
    const db2 = open();

    for (const db of [db1, db2]) {
      db.exec("CREATE TABLE kv (key TEXT PRIMARY KEY NOT NULL, val TEXT)");
      db.exec("SELECT crsql_as_crr('kv')");
    }

    // Both write to the same key
    db1.exec("INSERT INTO kv VALUES ('x', 'db1-first')");
    db2.exec("INSERT INTO kv VALUES ('x', 'db2-first')");

    // Bump db1's version higher by doing more writes
    db1.exec("UPDATE kv SET val = 'db1-second' WHERE key = 'x'");
    db1.exec("UPDATE kv SET val = 'db1-third' WHERE key = 'x'");

    // Merge db1 -> db2
    const changes1 = db1.prepare("SELECT * FROM crsql_changes").all();
    const stmt = db2.prepare(
      "INSERT INTO crsql_changes ([table], pk, cid, val, col_version, db_version, site_id, cl, seq) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
    );
    for (const c of changes1) {
      stmt.run(c.table, c.pk, c.cid, c.val, c.col_version, c.db_version, c.site_id, c.cl, c.seq);
    }

    // db1's higher col_version should win
    const result = db2.prepare("SELECT val FROM kv WHERE key = 'x'").get();
    assert.equal(result.val, "db1-third");

    for (const db of [db1, db2]) {
      db.exec("SELECT crsql_finalize()");
      db.close();
    }
  });
});

describe("crsql_as_table (undo CRR)", () => {
  it("removes CRR tracking from a table", () => {
    const db = open();
    db.exec("CREATE TABLE temp_crr (id INTEGER PRIMARY KEY NOT NULL, data TEXT)");
    db.exec("SELECT crsql_as_crr('temp_crr')");

    // Clock table should exist
    let clocks = db.prepare(
      "SELECT name FROM sqlite_master WHERE name = 'temp_crr__crsql_clock'"
    ).all();
    assert.equal(clocks.length, 1);

    // Undo
    db.exec("SELECT crsql_as_table('temp_crr')");

    clocks = db.prepare(
      "SELECT name FROM sqlite_master WHERE name = 'temp_crr__crsql_clock'"
    ).all();
    assert.equal(clocks.length, 0);

    db.exec("SELECT crsql_finalize()");
    db.close();
  });
});

describe("helper module", () => {
  it("exports extensionPath as a string", async () => {
    const mod = await import("../nodejs-helper.js");
    assert.equal(typeof mod.extensionPath, "string");
    assert.ok(mod.extensionPath.endsWith("crsqlite"));
  });
});
