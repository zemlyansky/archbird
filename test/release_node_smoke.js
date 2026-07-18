"use strict";

const fs = require("node:fs");
const path = require("node:path");

if (process.argv.length !== 5) {
  throw new Error("usage: release_node_smoke.js INSTALLED_PACKAGE CONFIG ROOT");
}
const packageRoot = path.resolve(process.argv[2]);
const archbird = require(packageRoot);
const metadata = JSON.parse(fs.readFileSync(path.join(packageRoot, "package.json")));
if (metadata.version !== "0.0.1") {
  throw new Error(`unexpected version: ${metadata.version}`);
}
const project = archbird.Project.fromConfig(path.resolve(process.argv[3]), {
  root: path.resolve(process.argv[4]),
});
const first = project.mapJson();
if (!first.equals(project.mapJson())) {
  throw new Error("installed Node Map is not deterministic");
}
const document = JSON.parse(first);
if (document.artifact !== "map" || document.project !== "map-base") {
  throw new Error("installed Node package returned the wrong map");
}
if (document.diagnostics.some((row) => row.severity === "error")) {
  throw new Error(JSON.stringify(document.diagnostics));
}
if (JSON.parse(archbird.auditMapFreshness(first, first)).status !== "current") {
  throw new Error("installed Node freshness audit failed");
}
const query = project.query({ paths: ["py/pkg"], depth: 0 });
if (query.artifact !== "query" || query.files.length !== 2) {
  throw new Error("installed Node query failed");
}
const graph = JSON.parse(project.graphViewJson());
const symbolGraph = JSON.parse(project.graphViewJson({
  view: "symbols",
  query: { symbols: ["js/index.js:add"], depth: 1 },
}));
if (graph.artifact !== "archbird-graph-view" || graph.source.artifact !== "map") {
  throw new Error("installed Node component graph failed");
}
if (symbolGraph.request.view !== "symbols" || symbolGraph.source.artifact !== "query") {
  throw new Error("installed Node symbol graph failed");
}
const symbols = document.files.reduce((count, row) => count + row.symbols.length, 0);
project.dispose();
project.dispose();
console.log(`node release smoke passed: files=${document.files.length} symbols=${symbols}`);
