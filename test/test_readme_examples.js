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

function execute(arguments_) {
  return spawnSync(process.execPath, [cli, ...arguments_], {
    cwd: root,
    encoding: "utf8",
    env: {
      ...process.env,
      ARCHBIRD_ENGINE: "native",
      ARCHBIRD_NATIVE_ADDON: addon,
    },
  });
}

function run(arguments_) {
  const result = execute(arguments_);
  if (result.status !== 0) {
    throw new Error(
      `archbird ${arguments_.join(" ")} exited ${result.status}\n` +
      `stdout:\n${result.stdout}\nstderr:\n${result.stderr}`,
    );
  }
  return result.stdout;
}

function markedNames(relative, name) {
  const text = fs.readFileSync(path.join(repository, relative), "utf8");
  const start = `<!-- ${name}:start -->`;
  const end = `<!-- ${name}:end -->`;
  assert.equal(text.split(start).length - 1, 1, `${relative}: ${name} start`);
  assert.equal(text.split(end).length - 1, 1, `${relative}: ${name} end`);
  const body = text.split(start, 2)[1].split(end, 1)[0];
  const names = [...body.matchAll(/`([^`\n]+)`/g)].map((match) => match[1]);
  assert.equal(new Set(names).size, names.length, `${relative}: duplicate ${name}`);
  return new Set(names);
}

try {
  fs.copyFileSync(
    path.join(repository, "examples", "quickstart.archbird.json"),
    path.join(root, "archbird.json"),
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

  const zeroConfig = JSON.parse(
    run(["--no-config", "--format", "json", "--check"]),
  );
  assert.ok(zeroConfig.files.length > 0);
  assert.ok(zeroConfig.project);
  const resolution = JSON.parse(run(["config", "show", "."]));
  assert.equal(resolution.artifact, "archbird-config-resolution");
  run([
    "config", "init", ".", "--output", ".archbird/generated.archbird.json",
  ]);
  const generatedConfig = JSON.parse(
    fs.readFileSync(path.join(root, ".archbird/generated.archbird.json")),
  );
  assert.equal(generatedConfig.schema_version, 2);
  assert.equal(Object.hasOwn(generatedConfig, "root"), false);

  const explicitMap = run(["map", ".", "--format", "json", "--check"]);
  assert.match(run([]), /^# demo architecture\n[\s\S]*\nMap `/);
  assert.equal(run(["--format", "json", "--check"]), explicitMap);
  assert.equal(run([".", "--format", "json", "--check"]), explicitMap);
  run(["map", ".", "--format", "json", "--output", ".archbird/map.json", "--check"]);
  run([
    "query", "--map", ".archbird/map.json", "--symbol", "demo_open",
    "--depth", "1", "--max-chars", "12000",
  ]);
  const liveQuery = JSON.parse(run([
    "query", ".", "--symbol", "demo_open", "--depth", "1",
    "--format", "json", "--check",
  ]));
  assert.equal(liveQuery.artifact, "query");
  run([
    "query", "public-api-impact", "--map", ".archbird/map.json",
    "--config", "archbird.json", "--format", "json",
    "--output", ".archbird/public-api-impact.json",
  ]);
  run([
    "verify", "--map", ".archbird/map.json", "--format", "json",
    "--output", "verification.json", "--check",
  ]);
  const verification = JSON.parse(fs.readFileSync(path.join(root, "verification.json")));
  assert.deepEqual(verification.summary.constraints, {
    fail: 0,
    not_applicable: 0,
    pass: 2,
    unknown: 0,
    waived: 0,
  });

  process.env.ARCHBIRD_ENGINE = "native";
  process.env.ARCHBIRD_NATIVE_ADDON = addon;
  const api = require(path.join(repository, "js", "src", "index.js"));
  const { Project, auditMapFreshness } = api;
  for (const relative of ["README.md", "js/README.md"]) {
    assert.deepEqual(
      markedNames(relative, "archbird-node-api"),
      new Set(Object.keys(api)),
      `${relative}: Node API inventory drifted`,
    );
  }
  const cliSource = fs.readFileSync(path.join(repository, "js", "src", "cli.js"), "utf8");
  const commandBlock = cliSource.match(/const COMMANDS = new Set\(\[([\s\S]*?)\]\);/);
  assert.ok(commandBlock, "Node command inventory is unavailable");
  const commands = new Set(
    [...commandBlock[1].matchAll(/"([^"]+)"/g)].map((match) => match[1]),
  );
  for (const relative of ["README.md", "js/README.md"]) {
    assert.deepEqual(
      markedNames(relative, "archbird-node-cli"),
      commands,
      `${relative}: Node CLI inventory drifted`,
    );
  }
  const packageDocument = JSON.parse(
    fs.readFileSync(path.join(repository, "js", "package.json"), "utf8"),
  );
  const entrypoints = new Set(
    Object.keys(packageDocument.exports).map((name) => (
      name === "." ? packageDocument.name : packageDocument.name + name.slice(1)
    )),
  );
  for (const relative of ["README.md", "js/README.md"]) {
    assert.deepEqual(
      markedNames(relative, "archbird-node-entrypoints"),
      entrypoints,
      `${relative}: npm entrypoint inventory drifted`,
    );
  }
  const browserSource = fs.readFileSync(
    path.join(repository, "js", "src", "browser.js"),
    "utf8",
  );
  const browserFacade = browserSource.match(
    /return Object\.freeze\(\{([\s\S]*?)\n  \}\);\n\}/,
  );
  assert.ok(browserFacade, "browser facade inventory is unavailable");
  const browserNames = new Set(
    [...browserFacade[1].matchAll(/^\s{4}([A-Za-z][A-Za-z0-9_]*)(?=:|,)/gm)]
      .map((match) => match[1]),
  );
  for (const relative of ["README.md", "js/README.md"]) {
    assert.deepEqual(
      markedNames(relative, "archbird-browser-api"),
      browserNames,
      `${relative}: browser API inventory drifted`,
    );
  }
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
