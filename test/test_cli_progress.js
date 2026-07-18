#!/usr/bin/env node
"use strict";

const assert = require("node:assert/strict");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

if (process.argv.length !== 5) {
  throw new Error("usage: test_cli_progress.js CLI REPOSITORY ADDON");
}
const cli = path.resolve(process.argv[2]);
const repository = path.resolve(process.argv[3]);
const addon = path.resolve(process.argv[4]);
const fixture = path.join(repository, "test/fixtures/map_base");

function run(mode) {
  const result = spawnSync(process.execPath, [
    cli, "map", fixture, "--config", path.join(fixture, "archbird.json"),
    "--progress", mode, "--no-cache", "--format", "json", "--check",
  ], {
    encoding: "utf8",
    env: {
      ...process.env,
      ARCHBIRD_ENGINE: "native",
      ARCHBIRD_NATIVE_ADDON: addon,
    },
  });
  if (result.status !== 0) {
    throw new Error(`Node progress CLI failed: ${result.stderr}`);
  }
  return result;
}

const always = run("always");
const automatic = run("auto");
assert.equal(always.stdout, automatic.stdout);
assert.equal(JSON.parse(always.stdout).project, "map-base");
for (const phase of ["discovery", "selected", "providers", "joining", "rendering", "complete"]) {
  assert.match(always.stderr, new RegExp(`\\] ${phase}:`));
}
assert.equal(automatic.stderr, "");
console.log("Node CLI progress isolation passed");
