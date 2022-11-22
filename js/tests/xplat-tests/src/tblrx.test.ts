import { DBAsync, DB as DBSync } from "@vlcn.io/xplat-api";
type DB = DBAsync | DBSync;
import tblrx from "@vlcn.io/rx-tbl";

function createSimpleSchema(db: DB) {
  return db.execMany([
    "CREATE TABLE foo (a primary key, b);",
    "SELECT crsql_as_crr('foo');",
  ]);
}

export const tests = {
  "watches all non clock tables": async (
    dbProvider: () => Promise<DB>,
    assert: (p: boolean) => void
  ) => {
    const db = await dbProvider();
    await createSimpleSchema(db);
    const rx = await tblrx(db);

    assert(
      (
        await db.execA<number[]>(
          "SELECT count(*) FROM temp.sqlite_master WHERE type = 'trigger' AND name LIKE 'foo__crsql_tblrx_%'"
        )
      )[0][0] == 3
    );

    assert(rx.watching.length == 1);
    assert(rx.watching[0] == "foo");
  },

  // TODO: untestable in async db mode
  "collects all notifications till the next micro task": async (
    dbProvider: () => Promise<DB>,
    assert: (p: boolean) => void
  ) => {
    const db = await dbProvider();
    await createSimpleSchema(db);
    const rx = await tblrx(db);

    let notified = false;
    rx.on(() => {
      notified = true;
    });

    db.exec("INSERT INTO foo VALUES (1,2)");
    db.exec("INSERT INTO foo VALUES (2,3)");
    const last = db.exec("DELETE FROM foo WHERE a = 1");

    assert(notified == false);

    if (last && "then" in last) {
      await last;
    } else {
      await new Promise((resolve) => setTimeout(resolve, 0));
    }

    // @ts-ignore
    assert(notified == true);
  },

  "de-dupes tables": async (
    dbProvider: () => Promise<DB>,
    assert: (p: boolean) => void
  ) => {
    const db = await dbProvider();
    await createSimpleSchema(db);
    const rx = await tblrx(db);

    let notified = false;
    // tbls must always be a set
    rx.on((tbls: Set<string>) => {
      notified = true;
    });
  },

  "can be re-installed on schema change": async (
    dbProvider: () => Promise<DB>,
    assert: (p: boolean) => void
  ) => {
    const db = await dbProvider();
    await createSimpleSchema(db);
    const rx = await tblrx(db);

    db.exec("CREATE TABLE bar (a, b)");
    await rx.schemaChanged();

    assert(rx.watching.length == 2);

    assert(rx.watching[0] == "foo");
    assert(rx.watching[1] == "bar");
  },

  "does not fatal for connections that have not loaded the rx extension": (
    dbProvider: () => Promise<DB>,
    assert: (p: boolean) => void
  ) => {},

  "can exclude tables from rx": (
    dbProvider: () => Promise<DB>,
    assert: (p: boolean) => void
  ) => {},

  "disposes of listeners when asked": async (
    dbProvider: () => Promise<DB>,
    assert: (p: boolean) => void
  ) => {
    const db = await dbProvider();
    await createSimpleSchema(db);
    const rx = await tblrx(db);

    let notified = false;
    const disposer = rx.on(() => {
      notified = true;
    });

    db.exec("INSERT INTO foo VALUES (1,2)");
    db.exec("INSERT INTO foo VALUES (2,3)");
    const last = db.exec("DELETE FROM foo WHERE a = 1");

    assert(notified == false);
    disposer();

    if (last && "then" in last) {
      await last;
    } else {
      await new Promise((resolve) => setTimeout(resolve, 0));
    }

    assert(notified == false);
  },
} as const;
