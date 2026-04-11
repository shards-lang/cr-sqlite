// Exports the path to the cr-sqlite loadable extension.
// Usage with node:sqlite (Node >= 22.5):
//
//   import { extensionPath } from '@anthropic/crsqlite';
//   import { DatabaseSync } from 'node:sqlite';
//   const db = new DatabaseSync(':memory:', { allowExtension: true });
//   db.loadExtension(extensionPath);
//
// Usage with better-sqlite3:
//
//   import { extensionPath } from '@anthropic/crsqlite';
//   import Database from 'better-sqlite3';
//   const db = new Database(':memory:');
//   db.loadExtension(extensionPath);

import { fileURLToPath } from "node:url";
import { join, dirname } from "node:path";

const __dirname = dirname(fileURLToPath(import.meta.url));
export const extensionPath = join(__dirname, "dist", "crsqlite");
