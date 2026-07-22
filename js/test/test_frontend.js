"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const { scipFixture } = require("../../test/scip_fixture");
const { ProviderCache } = require("../src/provider-cache");

if (process.argv.length !== 4) {
  throw new Error("usage: test_node_frontend.js ADDON REPOSITORY_ROOT");
}
process.env.ARCHBIRD_NATIVE_ADDON = path.resolve(process.argv[2]);
const {
  ChangeProposal,
  auditMapFreshness,
  analyzeOkfSource,
  compileProjectConfiguration,
  compileQueryPlan,
  evaluateConstraints,
  exportGraph,
  freezeConstraints,
  IMPLEMENTATION_SHA256,
  Project,
  publishOkfBundle,
  queryMap,
  queryMapMarkdown,
  renderMapMarkdown,
  resolveDiscovery,
  reportConstraints,
  Source,
  Workspace,
  jsonCanonicalize,
  NATIVE_ABI_VERSION,
  PATTERN_CONTRACT,
  PATTERN_CONTRACT_VERSION,
  PATTERN_ENGINE,
  PATTERN_OPTIONS,
  PATTERN_UNICODE,
} = require(path.resolve(process.argv[3], "js/src/index.js"));

assert.equal(NATIVE_ABI_VERSION, 0);
assert.equal(PATTERN_CONTRACT_VERSION, 1);
assert.equal(PATTERN_CONTRACT, "archbird-pcre2-v1");
assert.equal(PATTERN_ENGINE, "PCRE2 10.47");
assert.equal(PATTERN_UNICODE, "UCD 16.0.0");
assert.equal(
  PATTERN_OPTIONS,
  "UTF,UCP,NEWLINE_LF,BSR_UNICODE,NEVER_BACKSLASH_C,NEVER_CALLOUT,JIT_DISABLED",
);
assert.deepEqual(
  jsonCanonicalize(Buffer.from('{"b":2,"a":1}')),
  Buffer.from('{"a":1,"b":2}'),
);

const freshnessMap = {
  artifact: "map",
  discovery: { sha256: "3".repeat(64) },
  evidence: {
    config_sha256: "2".repeat(64),
    input_sha256: "1".repeat(64),
  },
  files: [
    { path: "src/a.js", sha256: "a".repeat(64) },
  ],
  project: "node-freshness",
  schema_version: 7,
  tool: {
    implementation_sha256: "4".repeat(64),
    name: "archbird",
    version: "fixture",
  },
};
const freshnessCurrent = JSON.parse(
  auditMapFreshness(
    Buffer.from(JSON.stringify(freshnessMap)),
    Buffer.from(JSON.stringify(freshnessMap)),
  ),
);
assert.equal(freshnessCurrent.status, "current");
const freshnessChanged = structuredClone(freshnessMap);
freshnessChanged.evidence.input_sha256 = "5".repeat(64);
freshnessChanged.files[0].sha256 = "b".repeat(64);
const freshnessStale = JSON.parse(
  auditMapFreshness(
    Buffer.from(JSON.stringify(freshnessMap)),
    Buffer.from(JSON.stringify(freshnessChanged)),
  ),
);
assert.equal(freshnessStale.status, "stale");
assert.deepEqual(freshnessStale.files.changed.map((row) => row.path), ["src/a.js"]);

const metricFixture = path.resolve(
  process.argv[3],
  "build/test-constraints-node",
);
fs.rmSync(metricFixture, { force: true, recursive: true });
fs.mkdirSync(path.join(metricFixture, "src"), { recursive: true });
fs.writeFileSync(path.join(metricFixture, "src/small.js"), "x=1;\n");
fs.writeFileSync(path.join(metricFixture, "src/large.js"), "x".repeat(20));
const constraintConfiguration = {
  schema_version: 2,
  project: "constraints-node",
  limits: { max_file_bytes: 100 },
  layers: [{
    name: "javascript",
    role: "core",
    language: "javascript",
    globs: ["src/**/*.js"],
  }],
  components: [
    { name: "primary", paths: ["src/small.js"] },
    { name: "shared", paths: ["src/small.js"] },
  ],
  projections: {
    "large-source": { paths: ["src/large.js"], select: "mapped_paths" },
  },
  queries: {
    "large-impact": { depth: 0, projection: "large-source" },
  },
  constraints: {
    "MAX-FILE-BYTES": {
      kind: "max_file_bytes",
      max: 10,
      include: ["src/**"],
      owner: "test",
      rationale: "Every selected source remains within the reviewed size bound.",
    },
    "COMPONENT-MEMBERSHIP": {
      kind: "component_membership",
      min: 1,
      owner: "test",
      rationale: "Every mapped source belongs to a component.",
    },
    "STABLE-LITERAL": {
      assert: "set_equal",
      actual: { literal: ["stable"] },
      expected: { literal: ["stable"] },
      owner: "test",
      rationale: "A passing sibling constraint can be preserved by Act.",
    },
  },
};
const constraintConfig = Buffer.from(JSON.stringify(constraintConfiguration));
fs.writeFileSync(path.join(metricFixture, "archbird.json"), constraintConfig);
const compiledConfiguration = JSON.parse(compileProjectConfiguration(constraintConfig));
assert.equal(compiledConfiguration.map_definition.schema_version, 2);
assert.equal(compiledConfiguration.constraint_policy_sha256.length, 64);
const metricProject = Project.fromRepository(metricFixture, {
  config: constraintConfig,
  scan: false,
});
metricProject.finalizeProviders();
const metricMap = metricProject.mapJson();
const metricQueryPlanArtifact = JSON.parse(
  compileQueryPlan(constraintConfig, "large-impact"),
);
assert.equal(metricQueryPlanArtifact.artifact, "query-plan");
assert.equal(metricQueryPlanArtifact.schema_version, 2);
assert.equal(metricQueryPlanArtifact.plan.projections.length, 1);
assert.equal(
  Object.hasOwn(
    metricQueryPlanArtifact.plan.projections[0],
    "projection_result_sha256",
  ),
  false,
);
const metricQuery = JSON.parse(queryMap(metricMap, {
  plan: metricQueryPlanArtifact.plan,
  resolutionJson: metricProject.resolutionJson,
}));
assert.deepEqual(metricQuery.files.map((row) => row.path), ["src/large.js"]);
assert.equal(metricQuery.query.projection_results.length, 1);
const metricVerification = evaluateConstraints(constraintConfig, metricMap, {
  resolutionJson: metricProject.resolutionJson,
});
const metricResult = JSON.parse(metricVerification);
assert.equal(metricResult.artifact, "verification");
assert.equal(metricResult.schema_version, 2);
assert.equal(metricResult.constraints[0].id, "COMPONENT-MEMBERSHIP");
assert.equal(metricResult.constraints[0].status, "fail");
assert.equal(metricResult.constraints[1].id, "MAX-FILE-BYTES");
assert.equal(metricResult.constraints[1].status, "fail");
assert.deepEqual(
  metricResult.constraints[1].findings.map((row) => row.key),
  ["src/large.js"],
);
assert.equal(metricResult.constraints[2].id, "STABLE-LITERAL");
assert.equal(metricResult.constraints[2].status, "pass");
const markdownConstraints = reportConstraints(constraintConfig, metricMap, {
  resolutionJson: metricProject.resolutionJson,
  format: "markdown",
});
assert.match(markdownConstraints.toString("utf8"), /src\/large\.js/);
const frozenConstraints = JSON.parse(freezeConstraints(constraintConfig, metricMap, {
  resolutionJson: metricProject.resolutionJson,
  owner: "architecture",
  rationale: "Review the current constraint debt.",
}));
assert.equal(frozenConstraints.artifact, "constraint-baseline");
const metricFinding = metricResult.constraints
  .find((row) => row.id === "MAX-FILE-BYTES").findings[0];
