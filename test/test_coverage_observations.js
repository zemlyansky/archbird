#!/usr/bin/env node
"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

if (process.argv.length !== 5) {
  throw new Error("usage: test_coverage_observations.js INDEX ROOT NATIVE");
}
process.env.ARCHBIRD_ENGINE = "native";
process.env.ARCHBIRD_NATIVE_ADDON = path.resolve(process.argv[4]);
const archbird = require(path.resolve(process.argv[2]));
const repositoryRoot = path.resolve(process.argv[3]);
const work = fs.mkdtempSync(path.join(repositoryRoot, "build", "tmp", "coverage-node-"));

function digest(bytes) {
  return crypto.createHash("sha256").update(bytes).digest("hex");
}

function add(files, name, bytes, symbols = []) {
  const target = path.join(work, name);
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, bytes);
  files.push({ path: name, sha256: digest(bytes), symbols });
}

function request(format, report) {
  return {
    artifact: "archbird-coverage-observation-request",
    cases: [{ path: "test/test.js", report: "case", selector: "suite.case" }],
    format,
    group: "suite",
    reports: [{ id: "case", path: report }],
    runner_paths: ["package.json"],
    schema_version: 1,
  };
}

try {
  const files = [];
  add(files, "package.json", Buffer.from('{"scripts":{"test":"node test.js"}}\n'));
  add(files, "test/test.js", Buffer.from("test('alpha', () => {});\n"));
  const source = Buffer.from('const emoji = "😀";\nfunction alpha() { return 1; }\n');
  add(files, "src/subject.js", source, [
    { kind: "function", line: 2, name: "alpha", public: true },
  ]);
  files.sort((left, right) => Buffer.compare(Buffer.from(left.path), Buffer.from(right.path)));
  const map = Buffer.from(JSON.stringify({
    artifact: "map",
    evidence: { config_sha256: "1".repeat(64), input_sha256: "2".repeat(64) },
    files,
    project: "coverage-fixture",
  }));

  const startOffset = source.toString("utf8").indexOf("function");
  assert.equal(startOffset, 20, "fixture must include a non-BMP UTF-16 code unit");
  fs.writeFileSync(path.join(work, "v8.json"), JSON.stringify({
    result: [{
      functions: [{ functionName: "alpha", ranges: [{ count: 2, endOffset: source.toString().length, startOffset }] }],
      url: `file://${path.join(work, "src/subject.js")}`,
    }],
  }));
  const observed = JSON.parse(archbird.compileTestObservations(
    map,
    Buffer.from(JSON.stringify(request("v8", "v8.json"))),
    { repository: work, requestDirectory: work },
  ));
  assert.deepEqual(observed.cases[0].symbols, [
    { hits: 2, path: "src/subject.js", symbol: "alpha" },
  ]);

  fs.writeFileSync(path.join(work, "istanbul.json"), JSON.stringify({
    [path.join(work, "src/subject.js")]: {
      f: { 0: 3 },
      fnMap: { 0: { decl: { start: { line: 2 } }, name: "alpha" } },
    },
  }));
  const istanbul = JSON.parse(archbird.compileTestObservations(
    map,
    Buffer.from(JSON.stringify(request("istanbul", "istanbul.json"))),
    { repository: work, requestDirectory: work },
  ));
  assert.equal(istanbul.cases[0].symbols[0].hits, 3);

  assert.throws(
    () => archbird.compileTestObservations(
      map,
      Buffer.from(JSON.stringify({
        ...request("coverage.py", "istanbul.json"),
        cases: [{
          context: "test/test.js::suite.case|run",
          path: "test/test.js",
          report: "case",
          selector: "suite.case",
        }],
      })),
      { repository: work, requestDirectory: work },
    ),
    /PyPI host/,
  );
  process.stdout.write("coverage observation Node adapter tests passed\n");
} finally {
  fs.rmSync(work, { recursive: true, force: true });
}
