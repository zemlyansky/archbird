"use strict";

const { createBrowserArchbird } = require("archbird/browser");
const { scipFixture } = require("./scip_fixture");

const config = {
  schema_version: 2,
  project: "browser-release",
  description: "Real-browser Wasm release smoke.",
  layers: [
    {
      name: "javascript",
      role: "frontend",
      language: "javascript",
      globs: ["src/**/*.js"],
    },
  ],
  components: [
    { name: "frontend", paths: ["src/**"] },
    { name: "shared", paths: ["src/defs.js"] },
  ],
  indexes: [
    {
      name: "compiler",
      format: "scip",
      path: "index.scip",
      position_encoding_fallback: "utf8",
    },
  ],
  constraints: {
    EXCLUSIVE: {
      kind: "component_membership",
      max: 1,
      owner: "test",
      rationale: "Exercise Wasm component-membership verification.",
    },
  },
};

(async () => {
  const archbird = await createBrowserArchbird({
    locateFile: (name) => (name === "archbird.wasm" ? "/archbird.wasm" : name),
  });
  const project = new archbird.Project(JSON.stringify(config), [
    new archbird.Source("index.scip", scipFixture()),
    new archbird.Source(
      "src/defs.js",
      "export function add(a, b) { return a + b; }\n",
    ),
    new archbird.Source(
      "src/use.js",
      'import { add } from "./defs.js";\nexport const value = add(1, 2);\n',
    ),
  ], { typescript: false });
  const mapBytes = project.mapJson();
  const map = JSON.parse(mapBytes.toString("utf8"));
  const freshness = JSON.parse(
    archbird.auditMapFreshness(mapBytes, mapBytes).toString("utf8"),
  );
  const verification = JSON.parse(
    archbird.core.constraintsEvaluate(
      JSON.stringify(config),
      mapBytes,
      "",
      "",
    ).toString("utf8"),
  );
  project.dispose();
  document.body.textContent = JSON.stringify({
    engine: archbird.ENGINE.kind,
    files: map.files.length,
    freshness: freshness.status,
    indexes: map.indexes.length,
    membershipFinding: verification.constraints[0].findings[0].key,
    membershipOverlap: verification.constraints[0].findings[0].evidence[0].path,
    project: map.project,
    semanticEdges: map.edges.filter((edge) => edge.kind === "semantic-reference").length,
    version: archbird.VERSION,
  });
})().catch((error) => {
  document.body.textContent = `ERROR: ${error.stack || error}`;
});