const metricProposal = ChangeProposal.compile(
  metricVerification,
  metricFinding.fingerprint,
);
assert.equal(metricProposal.data().origin.constraint, "MAX-FILE-BYTES");
const metricContract = metricProposal.review({
  objective: "Reduce the oversized selected source.",
  owner: "test",
  rationale: "Exercise the native Node constraint lifecycle.",
  preserveConstraints: ["STABLE-LITERAL"],
  selectedCandidates: metricProposal.data().candidates.map((row) => row.id),
});
assert.deepEqual(
  metricContract.data().preserved_constraints.map((row) => row.id),
  ["STABLE-LITERAL"],
);
assert.equal(
  JSON.parse(metricContract.verify(metricVerification, metricVerification)).status,
  "missing",
);
metricProject.dispose();

const constraintCli = path.resolve(process.argv[3], "js/src/cli.js");
const cliFailure = spawnSync(process.execPath, [
  constraintCli,
  "verify", "MAX-FILE-BYTES", "--root", metricFixture,
  "--format", "json", "--check", "--progress", "never",
], { encoding: "utf8", env: process.env });
assert.equal(cliFailure.status, 1, cliFailure.stderr);
assert.deepEqual(
  JSON.parse(cliFailure.stdout).constraints[0].findings.map((row) => row.key),
  ["src/large.js"],
);
fs.rmSync(metricFixture, { force: true, recursive: true });

const source = Buffer.from(`
const fs = require("node:fs");
function render(x) { return helper(x); }
class Model { run(x) { return this.render(x); } }
module.exports = { render, Model };
exports.alias = render;
postMessage({type: "ready"});
function receive(event) { switch (event.data.type) { case "work": return render(event.data); } }
`);
let project = new Project("node-test", [
  new Source("js/core.js", source, { language: "javascript", layer: "js" }),
]);
project.scan();
if (!/^[0-9a-f]{64}$/.test(IMPLEMENTATION_SHA256)) {
  throw new Error("native core implementation identity is invalid");
}
const first = project.fileFactsJson();
const second = project.fileFactsJson();
assert.deepEqual(first, second);
const artifact = JSON.parse(first.toString("utf8"));
assert.equal(artifact.source_manifest_sha256, project.manifestSha256);
assert.equal(artifact.files.length, 1);
const row = artifact.files[0];
assert.equal(row.path, "js/core.js");
assert.deepEqual(row.imports, ["node:fs"]);
assert.deepEqual(row.exports, ["Model", "alias", "render"]);
assert.deepEqual(row.messages, {
  receives: ["type:work"],
  sends: ["type:ready"],
});
assert.equal(row.call_counts.helper, 1);
assert.equal(row.method_call_counts.render, 1);
assert.equal(project.counts.sources, 1n);
assert.equal(project.counts.providers, 3n);
assert.ok(project.counts.facts > 15n);
assert.equal(project.mergeSummary().conflicts, 0n);
const ledger = JSON.parse(project.mergeLedgerJson().toString("utf8"));
assert.ok(
  ledger.selections.some((entry) => entry.domain === "reference-targets"),
);
let semanticProvider;
for (let index = 0; index < Number(project.counts.providers); index += 1) {
  const candidate = JSON.parse(
    project.providerFactsJson(index).toString("utf8"),
  );
  if (candidate.producer.name === "archbird-typescript")
    semanticProvider = candidate;
}
assert.ok(semanticProvider);
assert.equal(
  semanticProvider.producer.runtime.includes("typescript-6.0.3"),
  true,
);
assert.ok(
  semanticProvider.resolutions.some(
    (resolution) =>
      resolution.state === "unique" && resolution.targets.length === 1,
  ),
);
assert.match(project.manifestSha256, /^[0-9a-f]{64}$/);
assert.match(project.mapInputSha256, /^[0-9a-f]{64}$/);
assert.equal(
  row.sha256,
  crypto.createHash("sha256").update(source).digest("hex"),
);
assert.throws(
  () => jsonCanonicalize(Buffer.from('{"a":1,"a":2}')),
  /duplicate/i,
);
project = null;

