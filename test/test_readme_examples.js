#!/usr/bin/env node
"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

if (process.argv.length !== 5) {
  throw new Error("usage: test_readme_examples.js REPOSITORY CLI ADDON");
}

const repository = path.resolve(process.argv[2]);
const cli = path.resolve(process.argv[3]);
const addon = path.resolve(process.argv[4]);
const temporaryRoot = path.join(repository, "build", "tmp");
fs.mkdirSync(temporaryRoot, { recursive: true });
const root = fs.mkdtempSync(path.join(temporaryRoot, "readme-node-"));
fs.mkdirSync(path.join(root, ".archbird"));

function write(relative, contents) {
  const target = path.join(root, relative);
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, contents);
}

function run(arguments_) {
  const result = spawnSync(process.execPath, [cli, ...arguments_], {
    cwd: root,
    encoding: "utf8",
    env: {
      ...process.env,
      ARCHBIRD_ENGINE: "native",
      ARCHBIRD_NATIVE_ADDON: addon,
    },
  });
  if (result.status !== 0) {
    throw new Error(
      `archbird ${arguments_.join(" ")} exited ${result.status}\n` +
      `stdout:\n${result.stdout}\nstderr:\n${result.stderr}`,
    );
  }
  return result.stdout;
}

try {
  fs.copyFileSync(
    path.join(repository, "examples", "minimal.archbird.json"),
    path.join(root, "archbird.json"),
  );
  fs.copyFileSync(
    path.join(repository, "examples", "minimal.verify.json"),
    path.join(root, "architecture.verify.json"),
  );
  write(
    "include/demo.h",
    "#ifndef DEMO_H\n#define DEMO_H\n" +
      "int demo_open(void);\nvoid demo_close(void);\n#endif\n",
  );
  write(
    "src/core.c",
    '#include "demo.h"\nint demo_open(void) { return 0; }\n' +
      "void demo_close(void) {}\n",
  );
  write(
    "python/demo/__init__.py",
    "class Client:\n    def open(self):\n        return 0\n",
  );
  write(
    "python/tests/test_client.py",
    "from demo import Client\n\n" +
      "def test_client_open():\n    assert Client().open() == 0\n",
  );
  write(
    "python/pyproject.toml",
    '[project]\nname = "demo"\nversion = "1.0.0"\n',
  );
  write("js/src/index.ts", "export function createClient(): object { return {}; }\n");
  write(
    "js/test/index.test.ts",
    "import { createClient } from '../src/index';\n" +
      "test('create client', () => { expect(createClient()).toBeTruthy(); });\n",
  );
  write(
    "js/package.json",
    '{"name":"demo-js","version":"1.0.0","exports":"./src/index.ts",' +
      '"scripts":{"test":"node --test"}}\n',
  );
  write("Makefile", "all:\n\t@echo demo\n");

  run(["map", ".", "--format", "json", "--output", ".archbird/map.json", "--check"]);
  run([
    "query", "--map", ".archbird/map.json", "--symbol", "demo_open",
    "--depth", "1", "--max-chars", "12000",
  ]);
  run([
    "verify", "--config", "architecture.verify.json", "--format", "json",
    "--output", "verification.json", "--check",
  ]);
  const verification = JSON.parse(fs.readFileSync(path.join(root, "verification.json")));
  assert.deepEqual(verification.summary.checks, {
    fail: 0,
    not_applicable: 0,
    pass: 1,
    unknown: 0,
    waived: 0,
  });

  process.env.ARCHBIRD_ENGINE = "native";
  process.env.ARCHBIRD_NATIVE_ADDON = addon;
  const { Project, auditMapFreshness } = require(path.join(repository, "js", "src", "index.js"));
  const project = Project.fromRepository(root, {
    config: path.join(root, "archbird.json"),
  });
  const mapJson = project.mapJson({ pretty: true });
  assert.equal(project.map().project, "demo");
  assert.ok(project.mapMarkdown({ maxChars: 12_000 }).length > 0);
  assert.ok(project.queryMarkdown({ symbols: ["demo_open"], depth: 1 }).length > 0);
  assert.equal(
    JSON.parse(project.graphViewJson({ view: "components" })).artifact,
    "archbird-graph-view",
  );
  assert.equal(
    JSON.parse(auditMapFreshness(mapJson, project.mapJson())).status,
    "current",
  );
  project.dispose();
  console.log("README quick-start passed through Node native host");
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
