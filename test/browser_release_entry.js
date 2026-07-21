"use strict";

const { createBrowserArchbird } = require("archbird/browser");
const { scipFixture } = require("./scip_fixture");

const config = {
  schema_version: 1,
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
  const suite = JSON.stringify({
    schema_version: 1,
    suite: "browser-membership",
    projects: { subject: { map: "ARCHBIRD.json" } },
    extractors: {
      membership: { kind: "component_membership", project: "subject" },
    },
    checks: [{
      id: "EXCLUSIVE",
      assert: "numeric_bounds",
      actual: "membership",
      max: 1,
      owner: "test",
      rationale: "Exercise Wasm membership verification.",
    }],
  });
  const verificationInput = JSON.stringify({
    artifact: "verification-input",
    schema_version: 1,
    suite_path: "browser.verify.json",
    projects: [{ name: "subject", map, sources: [] }],
    provided_facts: [],
    attestations: [],
    baseline: null,
  });
  const verification = JSON.parse(
    archbird.core.verificationAnalyze(suite, verificationInput).toString("utf8"),
  );
  const overlap = JSON.parse(archbird.core.verificationDebug(
    suite,
    verificationInput,
    JSON.stringify({
      artifact: "verification-debug-request",
      schema_version: 1,
      view: "overlap",
    }),
  ).toString("utf8"));
  project.dispose();
  document.body.textContent = JSON.stringify({
    engine: archbird.ENGINE.kind,
    files: map.files.length,
    freshness: freshness.status,
    indexes: map.indexes.length,
    membershipFinding: verification.checks[0].findings[0].key,
    membershipOverlap: overlap.memberships[0].files[0].path,
    project: map.project,
    semanticEdges: map.edges.filter((edge) => edge.kind === "semantic-reference").length,
    version: archbird.VERSION,
  });
})().catch((error) => {
  document.body.textContent = `ERROR: ${error.stack || error}`;
});
