// Exports the path to the cr-sqlite loadable extension.
//
// Resolution order:
//   1. Platform-specific npm package (@shards-lang/crsqlite-{os}-{cpu})
//   2. Local build/ directory (development)
//   3. Local dist/ directory (legacy / manual builds)
//
// Usage with node:sqlite (Node >= 22.5):
//
//   import { extensionPath } from '@shards-lang/crsqlite';
//   import { DatabaseSync } from 'node:sqlite';
//   const db = new DatabaseSync(':memory:', { allowExtension: true });
//   db.loadExtension(extensionPath);
//
// Usage with better-sqlite3:
//
//   import { extensionPath } from '@shards-lang/crsqlite';
//   import Database from 'better-sqlite3';
//   const db = new Database(':memory:');
//   db.loadExtension(extensionPath);

import { createRequire } from "node:module";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { existsSync } from "node:fs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);

const PLATFORM_PACKAGES = {
  "linux-x64": "@shards-lang/crsqlite-linux-x64",
  "darwin-arm64": "@shards-lang/crsqlite-darwin-arm64",
  "darwin-x64": "@shards-lang/crsqlite-darwin-x64",
  "win32-x64": "@shards-lang/crsqlite-win32-x64",
};

function resolve() {
  // 1. Try the platform-specific npm package.
  const key = `${process.platform}-${process.arch}`;
  const pkg = PLATFORM_PACKAGES[key];
  if (pkg) {
    try {
      return require(pkg).path;
    } catch {}
  }

  // 2. Local build/ (dev).
  const buildPath = join(__dirname, "build", "crsqlite");
  if (
    existsSync(buildPath + ".so") ||
    existsSync(buildPath + ".dylib") ||
    existsSync(buildPath + ".dll")
  ) {
    return buildPath;
  }

  // 3. Local dist/ (legacy).
  return join(__dirname, "dist", "crsqlite");
}

export const extensionPath = resolve();
