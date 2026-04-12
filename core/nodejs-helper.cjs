// CJS entry point — see nodejs-helper.js for usage and resolution order.
const { join } = require("node:path");
const { existsSync } = require("node:fs");

const PLATFORM_PACKAGES = {
  "linux-x64": "@shards-lang/crsqlite-linux-x64",
  "darwin-arm64": "@shards-lang/crsqlite-darwin-arm64",
  "darwin-x64": "@shards-lang/crsqlite-darwin-x64",
  "win32-x64": "@shards-lang/crsqlite-win32-x64",
};

function resolve() {
  const key = `${process.platform}-${process.arch}`;
  const pkg = PLATFORM_PACKAGES[key];
  if (pkg) {
    try {
      return require(pkg).path;
    } catch {}
  }

  const buildPath = join(__dirname, "build", "crsqlite");
  if (
    existsSync(buildPath + ".so") ||
    existsSync(buildPath + ".dylib") ||
    existsSync(buildPath + ".dll")
  ) {
    return buildPath;
  }

  return join(__dirname, "dist", "crsqlite");
}

module.exports.extensionPath = resolve();