function conflictingSymbolProvider(projectValue, providerName, factName) {
  return Buffer.from(JSON.stringify({
    artifact: "archbird-provider-facts",
    capabilities: [{
      claims: ["syntax-structure"],
      coverage: "complete",
      domain: "symbols",
    }],
    diagnostics: [],
    facts: [{
      claim: "syntax-structure",
      domain: "symbols",
      id: `symbol:${factName}`,
      key: "a",
      kind: "variable",
      name: factName,
      path: "src/a.txt",
      project: "merge-conflict",
      span: { end: 3, start: 0 },
    }],
    inputs: [{
      project: "merge-conflict",
      source_manifest_sha256: projectValue.manifestSha256,
    }],
    producer: {
      configuration_sha256: (providerName === "primary" ? "1" : "2").repeat(64),
      implementation_sha256: "3".repeat(64),
      name: `fixture-${providerName}`,
      version: "1",
    },
    provenance: "derived",
    resolutions: [],
    schema_version: 1,
    subject: {
      path: "src/a.txt",
      project: "merge-conflict",
      scope: "file",
    },
  }));
}

const conflictProject = new Project("merge-conflict", [
  new Source("src/a.txt", Buffer.from("abc"), { language: "text" }),
]);
conflictProject.addProvider(
  conflictingSymbolProvider(conflictProject, "primary", "a"),
  "primary",
);
conflictProject.addProvider(
  conflictingSymbolProvider(conflictProject, "augment", "b"),
  "augment",
);
let mergeError;
try {
  conflictProject.finalizeProviders();
} catch (error) {
  mergeError = error;
}
assert.ok(mergeError);
const compactConflicts = JSON.parse(
  mergeError.mergeConflictsJson.toString("utf8"),
);
assert.equal(compactConflicts.artifact, "archbird-provider-merge-conflicts");
assert.deepEqual(compactConflicts.summary, {
  conflicts: 1,
  providers_in_conflicts: 2,
  providers_total: 2,
});
assert.equal(compactConflicts.conflicts[0].left_fact.name, "a");
assert.equal(compactConflicts.conflicts[0].right_fact.name, "b");
conflictProject.dispose();

let semanticProject = new Project("typescript-test", [
  new Source(
    "src/defs.ts",
    Buffer.from(
      "// π\nexport function target(value: number) { return value; }\n",
    ),
    {
      language: "typescript",
      layer: "ts",
    },
  ),
  new Source(
    "src/use.ts",
    Buffer.from(
      'import { target } from "./defs";\nimport { absent } from "./missing";\nconst π = target(1);\nconst spread = {...{}};\n',
    ),
    {
      language: "typescript",
      layer: "ts",
    },
  ),
]);
semanticProject.setConfig(
  JSON.stringify({
    schema_version: 2,
    project: "typescript-test",
    layers: [
      {
        name: "ts",
        language: "typescript",
        globs: ["src/**/*.ts"],
      },
    ],
  }),
);
semanticProject.scan();
const typescriptEvidence = [];
for (
  let index = 0;
  index < Number(semanticProject.counts.providers);
  index += 1
) {
  const candidate = JSON.parse(
    semanticProject.providerFactsJson(index).toString("utf8"),
  );
  if (candidate.producer.name === "archbird-typescript") {
    typescriptEvidence.push(candidate);
  }
}
assert.equal(typescriptEvidence.length, 2);
assert.equal(
  typescriptEvidence.some((bundle) =>
    bundle.facts.some((fact) => !fact.key || ("name" in fact && !fact.name)),
  ),
  false,
);
assert.deepEqual(
  typescriptEvidence.map((bundle) => bundle.subject.scope),
  ["file", "file"],
);
const targetReferences = typescriptEvidence.flatMap((bundle) =>
  bundle.facts
    .filter(
      (fact) => fact.domain === "reference-targets" && fact.name === "target",
    )
    .map((fact) => ({ bundle, fact })),
);
assert.ok(targetReferences.length >= 1);
const resolvedTarget = targetReferences
  .map(({ bundle, fact }) =>
    bundle.resolutions.find((resolution) => resolution.fact_id === fact.id),
  )
  .find((resolution) => resolution?.state === "unique");
assert.ok(resolvedTarget);
assert.match(resolvedTarget.targets[0], /^symbol:[0-9a-f]{64}$/);
assert.equal(
  typescriptEvidence.every((bundle) => bundle.diagnostics.length === 0),
  true,
);
const semanticMap = semanticProject.map();
assert.ok(
  semanticMap.edges.some(
    (edge) =>
      edge.kind === "semantic-call" &&
      edge.source === "src/use.ts" &&
      edge.target === "src/defs.ts" &&
      edge.names.includes("target"),
  ),
);
semanticProject = null;

function createScipProject(positionEncoding) {
  const current = new Project("scip-node", [
    new Source(
      "index.scip",
      scipFixture(),
      { roles: ["index"] },
    ),
    new Source(
      "src/defs.js",
      Buffer.from("export function add(a, b) { return a + b; }\n"),
      { language: "javascript", layer: "javascript" },
    ),
    new Source(
      "src/use.js",
      Buffer.from('import { add } from "./defs.js";\nexport const value = add(1, 2);\n'),
      { language: "javascript", layer: "javascript" },
    ),
  ]);
  current.setConfig(JSON.stringify({
    schema_version: 2,
    project: "scip-node",
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
        position_encoding_fallback: positionEncoding,
      },
    ],
  }));
  current.scan("primary", { typescript: false });
  return current;
}

