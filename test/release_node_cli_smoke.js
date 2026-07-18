"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

if (process.argv.length !== 6) {
  throw new Error("usage: release_node_cli_smoke.js CLI REPOSITORY_ROOT WORK ENGINE");
}

const cli = path.resolve(process.argv[2]);
const repository = path.resolve(process.argv[3]);
const work = path.resolve(process.argv[4]);
const engine = process.argv[5];
fs.mkdirSync(work, { recursive: true });

function run(arguments_, { expected = 0 } = {}) {
  const completed = spawnSync(cli, arguments_, {
    encoding: "utf8",
    env: { ...process.env, ARCHBIRD_ENGINE: engine },
  });
  if (completed.status !== expected) {
    throw new Error(
      `${cli} ${arguments_.join(" ")} exited ${completed.status}\n` +
      `stdout:\n${completed.stdout}\nstderr:\n${completed.stderr}`,
    );
  }
  return completed.stdout;
}

assert.equal(run(["--version"]).trim(), "0.0.1");
const support = JSON.parse(run(["support"]));
assert.equal(support.engine.kind, engine);
assert.deepEqual(support.providers.host, ["compiler:typescript"]);
assert.deepEqual(support.providers.portable, [
  "lexical:c",
  "lexical:javascript",
  "lexical:python",
  "lexical:r",
  "syntax:tree-sitter:c",
  "syntax:tree-sitter:cpp",
  "syntax:tree-sitter:python",
  "syntax:tree-sitter:javascript",
  "syntax:tree-sitter:typescript",
  "syntax:tree-sitter:tsx",
  "syntax:tree-sitter:r",
  "semantic:scip",
]);
assert.deepEqual(support.providers.precision, {
  c: "tree-sitter+lexical",
  cpp: "tree-sitter+lexical",
  javascript: "typescript-compiler+tree-sitter+lexical",
  python: "tree-sitter+lexical",
  r: "tree-sitter+lexical",
  tsx: "typescript-compiler+tree-sitter+lexical",
  typescript: "typescript-compiler+tree-sitter+lexical",
  vue: "lexical",
});

const fixture = path.join(repository, "test/fixtures/map_base");
const mergeLedgerPath = path.join(work, `${engine}.merge-conflicts.json`);
const map = JSON.parse(run([
  "--config", path.join(fixture, "archbird.json"),
  "--root", fixture,
  "--merge-ledger", mergeLedgerPath,
  "--format", "json",
  "--check",
]));
assert.equal(map.artifact, "map");
assert.equal(map.project, "map-base");
const mergeSummary = JSON.parse(fs.readFileSync(mergeLedgerPath)).summary;
assert.equal(mergeSummary.conflicts, 0);
assert.equal(mergeSummary.providers_in_conflicts, 0);
assert.equal(mergeSummary.providers_total > 0, true);
const zeroFixture = path.join(repository, "test/fixtures/zero_config");
const zeroMap = JSON.parse(run([
  "map", zeroFixture, "--no-config", "--format", "json", "--check",
]));
assert.equal(zeroMap.project, "zero-fixture");
assert.equal(zeroMap.packages[0].identity, "@archbird/zero-fixture");
assert.equal(zeroMap.tests.length, 1);
assert.equal(zeroMap.tests[0].inventory_state, "candidate");
assert.equal(zeroMap.tests[0].cases[0].selector, "test_main");
const configuredMap = JSON.parse(run([
  "map", zeroFixture, "--project", "cli-fixture", "--format", "json",
]));
assert.equal(configuredMap.project, "cli-fixture");
assert.equal(configuredMap.files.some((row) => row.path === "src/main.js"), false);
const resolution = JSON.parse(run([
  "config", "show", zeroFixture, "--no-config",
]));
assert.equal(resolution.artifact, "archbird-config-resolution");
const initializedPath = path.join(work, `${engine}.initialized.json`);
run([
  "config", "init", zeroFixture, "--no-config", "--output", initializedPath,
]);
assert.equal(JSON.parse(fs.readFileSync(initializedPath)).project, "zero-fixture");
run([
  "config", "init", zeroFixture, "--no-config", "--output", initializedPath,
], { expected: 2 });
const mapPath = path.join(work, `${engine}.map.json`);
fs.writeFileSync(mapPath, JSON.stringify(map));
const freshness = JSON.parse(run([
  "freshness", fixture,
  "--config", path.join(fixture, "archbird.json"),
  "--snapshot", mapPath, "--check",
]));
assert.equal(freshness.status, "current");
const query = JSON.parse(run([
  "query", "--map", mapPath, "--path", "py/pkg", "--depth", "0", "--format", "json",
]));
assert.equal(query.files.length, 2);
const queryPath = path.join(work, `${engine}.query.json`);
fs.writeFileSync(queryPath, JSON.stringify(query));
const componentGraph = JSON.parse(run([
  "export", "json", "--map", mapPath, "--view", "components",
]));
assert.equal(componentGraph.artifact, "archbird-graph-view");
assert.equal(componentGraph.source.artifact, "map");
const symbolQuery = run([
  "query", "--map", mapPath, "--symbol", "js/index.js:add", "--format", "json",
]);
fs.writeFileSync(queryPath, symbolQuery);
const symbolGraph = JSON.parse(run([
  "export", "json", "--map", queryPath, "--view", "symbols",
]));
assert.equal(symbolGraph.request.view, "symbols");
assert.equal(symbolGraph.source.artifact, "query");

