#!/usr/bin/env node
"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
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

const coherenceDirectory = fs.mkdtempSync(
  path.join(repository, "build", "query-coherence-"),
);
try {
  const currentMapPath = path.join(coherenceDirectory, "current-map.json");
  const mismatchedMapPath = path.join(coherenceDirectory, "mismatched-map.json");
  fs.writeFileSync(currentMapPath, always.stdout);
  const mismatched = JSON.parse(always.stdout);
  mismatched.tool.implementation_sha256 = "0".repeat(64);
  fs.writeFileSync(mismatchedMapPath, JSON.stringify(mismatched));
  const runQuery = (mapPath, check) => spawnSync(process.execPath, [
    cli, "query", "--map", mapPath, "--path", "js", "--depth", "0",
    "--format", "json", ...(check ? ["--check"] : []),
  ], {
    encoding: "utf8",
    env: {
      ...process.env,
      ARCHBIRD_ENGINE: "native",
      ARCHBIRD_NATIVE_ADDON: addon,
    },
  });
  const checkedCurrent = runQuery(currentMapPath, true);
  assert.equal(checkedCurrent.status, 0, checkedCurrent.stderr);
  assert.equal(JSON.parse(checkedCurrent.stdout).artifact, "query");
  const brief = spawnSync(process.execPath, [
    cli, "query", "--map", currentMapPath, "--path", "js", "--depth", "0",
    "--view", "changes", "--detail", "compact",
  ], {
    encoding: "utf8",
    env: {
      ...process.env,
      ARCHBIRD_ENGINE: "native",
      ARCHBIRD_NATIVE_ADDON: addon,
    },
  });
  assert.equal(brief.status, 0, brief.stderr);
  assert.match(brief.stdout, /^# Change brief:/);
  assert.match(brief.stdout, /## Evidence limits/);
  const blocked = runQuery(mismatchedMapPath, true);
  assert.equal(blocked.status, 1, blocked.stderr);
  assert.equal(blocked.stdout, "");
  assert.match(blocked.stderr, /saved Map core .* does not match active core/);
  const missingProducerMapPath = path.join(
    coherenceDirectory,
    "missing-producer-map.json",
  );
  const missingProducer = JSON.parse(always.stdout);
  delete missingProducer.tool.implementation_sha256;
  fs.writeFileSync(missingProducerMapPath, JSON.stringify(missingProducer));
  const missingProducerBlocked = runQuery(missingProducerMapPath, true);
  assert.equal(missingProducerBlocked.status, 1, missingProducerBlocked.stderr);
  assert.equal(missingProducerBlocked.stdout, "");
  assert.match(missingProducerBlocked.stderr, /digest is missing or invalid/);
  const crossVersion = runQuery(mismatchedMapPath, false);
  assert.equal(crossVersion.status, 0, crossVersion.stderr);
  const crossVersionDocument = JSON.parse(crossVersion.stdout);
  assert.equal(
    crossVersionDocument.source_tool.implementation_sha256,
    "0".repeat(64),
  );
  assert.notEqual(
    crossVersionDocument.tool.implementation_sha256,
    crossVersionDocument.source_tool.implementation_sha256,
  );
} finally {
  fs.rmSync(coherenceDirectory, { force: true, recursive: true });
}
console.log("Node CLI progress isolation passed");