function scipConfigurationSha(projectValue) {
  for (let index = 0; index < Number(projectValue.counts.providers); index += 1) {
    const provider = JSON.parse(
      projectValue.providerFactsJson(index).toString("utf8"),
    );
    if (provider.producer.name === "archbird-scip") {
      return provider.producer.configuration_sha256;
    }
  }
  throw new Error("SCIP provider facts are absent");
}

function scipProvider(projectValue) {
  for (let index = 0; index < Number(projectValue.counts.providers); index += 1) {
    const provider = JSON.parse(
      projectValue.providerFactsJson(index).toString("utf8"),
    );
    if (provider.producer.name === "archbird-scip") return provider;
  }
  throw new Error("SCIP provider facts are absent");
}

let scipProject = createScipProject("utf8");
const scipMap = scipProject.map();
const scipEvidence = scipProvider(scipProject);
assert.equal(scipMap.indexes.length, 1);
assert.equal(scipMap.indexes[0].coverage.documents_mapped, 2);
assert.equal(scipMap.indexes[0].coverage.edges, 1);
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
assert.ok(
  scipEvidence.facts.some(
    (fact) =>
      fact.domain === "semantic-definitions" &&
      fact.name === "add" &&
      fact.attributes.display_name_state === "source-range" &&
      fact.attributes.semantic_symbol === "scip npm fixture 1.0 fixture/add().",
  ),
);
assert.ok(
  scipMap.diagnostics.some(
    (row) => row.code === "scip-configured-position-encoding",
  ),
);
const utf8ScipConfiguration = scipConfigurationSha(scipProject);
scipProject.dispose();
scipProject = null;
const utf16ScipProject = createScipProject("utf16");
assert.notEqual(
  scipConfigurationSha(utf16ScipProject),
  utf8ScipConfiguration,
);
utf16ScipProject.dispose();

