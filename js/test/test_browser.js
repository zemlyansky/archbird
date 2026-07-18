"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { scipFixture } = require("../../test/scip_fixture");

if (process.argv.length !== 4) {
  throw new Error("usage: test_browser.js PACKAGE_ROOT REPOSITORY_ROOT");
}

const packageRoot = path.resolve(process.argv[2]);
const repository = path.resolve(process.argv[3]);
const { createBrowserArchbird } = require(path.join(packageRoot, "src/browser.js"));

(async () => {
  const archbird = await createBrowserArchbird({
    wasmBinary: fs.readFileSync(path.join(packageRoot, "wasm/archbird.wasm")),
  });
  assert.equal(archbird.ENGINE.kind, "wasm");
  assert.equal(archbird.VERSION, "0.0.1");
  const fixture = path.join(repository, "test/fixtures/map_base");
  const config = fs.readFileSync(path.join(fixture, "archbird.json"));
  const sources = ["js/index.js", "py/pkg/__init__.py", "py/pkg/api.py"].map(
    (sourcePath) => new archbird.Source(
      sourcePath,
      fs.readFileSync(path.join(fixture, sourcePath)),
    ),
  );
  const project = new archbird.Project(config, sources, { typescript: false });
  assert.match(project.mapInputSha256, /^[0-9a-f]{64}$/);
  const first = project.mapJson();
  assert.deepEqual(first, project.mapJson());
  assert.equal(JSON.parse(first).project, "map-base");
  assert.equal(
    JSON.parse(archbird.auditMapFreshness(first, first)).status,
    "current",
  );
  assert.equal(project.queryJson({ paths: ["py/pkg"], depth: 0 }).length > 0, true);
  const contextPolicy = { profile: "exact", quotas: { files: 1 } };
  const contextQuery = JSON.parse(
    project.queryJson({ paths: ["py/pkg"], depth: 0, context: contextPolicy }),
  );
  assert.deepEqual(contextQuery.query.context, contextPolicy);
  assert.equal(contextQuery.files.length, 2);
  assert.match(
    project.queryMarkdown({
      paths: ["py/pkg"],
      depth: 0,
      context: contextPolicy,
    }).toString("utf8"),
    /Context: profile=exact;.*files=1\/2\./,
  );
  assert.match(project.mapMarkdown().toString("utf8"), /^# map-base/);
  const componentGraphJson = project.graphViewJson();
  const symbolGraphJson = project.graphViewJson({
    view: "symbols",
    query: { symbols: ["js/index.js:add"], depth: 1, testDepth: 1 },
  });
  assert.equal(JSON.parse(componentGraphJson).artifact, "archbird-graph-view");
  assert.equal(JSON.parse(componentGraphJson).request.view, "components");
  assert.equal(JSON.parse(symbolGraphJson).request.view, "symbols");
  assert.ok(
    JSON.parse(symbolGraphJson).nodes.some(
      (node) => node.kind === "symbol" && node.label === "add",
    ),
  );
  project.dispose();
  const scipConfig = {
    schema_version: 1,
    project: "scip-browser",
    layers: [
      {
        name: "javascript",
        language: "javascript",
        globs: ["src/**/*.js"],
      },
    ],
    indexes: [
      {
        name: "compiler",
        format: "scip",
        path: "index.scip",
        position_encoding_fallback: "utf8",
      },
    ],
  };
  const scipProject = archbird.Project.fromFiles([
    new archbird.Source("index.scip", scipFixture()),
    new archbird.Source(
      "src/defs.js",
      Buffer.from("export function add(a, b) { return a + b; }\n"),
    ),
    new archbird.Source(
      "src/use.js",
      Buffer.from('import { add } from "./defs.js";\nexport const value = add(1, 2);\n'),
    ),
  ], { config: JSON.stringify(scipConfig), typescript: false });
  const scipMap = scipProject.map();
  assert.equal(scipMap.indexes[0].coverage.documents_mapped, 2);
  assert.equal(scipMap.indexes[0].coverage.position_encoding_fallback_documents, 2);
  assert.equal(scipMap.indexes[0].position_encoding_fallback, "utf8");
  assert.ok(
    scipMap.edges.some(
      (edge) =>
        edge.kind === "semantic-reference" &&
        edge.source === "src/use.js" &&
        edge.target === "src/defs.js" &&
        edge.names.includes("add"),
    ),
  );
  scipProject.dispose();
  const virtual = archbird.Project.fromFiles([
    new archbird.Source(
      "package.json",
      Buffer.from('{"name":"@archbird/browser-fixture","version":"1.0.0"}'),
    ),
    new archbird.Source(".gitignore", Buffer.from("ignored.py\n")),
    new archbird.Source("ignored.py", Buffer.from("def ignored(): pass\n")),
    new archbird.Source("src/main.py", Buffer.from("def main(): return 1\n")),
    new archbird.Source("tests/test_main.py", Buffer.from("def test_main(): pass\n")),
    new archbird.Source("vendor/lib.c", Buffer.from("int vendor(void) { return 1; }\n")),
  ], { typescript: false });
  const virtualResolution = JSON.parse(virtual.resolutionJson.toString("utf8"));
  assert.equal(virtualResolution.project, "browser-fixture");
  assert.deepEqual(
    virtualResolution.files.map((row) => row.path),
    ["package.json", "src/main.py", "tests/test_main.py", "vendor/lib.c"],
  );
  const virtualMap = virtual.map();
  assert.deepEqual(
    virtualMap.files
      .filter((row) => row.roles)
      .map((row) => [row.path, row.roles]),
    [
      ["tests/test_main.py", ["test-candidate"]],
      ["vendor/lib.c", ["third-party-candidate"]],
    ],
  );
  assert.equal(virtualMap.tests.length, 1);
  assert.equal(virtualMap.tests[0].inventory_state, "candidate");
  assert.equal(virtualMap.tests[0].cases[0].selector, "test_main");
  virtual.dispose();
  const virtualR = archbird.Project.fromFiles([
    new archbird.Source(
      "DESCRIPTION",
      Buffer.from("Package: browserR\nVersion: 3.2.1\n"),
    ),
    new archbird.Source("NAMESPACE", Buffer.from("export(alpha, beta)\n")),
    new archbird.Source(
      "R/api.R",
      Buffer.from("alpha <- function(x) x\nbeta <- function(x) alpha(x)\n"),
    ),
  ], { typescript: false });
  const virtualRMap = virtualR.map();
  assert.equal(virtualRMap.project, "browserR");
  assert.equal(virtualRMap.packages.length, 1);
  assert.equal(virtualRMap.packages[0].identity, "browserR");
  assert.equal(virtualRMap.packages[0].version, "3.2.1");
  assert.deepEqual(virtualRMap.packages[0].exports, ["alpha", "beta"]);
  virtualR.dispose();
  console.log("packaged browser/Wasm Map passed");
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
