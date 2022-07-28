import * as fs from "fs";
// @ts-ignore -- @types/better-sqlite3 is incorect on export
import Database from "better-sqlite3";
import { Database as DB } from "better-sqlite3";
import chalk from "chalk";
import { nanoid } from "nanoid";
import gatherTables from "./gatherTables.js";
import migrateTableSchemas from "./migrateTableSchema.js";
import copyData from "./copyData.js";

const crrSchemaVersion = 1;

/**
 * Migrate the source db file and write it to dest file.
 * Dest file will be overwritten!
 *
 * Optionally, provide a subset of tables to make conflict free rather
 * then making every table conflict free.
 *
 * TODO: migrate must somehow track if is being run against a db that was previously migrated...
 * TODO: alert on fk constraints being dropped
 *
 * @param sourceDbFile
 * @param destDbFile
 * @param tables
 */
export default function migrate(
  src: string,
  dest: string,
  tables?: string[],
  overwrite: boolean = false
) {
  if (dest !== ":memory:" && fs.existsSync(dest)) {
    if (!overwrite) {
      throw {
        type: "invariant",
        msg: `${dest} already exists. Please remove it or re-run with the --overwrite option.`,
      };
    } else {
      fs.unlinkSync(dest);
    }
  }

  if (src !== ":memory:" && !fs.existsSync(src)) {
    throw {
      type: "invariant",
      msg: `source database ${src} does not exist.`,
    };
  }

  const srcDb = new Database(src) as DB;
  srcDb.defaultSafeIntegers();
  const destDb = new Database(dest) as DB;
  destDb.defaultSafeIntegers();
  runMigrationSteps(srcDb, destDb, tables);
}

function runMigrationSteps(srcDb: DB, destDb: DB, tables?: string[]) {
  console.log(chalk.green("Creating common tables..."));
  createCommonTables(destDb);

  console.log(chalk.green("Populating common tables..."));
  populateCommonTables(destDb);

  // TODO: allow the user to specify a schema to migrate rather than just `main`
  const tablesToMigrate = gatherTables(srcDb, tables);
  console.log(
    chalk.green(
      "Migrating",
      chalk.underline.yellow(tablesToMigrate.length),
      "tables..."
    )
  );
  tablesToMigrate.forEach((t) => console.log(`\t${t}`));
  migrateTableSchemas(srcDb, destDb, tablesToMigrate);

  console.log(
    chalk.magenta(
      "\nFinished creating table schemas. Now copying over the data."
    )
  );
  tablesToMigrate.forEach((t) => copyData(srcDb, destDb, t));
}

function createCommonTables(db: DB) {
  console.log("\tcreation db version table");
  // TODO: move this to a sqlite extension rather than table.
  db.prepare(
    `CREATE TABLE IF NOT EXISTS "crr_db_version" (
    "id" INTEGER PRIMARY KEY CHECK (id = 0),
    "version" INTEGER DEFAULT 0
  );`
  ).run();

  console.log("\tcreation db site id table");
  // TODO: move this to a sqlite extension rather than table
  db.prepare(
    `CREATE TABLE IF NOT EXISTS "crr_site_id" (
    "invariant" INTEGER PRIMARY KEY CHECK (invariant = 0),
    "id" TEXT NOT NULL
  );`
  ).run();

  console.log("\tcreation migration metadata table");
  db.prepare(
    `CREATE TABLE IF NOT EXISTS "crr_migration_meta_table" (
    "crr_schema_version" INTEGER,
    "when" INTEGER
  )`
  ).run();
}

function populateCommonTables(db: DB) {
  console.log("\tsetting database version");
  db.prepare(`INSERT OR IGNORE INTO "crr_db_version" VALUES (0, 0);`).run();

  console.log("\tsetting db site id");
  db.prepare(
    `INSERT OR IGNORE INTO "crr_site_id" VALUES (0, '${nanoid()}');`
  ).run();

  console.log("\trecording migration start");
  db.prepare(
    `INSERT INTO crr_migration_meta_table VALUES(${crrSchemaVersion}, ${Math.floor(
      Date.now() / 1000
    )})`
  ).run();
}