let invalidTypescript = new Project("invalid-typescript", [
  new Source("src/broken.ts", Buffer.from("export const = 1;\n"), {
    language: "typescript",
    layer: "ts",
  }),
]);
invalidTypescript.setConfig(
  JSON.stringify({
    schema_version: 2,
    project: "invalid-typescript",
    layers: [
      {
        name: "ts",
        language: "typescript",
        globs: ["src/**/*.ts"],
      },
    ],
  }),
);
invalidTypescript.scan();
assert.ok(
  invalidTypescript
    .map()
    .diagnostics.some((row) => row.code.startsWith("typescript-")),
);
invalidTypescript = null;
const repositoryFixture = path.resolve(
  process.argv[3],
  "test/fixtures/map_base",
);
let repositoryProject = Project.fromConfig(
  path.join(repositoryFixture, "archbird.json"),
  { root: repositoryFixture, typescript: false },
);
assert.deepEqual(
  repositoryProject.sources.map((item) => item.path),
  ["js/index.js", "py/pkg/__init__.py", "py/pkg/api.py"],
);
assert.equal(repositoryProject.map().project, "map-base");
assert.deepEqual(repositoryProject.mapJson(), repositoryProject.mapJson());
const repositoryMapJson = repositoryProject.mapJson();
const currentProducerQuery = JSON.parse(queryMap(repositoryMapJson, {
  paths: ["py/pkg"],
  depth: 0,
  producerPolicy: "current",
}));
assert.equal(currentProducerQuery.query.producer_policy, "current");
assert.equal(currentProducerQuery.query.producer_compatibility, "current");
const retrievalQuery = JSON.parse(queryMap(repositoryMapJson, {
  search: ["twce javascript"], searchLimit: 4, depth: 0, testDepth: 0,
}));
assert.equal(retrievalQuery.query.retrieval.contract, "archbird-lexical-ranking-v2");
assert.equal(retrievalQuery.query.retrieval.confidence, "candidate");
assert.equal(retrievalQuery.query.retrieval.hits.length, 4);
assert.equal(retrievalQuery.query.retrieval.hits[0].path, "js/index.js");
assert.equal(retrievalQuery.query.retrieval.hits[0].name, "twice");
assert.ok(retrievalQuery.query.retrieval.hits[0].reasons.some(
  (reason) => reason.match === "edit-1",
));
assert.ok(queryMapMarkdown(repositoryMapJson, {
  search: ["twce javascript"], searchLimit: 4, depth: 0, testDepth: 0,
}).includes("## Candidate seeds"));
const sameLineCConfig = Buffer.from(JSON.stringify({
  schema_version: 2,
  project: "same-line-c-query",
  layers: [{
    name: "c",
    language: "c",
    globs: ["src/**/*.c"],
  }],
}));
const sameLineCProject = new Project("same-line-c-query", [
  new Source(
    "src/api.c",
    Buffer.from(
      "int archbird_pair(void); "
      + "int archbird_pair(void) { return 0; }\n",
    ),
    { language: "c", layer: "c" },
  ),
]);
sameLineCProject.setConfig(sameLineCConfig);
sameLineCProject.scan("primary", { typescript: false });
const sameLineCQuery = JSON.parse(queryMap(sameLineCProject.mapJson(), {
  search: ["archbird pair"], searchLimit: 8, depth: 0, testDepth: 0,
}));
assert.deepEqual(
  sameLineCQuery.query.retrieval.hits
    .filter((row) => row.kind === "symbol")
    .map((row) => [row.name, row.symbol_kind, row.line])
    .sort(),
  [
    ["archbird_pair", "declaration", 1],
    ["archbird_pair", "function", 1],
  ],
);
assert.equal(
  sameLineCQuery.query.projection_results[0].completeness.classification,
  "complete",
);
sameLineCProject.dispose();
const differentProducerMap = JSON.parse(repositoryMapJson);
differentProducerMap.tool.implementation_sha256 = "0".repeat(64);
const compatibleProducerQuery = JSON.parse(queryMap(
  Buffer.from(JSON.stringify(differentProducerMap)),
  { paths: ["py/pkg"], depth: 0, producerPolicy: "compatible" },
));
assert.equal(
  compatibleProducerQuery.query.producer_compatibility,
  "different",
);
assert.throws(
  () => queryMap(Buffer.from(JSON.stringify(differentProducerMap)), {
    paths: ["py/pkg"],
    depth: 0,
    producerPolicy: "current",
  }),
  (error) => error.code === "ARCHBIRD_STATUS_10",
);
const cacheRoot = path.resolve(
  process.argv[3],
  "build/test-provider-cache-node",
);
fs.rmSync(cacheRoot, { force: true, recursive: true });
const cachedCold = Project.fromConfig(
  path.join(repositoryFixture, "archbird.json"),
  {
    root: repositoryFixture, typescript: false, cacheDir: cacheRoot,
    mapCache: false,
  },
);
const cachedWarm = Project.fromConfig(
  path.join(repositoryFixture, "archbird.json"),
  {
    root: repositoryFixture, typescript: false, cacheDir: cacheRoot,
    mapCache: false,
  },
);
assert.deepEqual(cachedCold.mapJson(), repositoryMapJson);
assert.deepEqual(cachedWarm.mapJson(), repositoryMapJson);
assert.ok(cachedCold.cacheStats.misses > 0);
assert.equal(cachedCold.cacheStats.writes, cachedCold.cacheStats.misses);
assert.equal(cachedWarm.cacheStats.hits, cachedCold.cacheStats.writes);
assert.equal(cachedWarm.cacheStats.misses, 0);
const cInputCacheRoot = path.resolve(
  process.argv[3], "build/test-c-input-cache-node",
);
fs.rmSync(cInputCacheRoot, { force: true, recursive: true });
const cInputSources = (header, note) => [
  new Source("include/api.h", Buffer.from(header), {
    language: "c", layer: "core", roles: ["public-header", "source"],
  }),
  new Source("notes/state.txt", Buffer.from(note), {
    language: "text", layer: "docs",
  }),
  new Source("src/api.c", Buffer.from("int api(void) { return 1; }\n"), {
    language: "c", layer: "core",
  }),
];
const cInputCold = new Project(
  "c-input-cache", cInputSources("int api(void);\n", "before\n"),
);
cInputCold.scan("primary", {
  typescript: false, cacheDir: cInputCacheRoot, mapCache: false,
});
const cInputUnrelated = new Project(
  "c-input-cache", cInputSources("int api(void);\n", "after\n"),
);
cInputUnrelated.scan("primary", {
  typescript: false, cacheDir: cInputCacheRoot, mapCache: false,
});
assert.equal(cInputUnrelated.cacheStats.hits, 4);
assert.equal(cInputUnrelated.cacheStats.misses, 0);
const cInputHeaderChanged = new Project(
  "c-input-cache",
  cInputSources("int api(void);\nint added(void);\n", "after\n"),
);
cInputHeaderChanged.scan("primary", {
  typescript: false, cacheDir: cInputCacheRoot, mapCache: false,
});
assert.equal(cInputHeaderChanged.cacheStats.hits, 1);
assert.equal(cInputHeaderChanged.cacheStats.misses, 3);
assert.equal(cInputHeaderChanged.cacheStats.invalid, 1);
fs.rmSync(cInputCacheRoot, { force: true, recursive: true });
const mapCacheRoot = path.resolve(
  process.argv[3],
  "build/test-map-cache-node",
);
fs.rmSync(mapCacheRoot, { force: true, recursive: true });
const mapCachedCold = Project.fromConfig(
  path.join(repositoryFixture, "archbird.json"),
  { root: repositoryFixture, typescript: false, cacheDir: mapCacheRoot },
);
const mapCachedColdJson = mapCachedCold.mapJson();
const mapCachedWarm = Project.fromConfig(
  path.join(repositoryFixture, "archbird.json"),
  { root: repositoryFixture, typescript: false, cacheDir: mapCacheRoot },
);
assert.deepEqual(mapCachedWarm.mapJson(), mapCachedColdJson);
assert.deepEqual(mapCachedCold.mapCacheStats, {
  errors: 0, hits: 0, invalid: 0, misses: 1,
  noSpace: 0, skipped: 0, writes: 1,
});
assert.deepEqual(mapCachedWarm.mapCacheStats, {
  errors: 0, hits: 1, invalid: 0, misses: 0,
  noSpace: 0, skipped: 0, writes: 0,
});
assert.equal(mapCachedWarm.cacheStats.hits, 0);
assert.ok(mapCachedWarm.counts.providers > 0n);
const mapPrefixes = fs.readdirSync(path.join(mapCacheRoot, "maps-v1"));
assert.equal(mapPrefixes.length, 1);
const mapDirectory = path.join(mapCacheRoot, "maps-v1", mapPrefixes[0]);
const mapFiles = fs.readdirSync(mapDirectory);
assert.equal(mapFiles.length, 1);
fs.writeFileSync(path.join(mapDirectory, mapFiles[0]), "{broken");
const mapCachedRecovered = Project.fromConfig(
  path.join(repositoryFixture, "archbird.json"),
  { root: repositoryFixture, typescript: false, cacheDir: mapCacheRoot },
);
assert.deepEqual(mapCachedRecovered.mapJson(), mapCachedColdJson);
assert.equal(mapCachedRecovered.mapCacheStats.invalid, 1);
const changedMapFixture = path.resolve(
  process.argv[3], "build/test-map-cache-source-node",
);
fs.rmSync(changedMapFixture, { force: true, recursive: true });
fs.cpSync(repositoryFixture, changedMapFixture, { recursive: true });
const changedMap = Project.fromConfig(
  path.join(changedMapFixture, "archbird.json"),
  { root: changedMapFixture, typescript: false, cacheDir: mapCacheRoot },
);
const changedMapJson = changedMap.mapJson();
fs.appendFileSync(
  path.join(changedMapFixture, "js/index.js"),
  "\nexport function cacheInvalidationProbe() { return 2; }\n",
);
const changedMapAgain = Project.fromConfig(
  path.join(changedMapFixture, "archbird.json"),
  { root: changedMapFixture, typescript: false, cacheDir: mapCacheRoot },
);
assert.equal(changedMapAgain.mapCacheStats.misses, 1);
assert.notDeepEqual(changedMapAgain.mapJson(), changedMapJson);
fs.rmSync(changedMapFixture, { force: true, recursive: true });
fs.rmSync(mapCacheRoot, { force: true, recursive: true });
const cacheChanged = new Project("cache-source", [
  new Source("src/a.js", Buffer.from("export function a() { return 1; }\n"), {
    language: "javascript",
  }),
]);
cacheChanged.scan("primary", {
  typescript: false, cacheDir: cacheRoot, mapCache: false,
});
const cacheChangedAgain = new Project("cache-source", [
  new Source("src/a.js", Buffer.from("export function a() { return 2; }\n"), {
    language: "javascript",
  }),
]);
cacheChangedAgain.scan("primary", {
  typescript: false,
  cacheDir: cacheRoot,
  mapCache: false,
});
assert.equal(cacheChangedAgain.cacheStats.hits, 0);
assert.equal(cacheChangedAgain.cacheStats.misses, 2);
fs.rmSync(cacheRoot, { force: true, recursive: true });
const boundedCacheRoot = path.resolve(
  process.argv[3],
  "build/test-provider-cache-bounded-node",
);
fs.rmSync(boundedCacheRoot, { force: true, recursive: true });
for (const invalidBudget of [true, 0, -1, 1.5, Number.MAX_SAFE_INTEGER + 1]) {
  assert.throws(
    () => new ProviderCache(boundedCacheRoot, { maxBytes: invalidBudget }),
    /positive safe integer/,
  );
}
const priorCacheBudget = process.env.ARCHBIRD_CACHE_MAX_BYTES;
for (const invalidEnvironment of ["", "+1", " 1", "1.5", "9007199254740992"]) {
  process.env.ARCHBIRD_CACHE_MAX_BYTES = invalidEnvironment;
  assert.throws(
    () => new ProviderCache(boundedCacheRoot),
    /positive (safe )?integer/,
  );
}
if (priorCacheBudget === undefined) {
  delete process.env.ARCHBIRD_CACHE_MAX_BYTES;
} else {
  process.env.ARCHBIRD_CACHE_MAX_BYTES = priorCacheBudget;
}
const boundedCache = new ProviderCache(boundedCacheRoot, { maxBytes: 100 });
const boundedParameters = {
  namespace: "fixture",
  project: "cache-budget",
  providerId: "fixture",
  path: "a.js",
  sourceSha256: "1".repeat(64),
};
boundedCache.store(Buffer.alloc(60, "a"), boundedParameters);
const boundedSecond = {
  ...boundedParameters,
  path: "b.js",
  sourceSha256: "2".repeat(64),
};
boundedCache.store(Buffer.alloc(60, "b"), boundedSecond);
assert.ok(boundedCache.stats.bytes <= 100);
assert.equal(boundedCache.stats.evictions, 1);
assert.equal(boundedCache.load(boundedParameters), null);
assert.deepEqual(boundedCache.load(boundedSecond), Buffer.alloc(60, "b"));
boundedCache.store(Buffer.alloc(101, "c"), boundedParameters);
assert.equal(boundedCache.stats.skipped, 1);
const staleCacheTemporary = path.join(
  boundedCacheRoot,
  "providers-v1",
  "aa",
  ".stale.tmp",
);
fs.mkdirSync(path.dirname(staleCacheTemporary), { recursive: true });
fs.writeFileSync(staleCacheTemporary, "partial");
const recoveredCache = new ProviderCache(boundedCacheRoot, { maxBytes: 100 });
assert.equal(fs.existsSync(staleCacheTemporary), false);
assert.equal(recoveredCache.stats.temporariesRemoved, 1);
const originalOpenSync = fs.openSync;
fs.openSync = () => {
  const error = new Error("no space left on device");
  error.code = "ENOSPC";
  throw error;
};
try {
  recoveredCache.store(Buffer.from("d"), boundedParameters);
} finally {
  fs.openSync = originalOpenSync;
}
assert.equal(recoveredCache.stats.noSpace, 1);
assert.equal(recoveredCache.stats.errors, 1);
fs.rmSync(boundedCacheRoot, { force: true, recursive: true });
const zeroFixture = path.resolve(process.argv[3], "test/fixtures/zero_config");
const zeroResolutionJson = resolveDiscovery(zeroFixture);
assert.deepEqual(zeroResolutionJson, resolveDiscovery(zeroFixture));
const zeroResolution = JSON.parse(zeroResolutionJson);
assert.equal(zeroResolution.project, "zero-fixture");
assert.deepEqual(
  zeroResolution.files.map((row) => row.path),
  [
    "Makefile",
    "generated/parser.c",
    "large.py",
    "nested/keep.py",
    "package.json",
    "pyproject.toml",
    "src/custom.py",
    "src/main.js",
    "src/main.py",
    "src/reinclude.skip.py",
    "src/zero_python/__init__.py",
    "tests/test_main.py",
    "vendor/lib.c",
  ],
);
assert.deepEqual(
  zeroResolution.ignore_files.map((row) => row.path),
  [".gitignore", ".ignore", ".archbirdignore", "nested/.gitignore"],
);
const materializedConfig = Buffer.from(
  JSON.stringify(zeroResolution.effective_config),
);
const materializedResolution = JSON.parse(
  resolveDiscovery(zeroFixture, { config: materializedConfig }),
);
const zeroResolutionEvidence = structuredClone(zeroResolution);
const materializedResolutionEvidence = structuredClone(materializedResolution);
delete zeroResolutionEvidence.origins;
delete zeroResolutionEvidence.sha256;
delete materializedResolutionEvidence.origins;
delete materializedResolutionEvidence.sha256;
assert.deepEqual(materializedResolutionEvidence, zeroResolutionEvidence);
const boundedResolution = JSON.parse(
  resolveDiscovery(zeroFixture, {
    project: "cli",
    ignoreFiles: [".customignore"],
    maxFileBytes: 100,
  }),
);
assert.deepEqual(boundedResolution.coverage, {
  assets: 10,
  ignored: 3,
  inventory_files: 23,
  oversized: 1,
  pruned_directories: 1,
  selected: 11,
  unsupported_known: 1,
});
assert.deepEqual(boundedResolution.diagnostics, [
  {
    bytes: 167,
    code: "discovery-file-oversized",
    limit: 100,
    path: "large.py",
    severity: "warning",
  },
]);
const customOnlyResolution = JSON.parse(
  resolveDiscovery(zeroFixture, {
    ignore: false,
    ignoreFiles: [".customignore"],
  }),
);
const customOnlyPaths = new Set(
  customOnlyResolution.files.map((row) => row.path),
);
assert.equal(customOnlyPaths.has("src/custom.py"), false);
assert.equal(customOnlyPaths.has("ignored/drop.py"), true);
assert.equal(customOnlyPaths.has("nested/local.py"), true);
assert.equal(customOnlyPaths.has("src/from-ignore.py"), true);
assert.deepEqual(
  customOnlyResolution.ignore_files.map((row) => row.path),
  [".customignore"],
);
assert.equal(
  customOnlyResolution.origins.some(
    (row) => row.pointer === "/selection/ignore" && row.source === "cli",
  ),
  true,
);
let zeroProject = Project.fromRepository(zeroFixture, {
  mapCache: false,
  typescript: false,
});
const zeroMap = zeroProject.map();
const materializedProject = Project.fromRepository(zeroFixture, {
  config: materializedConfig,
  mapCache: false,
  typescript: false,
});
const materializedMap = materializedProject.map();
assert.equal(materializedProject.configSha256, zeroProject.configSha256);
assert.equal(materializedProject.mapInputSha256, zeroProject.mapInputSha256);
const zeroMapEvidence = structuredClone(zeroMap);
const materializedMapEvidence = structuredClone(materializedMap);
delete zeroMapEvidence.discovery;
delete materializedMapEvidence.discovery;
assert.deepEqual(materializedMapEvidence, zeroMapEvidence);
materializedProject.dispose();
assert.equal(zeroMap.project, "zero-fixture");
const zeroPackages = new Map(zeroMap.packages.map((row) => [row.name, row]));
assert.equal(zeroPackages.get("npm-root").identity, "@archbird/zero-fixture");
assert.deepEqual(zeroPackages.get("python-root").aliases, [
  "zero-python",
  "zero_python",
]);
assert.deepEqual(zeroPackages.get("python-root").entrypoints, {
  "configured:0": "src/zero_python/__init__.py",
});
assert.deepEqual(zeroMap.discovery, {
  coverage: zeroResolution.coverage,
  profile: zeroResolution.profile,
  sha256: zeroResolution.sha256,
});
assert.match(
  zeroProject.mapMarkdown({ view: "audit" }).toString("utf8"),
  /unsupported-known=1/,
);
assert.match(
  zeroProject.mapMarkdown({ view: "audit" }).toString("utf8"),
  /Coverage warning:/,
);
assert.deepEqual(
  zeroMap.files.filter((row) => row.roles).map((row) => [row.path, row.roles]),
  [
    ["generated/parser.c", ["generated-candidate"]],
    ["tests/test_main.py", ["test-candidate"]],
    ["vendor/lib.c", ["third-party-candidate"]],
  ],
);
assert.equal(zeroMap.tests.length, 1);
assert.equal(zeroMap.tests[0].path, "tests/test_main.py");
assert.equal(zeroMap.tests[0].inventory_source, "discovery");
assert.equal(zeroMap.tests[0].inventory_state, "candidate");
assert.deepEqual(
  zeroMap.tests[0].cases.map((row) => [row.selector, row.evidence_kind]),
  [["test_main", "test_definition"]],
);
assert.deepEqual(zeroMap.tests[0].cases[0].routes, {
  "src/zero_python/__init__.py": 1,
});
const cRegistryFixture = path.resolve(
  process.argv[3],
  "test/fixtures/zero_config_c_registry",
);
const cRegistryProject = Project.fromRepository(cRegistryFixture, {
  typescript: false,
});
const cRegistryMap = cRegistryProject.map();
const cRegistryTest = cRegistryMap.tests.find(
  (row) => row.path === "test/test_widget.c",
);
assert.ok(cRegistryTest);
assert.deepEqual(
  cRegistryTest.cases.map((row) => [row.selector, row.evidence_kind]),
  [
    ["direct", "test_definition"],
    ["widget/explicit", "test_registration_candidate"],
    ["widget/forwarded", "test_registration_candidate"],
  ],
);
assert.deepEqual(cRegistryTest.cases[1].routes, {
  "test/test_widget.c": 1,
});
assert.deepEqual(cRegistryTest.cases[2].routes, {
  "test/test_widget.c": 1,
});
const standardMapReport = repositoryProject.mapMarkdown();
assert.match(
  repositoryProject.mapMarkdown({ view: "audit" }).toString("utf8"),
  /Rendered detail for \d+ of \d+ mapped files; the canonical Map contains all \d+\./,
);
assert.equal(
  standardMapReport
    .toString("utf8")
    .startsWith("# map-base architecture\n"),
  true,
);
assert.match(standardMapReport.toString("utf8"), /## Key files and symbols/);
assert.deepEqual(standardMapReport, renderMapMarkdown(repositoryMapJson));
assert.match(
  repositoryProject.mapMarkdown({ view: "architecture" }).toString("utf8"),
  /## Languages/,
);
assert.ok(
  repositoryProject.mapMarkdown({ detail: "compact" }).length <
    standardMapReport.length,
);
assert.notDeepEqual(
  standardMapReport,
  repositoryProject.mapMarkdown({ full: true }),
);
const publishedOkf = publishOkfBundle(repositoryMapJson);
assert.deepEqual(publishedOkf, publishOkfBundle(repositoryMapJson));
assert.equal(JSON.parse(publishedOkf).artifact, "okf-output-bundle");
const unicodeMap = JSON.parse(repositoryMapJson);
unicodeMap.project = "Straße ﬀ Σς";
const unicodeOkf = JSON.parse(
  publishOkfBundle(Buffer.from(JSON.stringify(unicodeMap))),
);
assert.equal(unicodeOkf.project, "Straße ﬀ Σς");
assert.equal(
  exportGraph(repositoryMapJson, { format: "graphml", view: "files" })
    .toString("utf8")
    .startsWith("<?xml"),
  true,
);
assert.equal(
  exportGraph(repositoryMapJson, { format: "mermaid" })
    .toString("utf8")
    .startsWith("%% Archbird components graph"),
  true,
);
const componentGraphJson = repositoryProject.graphViewJson();
const symbolGraphJson = repositoryProject.graphViewJson({
  view: "symbols",
  query: { symbols: ["js/index.js:add"], depth: 1, testDepth: 1 },
});
assert.deepEqual(componentGraphJson, repositoryProject.graphViewJson());
assert.equal(JSON.parse(componentGraphJson).artifact, "archbird-graph-view");
assert.equal(JSON.parse(componentGraphJson).request.view, "components");
assert.equal(JSON.parse(componentGraphJson).source.artifact, "map");
assert.deepEqual(
  symbolGraphJson,
  repositoryProject.graphViewJson({
    view: "symbols",
    query: { symbols: ["js/index.js:add"], depth: 1, testDepth: 1 },
  }),
);
assert.equal(JSON.parse(symbolGraphJson).request.view, "symbols");
assert.equal(JSON.parse(symbolGraphJson).source.artifact, "query");
assert.ok(
  JSON.parse(symbolGraphJson).nodes.some(
    (node) => node.kind === "symbol" && node.label === "add",
  ),
);
const okfSource = fs.readFileSync(
  path.resolve(process.argv[3], "test/fixtures/okf/source-bundle.json"),
);
const okfFirst = analyzeOkfSource(okfSource);
assert.deepEqual(okfFirst, analyzeOkfSource(okfSource));
assert.equal(JSON.parse(okfFirst).artifact, "okf-index");
assert.throws(
  () =>
    exportGraph(repositoryMapJson, {
      format: "mermaid",
      view: "files",
      maxNodes: 1,
    }),
  /exceeding/,
);
assert.equal(
  repositoryProject.query({ paths: ["py/pkg"], depth: 0 }).files.length,
  2,
);
const queryReport = repositoryProject.queryMarkdown({
  paths: ["py/pkg"],
  depth: 0,
});
assert.equal(
  queryReport
    .toString("utf8")
    .startsWith("# Focused architecture map: map-base\n"),
  true,
);
assert.deepEqual(
  queryReport,
  queryMapMarkdown(repositoryMapJson, { paths: ["py/pkg"], depth: 0 }),
);
const changeBrief = repositoryProject.queryMarkdown({
  paths: ["py/pkg"],
  depth: 0,
  view: "changes",
});
assert.match(changeBrief.toString("utf8"), /^# Change brief: map-base\n/);
assert.match(changeBrief.toString("utf8"), /## Affected code/);
assert.match(changeBrief.toString("utf8"), /## Routes, tests, and delivery/);
assert.match(changeBrief.toString("utf8"), /## Evidence limits/);
assert.deepEqual(
  changeBrief,
  queryMapMarkdown(repositoryMapJson, {
    paths: ["py/pkg"],
    depth: 0,
    view: "changes",
  }),
);
assert.throws(
  () => repositoryProject.queryMarkdown({ paths: ["py/pkg"], view: "other" }),
  /view must be focused or changes/,
);
assert.throws(
  () => repositoryProject.queryMarkdown({
    paths: ["py/pkg"], compact: true, full: true,
  }),
  /compact and full conflict/,
);
const contextPolicy = { profile: "exact", quotas: { files: 1 } };
const contextQuery = repositoryProject.query({
  paths: ["py/pkg"],
  depth: 0,
  context: contextPolicy,
});
assert.deepEqual(contextQuery.query.context, contextPolicy);
assert.equal(contextQuery.files.length, 2);
const contextReport = repositoryProject.queryMarkdown({
  paths: ["py/pkg"],
  depth: 0,
  context: contextPolicy,
}).toString("utf8");
assert.match(contextReport, /Context: profile=exact;/);
assert.match(contextReport, /files=1\/2\./);
assert.match(contextReport, /## Selection manifest/);
assert.throws(
  () => renderMapMarkdown(repositoryMapJson, { maxChars: -1 }),
  /nonnegative safe integer/,
);
let workspace = Workspace.fromConfig(
  path.resolve(process.argv[3], "test/fixtures/workspace.json"),
);
const workspaceDocument = workspace.data();
assert.equal(workspaceDocument.workspace, "fixture-workspace");
assert.equal(workspaceDocument.routes.length, 2);
repositoryProject.dispose();
repositoryProject.dispose();
repositoryProject = null;
zeroProject.dispose();
zeroProject = null;
workspace.dispose();
workspace = null;
if (global.gc) {
  global.gc();
  global.gc();
}
console.log("native Node frontend parity passed");