const observationFixture = path.join(repository, "test/fixtures/map_correctness");
const observationMap = JSON.parse(run([
  "map", observationFixture,
  "--config", path.join(observationFixture, "archbird.json"),
  "--format", "json", "--check",
]));
const hashFile = (relative) => crypto.createHash("sha256")
  .update(fs.readFileSync(path.join(observationFixture, relative))).digest("hex");
const evidence = [
  { path: "test/test_cases.c", role: "runner", sha256: hashFile("test/test_cases.c") },
  { path: "csrc/callbacks.c", role: "subject", sha256: hashFile("csrc/callbacks.c") },
  { path: "test/test_cases.c", role: "test_inventory", sha256: hashFile("test/test_cases.c") },
];
const evidenceSliceSha256 = crypto.createHash("sha256")
  .update(JSON.stringify(evidence)).digest("hex");
const observation = {
  artifact: "archbird-test-symbol-observations",
  cases: [{
    group: "c",
    path: "test/test_cases.c",
    selector: "sched.2d_e2e",
    symbols: [{ hits: 1, path: "csrc/callbacks.c", symbol: "alpha_callback" }],
  }],
  producer: {
    configuration_sha256: "4".repeat(64),
    implementation_sha256: "3".repeat(64),
    name: "node-cli-symbol-runner",
    runtime: `fixture-${engine}`,
    version: "1",
  },
  project: "map-correctness",
  provenance: "observed",
  schema_version: 1,
  source: {
    config_sha256: observationMap.evidence.config_sha256,
    evidence,
    evidence_slice_sha256: evidenceSliceSha256,
    map_input_sha256: observationMap.evidence.input_sha256,
  },
};
const observationPath = path.join(work, `${engine}.test-symbol-observations.json`);
fs.writeFileSync(observationPath, JSON.stringify(observation));
const observedQuery = JSON.parse(run([
  "query", observationFixture,
  "--config", path.join(observationFixture, "archbird.json"),
  "--test-symbol-observations", observationPath,
  "--symbol", "csrc/callbacks.c:alpha_callback",
  "--direction", "upstream", "--depth", "0", "--test-depth", "1",
  "--format", "json", "--check",
]));
const observedMatch = observedQuery.test_matches.find((row) =>
  row.group === "c" && row.path === "test/test_cases.c" &&
  row.selector === "sched.2d_e2e");
assert.equal(observedMatch.classification, "observed");

const providerSuite = path.join(
  repository,
  "test/fixtures/act/provider/provider.verify.json",
);
const verificationBytes = run(["verify", "--config", providerSuite, "--format", "json"]);
const verification = JSON.parse(verificationBytes);
const finding = verification.checks
  .find((row) => row.id === "PROVIDER-RENAME")
  .findings.find((row) => row.key === "core_sum");
assert.ok(finding.fingerprint);
const verificationPath = path.join(work, `${engine}.verify.json`);
const proposalPath = path.join(work, `${engine}.proposal.json`);
const contractPath = path.join(work, `${engine}.contract.json`);
fs.writeFileSync(verificationPath, verificationBytes);
fs.writeFileSync(proposalPath, run([
  "plan", "--verification", verificationPath, "--finding", finding.fingerprint,
]));
fs.writeFileSync(contractPath, run([
  "contract", "--proposal", proposalPath,
  "--objective", "Exercise packaged Act",
  "--owner", "release",
  "--rationale", "Prove the packaged CLI reaches the native Act engine.",
  "--preserve-all",
]));
const result = JSON.parse(run([
  "verify-plan",
  "--proposal", proposalPath,
  "--contract", contractPath,
  "--before-verification", verificationPath,
  "--after-verification", verificationPath,
]));
assert.equal(result.artifact, "change-result");
assert.equal(result.status, "missing");
console.log(`packaged Node CLI Map/Verify/Act passed through ${engine}`);
