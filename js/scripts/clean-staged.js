"use strict";

const fs = require("node:fs");
const path = require("node:path");

const packageRoot = path.resolve(__dirname, "..");
for (const [name, artifact] of [
  ["native", "archbird-node-native-directory"],
  ["wasm", "archbird-wasm-directory"],
  ["app", "archbird-visualization-application"],
]) {
  const directory = path.join(packageRoot, name);
  if (!fs.existsSync(directory)) continue;
  const marker = path.join(directory, ".archbird-meta.json");
  const document = fs.existsSync(marker)
    ? JSON.parse(fs.readFileSync(marker, "utf8"))
    : null;
  if (document?.artifact !== artifact) {
    throw new Error(`refusing to clean an unrecognized ${name} directory`);
  }
  fs.rmSync(directory, { recursive: true });
}
fs.rmSync(path.join(packageRoot, "src/generated/identities.json"), { force: true });
try {
  fs.rmdirSync(path.join(packageRoot, "src/generated"));
} catch (_) {
  // Keep a non-empty developer-owned directory.
}
fs.rmSync(path.join(packageRoot, "LICENSE"), { force: true });
