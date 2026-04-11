// CJS entry point — see nodejs-helper.js for usage examples.
const { join } = require("node:path");
const extensionPath = join(__dirname, "dist", "crsqlite");
module.exports = { extensionPath };
