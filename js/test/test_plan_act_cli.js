"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

if (process.argv.length !== 4) {
  throw new Error("usage: test_plan_act_cli.js NATIVE_ADDON REPOSITORY");
}

const addon = path.resolve(process.argv[2]);
const repository = path.resolve(process.argv[3]);
process.env.ARCHBIRD_ENGINE = "native";
process.env.ARCHBIRD_NATIVE_ADDON = addon;
const archbird = require(path.join(repository, "js/src/index.js"));
const actTransport = require(path.join(repository, "js/src/act-transport.js"));
const root = fs.mkdtempSync(path.join(repository, "build/node-plan-act-"));
const rootLink = `${root}-link`;
const artifacts = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-artifacts-"),
);
const surfaceRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-surface-"),
);
const registrationRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-registration-"),
);
const providerParityRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-provider-parity-"),
);
const coordinatedRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-coordinated-"),
);
const napiRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-napi-"),
);
const observedRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-observed-"),
);
const assertedRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-asserted-"),
);
const assertedCreateRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-asserted-create-"),
);
const requiredEdgeRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-required-edge-"),
);
const forbiddenEdgeRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-forbidden-edge-"),
);
const redirectRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-redirect-"),
);
const ecmascriptRedirectRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-ecmascript-redirect-"),
);
const ecmascriptUnobservedRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-ecmascript-unobserved-"),
);
const ecmascriptMultiRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-ecmascript-multi-"),
);
const ecmascriptReferenceRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-ecmascript-reference-"),
);
const cli = path.join(repository, "js/src/cli.js");
const makeProvider = {
  definition_sha256:
    "a9d5a1c18d33c5c63cd34ced178608b7bb184126a4a9aeda6ad9c057c2e98fa3",
  kind: "make_variable",
  path: "Makefile",
  variable: "WASM_EXPORTS",
};

function run(arguments_, expected = 0) {
  const completed = spawnSync(process.execPath, [cli, ...arguments_], {
    encoding: null,
    env: {
      ...process.env,
      ARCHBIRD_ENGINE: "native",
      ARCHBIRD_NATIVE_ADDON: addon,
    },
  });
  assert.equal(
    completed.status,
    expected,
    `${arguments_.join(" ")} exited ${completed.status}\n` +
      `stdout:\n${Buffer.from(completed.stdout || []).toString("utf8")}\n` +
      `stderr:\n${Buffer.from(completed.stderr || []).toString("utf8")}`,
  );
  return completed;
}

function git(arguments_, cwd) {
  const completed = spawnSync("git", arguments_, {
    cwd,
    encoding: null,
  });
  assert.equal(
    completed.status,
    0,
    `git ${arguments_.join(" ")} exited ${completed.status}\n` +
      `stdout:\n${Buffer.from(completed.stdout || []).toString("utf8")}\n` +
      `stderr:\n${Buffer.from(completed.stderr || []).toString("utf8")}`,
  );
  return Buffer.from(completed.stdout || []);
}

function configureEcmascriptRedirect(
  projectRoot,
  {
    observed = true,
    multiple = false,
    referenceOnly = false,
    typescript = false,
  } = {},
) {
  for (const directory of ["app", "service", "storage"]) {
    fs.mkdirSync(path.join(projectRoot, directory), { recursive: true });
  }
  const imported = multiple ? "rawValue, other" : "rawValue";
  fs.writeFileSync(
    path.join(projectRoot, "app/main.js"),
    `import { ${imported} } from "../storage/raw.js";\n\n` +
      "export function render() {\n  return rawValue();\n}\n",
  );
  fs.writeFileSync(
    path.join(projectRoot, "app/alias.js"),
    "import { rawValue as read } from \"../storage/raw.js\";\n\n" +
      "export function aliasRender() {\n  return read();\n}\n",
  );
  fs.writeFileSync(
    path.join(projectRoot, "app/self-alias.js"),
    "import { rawValue as rawValue } from \"../storage/raw.js\";\n\n" +
      "export function selfAliasRender() {\n  return rawValue();\n}\n",
  );
  if (observed) {
    fs.writeFileSync(
      path.join(projectRoot, "app/peer.js"),
      "import { serviceValue } from \"../service/api.js\";\n\n" +
      "export function peer() {\n  return serviceValue();\n}\n",
    );
  }
  if (referenceOnly) {
    fs.writeFileSync(
      path.join(projectRoot, "app/reference.js"),
      "import { rawValue as selected } from \"../storage/raw.js\";\n\n" +
        "export { selected };\n",
    );
  }
  if (typescript) {
    fs.writeFileSync(
      path.join(projectRoot, "app/typed.ts"),
      "import { rawValue } from \"../storage/raw.js\";\n\n" +
      "export function typed(): number {\n  return rawValue();\n}\n",
    );
    fs.writeFileSync(
      path.join(projectRoot, "app/typed.tsx"),
      "import { rawValue } from \"../storage/raw.js\";\n\n" +
        "export function typedTsx(): number {\n  return rawValue();\n}\n",
    );
  }
  fs.writeFileSync(
    path.join(projectRoot, "service/api.js"),
    "import { rawValue } from \"../storage/raw.js\";\n\n" +
      "export function serviceValue() {\n  return rawValue();\n}\n",
  );
  fs.writeFileSync(
    path.join(projectRoot, "storage/raw.js"),
    "export function rawValue() {\n  return 7;\n}\n\n" +
      "export function other() {\n  return 8;\n}\n",
  );
  fs.writeFileSync(
    path.join(projectRoot, "package.json"),
    JSON.stringify({ private: true, type: "module" }),
  );
  fs.writeFileSync(
    path.join(projectRoot, "archbird.json"),
    JSON.stringify({
      project: "ecmascript-dependency-redirect",
      layers: [{
        name: "javascript",
        language: "javascript",
        globs: ["**/*.js"],
        import_roots: ["."],
      }, ...(typescript ? [{
        name: "typescript",
        language: "typescript",
        globs: ["**/*.ts", "**/*.tsx"],
        import_roots: ["."],
      }] : [])],
      components: [
        { name: "app", paths: ["app/**"] },
        { name: "service", paths: ["service/**"] },
        { name: "storage", paths: ["storage/**"] },
      ],
      constraints: {
        "APP-STORAGE-BOUNDARY": {
          kind: "forbidden_component_edges",
          edges: [{ source: "app", kind: "import", target: "storage" }],
          kinds: ["import"],
          owner: "architecture",
          rationale: "Application code reaches storage through service.",
        },
      },
    }),
  );
}

try {
  fs.symlinkSync(root, rootLink, "dir");
  assert.deepEqual(
    JSON.parse(
      actTransport.observeSourceRequirements(
        rootLink,
        Buffer.from('{"files":[],"absent":[],"observe":[]}'),
      ).toString("utf8"),
    ),
    { absent: [], files: [] },
  );

  fs.writeFileSync(
    path.join(root, "archbird.json"),
    JSON.stringify({
      project: "node-plan-act",
      constraints: {
        "NO-LEGACY": {
          kind: "forbidden_paths",
          paths: ["legacy.js"],
          owner: "architecture",
          rationale: "Legacy implementation stays absent.",
        },
      },
    }),
  );
  const legacy = path.join(root, "legacy.js");
  fs.writeFileSync(legacy, "module.exports = 1;\n");
  const plan = path.join(artifacts, "plan.json");
  const act = path.join(artifacts, "act.json");

  run(["plan", "NO-LEGACY", "--root", root, "--output", plan]);
  const planMarkdown = archbird
    .renderPlanMarkdown(fs.readFileSync(plan))
    .toString("utf8");
  const cliPlanMarkdown = run([
    "plan", "NO-LEGACY", "--root", root, "--format", "markdown",
  ]).stdout.toString("utf8");
  assert.equal(cliPlanMarkdown, planMarkdown);
  assert.match(planMarkdown, /^# Change Plan: node-plan-act/m);
  assert.match(planMarkdown, /### 1\. EXECUTABLE/);
  assert.match(planMarkdown, /- Acceptance: `NO-LEGACY`/);
  assert.match(
    planMarkdown,
    /Result: items=1; executable=1; input-required=0; unknowns=0;/,
  );
  const preview = run([
    "act", plan, "--root", root, "--format", "patch",
  ]).stdout.toString("utf8");
  assert.match(preview, /--- a\/legacy\.js/);
  assert.match(preview, /\+\+\+ \/dev\/null/);
  assert.equal(fs.existsSync(legacy), true, "Act mutated the repository");

  run([
    "act", plan, "--root", root, "--format", "json", "--output", act,
  ]);
  const document = JSON.parse(fs.readFileSync(act, "utf8"));
  assert.equal(document.artifact, "act");
  assert.equal(document.state, "accepted");
  assert.equal(document.acceptance.status, "satisfied");
  assert.deepEqual(document.acceptance.constraints, [
    { id: "NO-LEGACY", status: "pass" },
  ]);
  assert.equal(fs.existsSync(legacy), true, "Act creation mutated the repository");

  assert.equal(
    run(["apply", act, "--root", root]).stdout.toString("utf8"),
    "Result: applied-transitions=1; state=applied\n",
  );
  assert.equal(fs.existsSync(legacy), false);
  assert.equal(
    run(["apply", act, "--root", root]).stdout.toString("utf8"),
    "Result: applied-transitions=0; state=already-satisfied\n",
  );

  fs.writeFileSync(
    path.join(assertedRoot, "archbird.json"),
    JSON.stringify({
      project: "node-plan-act-asserted",
      constraints: {
        "IMPLEMENT-API": {
          kind: "required_symbols",
          symbols: ["futureApi"],
          paths: ["api.js"],
          kinds: ["function"],
          owner: "architecture",
          rationale: "The reviewed API implementation exists.",
        },
      },
    }),
  );
  fs.writeFileSync(
    path.join(assertedRoot, "api.js"),
    "export function currentApi() {\n  return 1;\n}\n",
  );
  const assertedPlan = path.join(artifacts, "asserted-plan.json");
  const assertedAct = path.join(artifacts, "asserted-act.json");
  const assertedReplacement = path.join(artifacts, "asserted-api.js");
  fs.writeFileSync(
    assertedReplacement,
    "export function currentApi() {\n  return 1;\n}\n\n" +
      "export function futureApi() {\n  return 2;\n}\n",
  );
  run(["plan", "--root", assertedRoot, "--output", assertedPlan]);
  const assertedPlanBytes = fs.readFileSync(assertedPlan);
  const assertedPlanDocument = JSON.parse(assertedPlanBytes);
  const assertedItem = assertedPlanDocument.items[0];
  assert.equal(assertedItem.operation.action, "add_symbol");
  assert.equal(assertedItem.executable, false);
  assert.match(
    run([
      "act", assertedPlan, "--root", assertedRoot, "--format", "json",
    ], 2).stderr.toString("utf8"),
    /manual or blocked item/,
  );
  run([
    "act", assertedPlan, "--root", assertedRoot,
    "--submit", `${assertedItem.id}=${assertedReplacement}`,
    "--format", "json", "--output", assertedAct,
  ]);
  assert.deepEqual(fs.readFileSync(assertedPlan), assertedPlanBytes);
  const assertedActDocument = JSON.parse(fs.readFileSync(assertedAct));
  assert.equal(assertedActDocument.state, "accepted");
  assert.equal(
    assertedActDocument.executors[0].capability,
    "archbird.asserted.source.replace-file@1",
  );
  assert.deepEqual(
    assertedActDocument.executors[0].item_ids,
    [assertedItem.id],
  );
  run(["apply", assertedAct, "--root", assertedRoot]);
  assert.match(
    fs.readFileSync(path.join(assertedRoot, "api.js"), "utf8"),
    /function futureApi/,
  );
  run(["verify", "--root", assertedRoot, "--check"]);

  fs.writeFileSync(
    path.join(assertedCreateRoot, "archbird.json"),
    JSON.stringify({
      project: "node-plan-act-asserted-create",
      layers: [{
        name: "javascript",
        language: "javascript",
        globs: ["**/*.js"],
        import_roots: ["."],
      }],
      constraints: {
        "IMPLEMENT-API": {
          kind: "required_symbols",
          symbols: ["futureApi"],
          paths: ["future-api.js"],
          kinds: ["function"],
          owner: "architecture",
          rationale: "The reviewed API implementation exists.",
        },
      },
    }),
  );
  fs.writeFileSync(
    path.join(assertedCreateRoot, "current.js"),
    "export function currentApi() {\n  return 1;\n}\n",
  );
  const assertedCreatePlan = path.join(artifacts, "asserted-create-plan.json");
  const assertedCreateAct = path.join(artifacts, "asserted-create-act.json");
  const assertedCreateInput = path.join(artifacts, "future-api.js");
  const assertedCreateDestination = path.join(
    assertedCreateRoot,
    "future-api.js",
  );
  fs.writeFileSync(
    assertedCreateInput,
    "export function futureApi() {\n  return 2;\n}\n",
  );
  run([
    "plan", "--root", assertedCreateRoot, "--output", assertedCreatePlan,
  ]);
  const assertedCreateItem =
    JSON.parse(fs.readFileSync(assertedCreatePlan)).items[0];
  assert.equal(assertedCreateItem.operation.action, "add_symbol");
  assert.equal(assertedCreateItem.operation.path, "future-api.js");
  assert.equal(assertedCreateItem.executable, false);
  run([
    "act", assertedCreatePlan, "--root", assertedCreateRoot,
    "--submit", `${assertedCreateItem.id}=${assertedCreateInput}`,
    "--format", "json", "--output", assertedCreateAct,
  ]);
  const assertedCreateDocument =
    JSON.parse(fs.readFileSync(assertedCreateAct));
  assert.equal(assertedCreateDocument.state, "accepted");
  assert.equal(
    assertedCreateDocument.executors[0].capability,
    "archbird.asserted.source.create-file@1",
  );
  assert.deepEqual(assertedCreateDocument.executors[0].reads, []);
  assert.deepEqual(
    assertedCreateDocument.executors[0].writes,
    ["future-api.js"],
  );
  assert.equal(assertedCreateDocument.transitions[0].kind, "create");
  assert.equal(fs.existsSync(assertedCreateDestination), false);
  assert.equal(
    run([
      "apply", assertedCreateAct, "--root", assertedCreateRoot,
    ]).stdout.toString("utf8"),
    "Result: applied-transitions=1; state=applied\n",
  );
  assert.match(
    fs.readFileSync(assertedCreateDestination, "utf8"),
    /function futureApi/,
  );
  run(["verify", "--root", assertedCreateRoot, "--check"]);
  assert.equal(
    run([
      "apply", assertedCreateAct, "--root", assertedCreateRoot,
    ]).stdout.toString("utf8"),
    "Result: applied-transitions=0; state=already-satisfied\n",
  );

  fs.writeFileSync(
    path.join(requiredEdgeRoot, "archbird.json"),
    JSON.stringify({
      project: "node-plan-act-required-edge",
      layers: [{
        name: "python",
        language: "python",
        globs: ["*.py"],
        import_roots: ["."],
      }],
      constraints: {
        "REQUIRED-IMPORT": {
          kind: "required_file_edge",
          edge_kind: "import",
          source: "consumer.py",
          target: "provider.py",
          name: "provider",
          owner: "architecture",
          rationale: "The consumer imports the reviewed provider.",
        },
      },
    }),
  );
  const requiredEdgeConsumer = path.join(requiredEdgeRoot, "consumer.py");
  fs.writeFileSync(requiredEdgeConsumer, "def consume():\n    return 1\n");
  fs.writeFileSync(path.join(requiredEdgeRoot, "provider.py"), "VALUE = 1\n");
  const requiredEdgePlan = path.join(
    artifacts,
    "required-edge-plan.json",
  );
  const requiredEdgeAct = path.join(artifacts, "required-edge-act.json");
  const requiredEdgeInvalid = path.join(
    artifacts,
    "required-edge-invalid.py",
  );
  const requiredEdgeSubmitted = path.join(
    artifacts,
    "required-edge-consumer.py",
  );
  run(["plan", "--root", requiredEdgeRoot, "--output", requiredEdgePlan]);
  const requiredEdgePlanBytes = fs.readFileSync(requiredEdgePlan);
  const requiredEdgeDocument = JSON.parse(requiredEdgePlanBytes);
  const requiredEdgeItem = requiredEdgeDocument.items[0];
  assert.deepEqual(requiredEdgeItem.operation, {
    action: "add_dependency",
    name: "provider",
    relation: "import",
    source_path: "consumer.py",
    target_path: "provider.py",
  });
  assert.equal(requiredEdgeItem.executable, false);
  assert.match(
    run([
      "act", requiredEdgePlan, "--root", requiredEdgeRoot,
    ], 2).stderr.toString("utf8"),
    /manual or blocked item/,
  );
  fs.writeFileSync(requiredEdgeInvalid, "def consume():\n    return 2\n");
  assert.match(
    run([
      "act", requiredEdgePlan, "--root", requiredEdgeRoot,
      "--submit", `${requiredEdgeItem.id}=${requiredEdgeInvalid}`,
    ], 2).stderr.toString("utf8"),
    /fresh Verification has a failing constraint/,
  );
  assert.equal(
    fs.readFileSync(requiredEdgeConsumer, "utf8"),
    "def consume():\n    return 1\n",
  );
  fs.writeFileSync(
    requiredEdgeSubmitted,
    "import provider\n\n\ndef consume():\n    return provider.VALUE\n",
  );
  const requiredEdgePatch = run([
    "act", requiredEdgePlan, "--root", requiredEdgeRoot,
    "--submit", `${requiredEdgeItem.id}=${requiredEdgeSubmitted}`,
    "--format", "patch",
  ]).stdout.toString("utf8");
  assert.match(requiredEdgePatch, /\+import provider/);
  assert.equal(
    fs.readFileSync(requiredEdgeConsumer, "utf8"),
    "def consume():\n    return 1\n",
  );
  run([
    "act", requiredEdgePlan, "--root", requiredEdgeRoot,
    "--submit", `${requiredEdgeItem.id}=${requiredEdgeSubmitted}`,
    "--format", "json", "--output", requiredEdgeAct,
  ]);
  assert.deepEqual(fs.readFileSync(requiredEdgePlan), requiredEdgePlanBytes);
  const acceptedRequiredEdge = JSON.parse(
    fs.readFileSync(requiredEdgeAct, "utf8"),
  );
  assert.equal(acceptedRequiredEdge.acceptance.status, "satisfied");
  assert.deepEqual(
    acceptedRequiredEdge.executors[0].reads,
    ["consumer.py"],
  );
  assert.deepEqual(
    acceptedRequiredEdge.executors[0].writes,
    ["consumer.py"],
  );
  assert.equal(acceptedRequiredEdge.transitions[0].kind, "modify");
  run(["apply", requiredEdgeAct, "--root", requiredEdgeRoot]);
  assert.equal(
    fs.readFileSync(requiredEdgeConsumer, "utf8"),
    fs.readFileSync(requiredEdgeSubmitted, "utf8"),
  );
  assert.equal(
    fs.readFileSync(path.join(requiredEdgeRoot, "provider.py"), "utf8"),
    "VALUE = 1\n",
  );
  run(["verify", "--root", requiredEdgeRoot, "--check"]);

  fs.writeFileSync(
    path.join(forbiddenEdgeRoot, "archbird.json"),
    JSON.stringify({
      project: "node-plan-act-forbidden-edge",
      layers: [{
        name: "python",
        language: "python",
        globs: ["*.py"],
        import_roots: ["."],
      }],
      constraints: {
        "FORBIDDEN-IMPORT": {
          kind: "forbidden_file_edges",
          edges: [{
            source: "consumer.py",
            kind: "import",
            target: "provider.py",
          }],
          from_paths: ["consumer.py"],
          to_paths: ["provider.py"],
          kinds: ["import"],
          owner: "architecture",
          rationale: "The consumer does not import this provider.",
        },
      },
    }),
  );
  const forbiddenEdgeConsumer = path.join(forbiddenEdgeRoot, "consumer.py");
  const forbiddenEdgeBefore =
    "import provider\n\n\ndef consume():\n    return provider.VALUE\n";
  fs.writeFileSync(forbiddenEdgeConsumer, forbiddenEdgeBefore);
  fs.writeFileSync(
    path.join(forbiddenEdgeRoot, "provider.py"),
    "VALUE = 1\n",
  );
  const forbiddenEdgePlan = path.join(
    artifacts,
    "forbidden-edge-plan.json",
  );
  const forbiddenEdgeAct = path.join(artifacts, "forbidden-edge-act.json");
  const forbiddenEdgeInvalid = path.join(
    artifacts,
    "forbidden-edge-invalid.py",
  );
  const forbiddenEdgeSubmitted = path.join(
    artifacts,
    "forbidden-edge-consumer.py",
  );
  run(["plan", "--root", forbiddenEdgeRoot, "--output", forbiddenEdgePlan]);
  const forbiddenEdgePlanBytes = fs.readFileSync(forbiddenEdgePlan);
  const forbiddenEdgeDocument = JSON.parse(forbiddenEdgePlanBytes);
  const forbiddenEdgeItem = forbiddenEdgeDocument.items[0];
  assert.deepEqual(forbiddenEdgeItem.operation, {
    action: "remove_dependency",
    relation: "import",
    source_path: "consumer.py",
    target_path: "provider.py",
  });
  assert.equal(forbiddenEdgeItem.executable, false);
  assert.match(
    run([
      "act", forbiddenEdgePlan, "--root", forbiddenEdgeRoot,
    ], 2).stderr.toString("utf8"),
    /manual or blocked item/,
  );
  fs.writeFileSync(
    forbiddenEdgeInvalid,
    forbiddenEdgeBefore.replace("VALUE", "VALUE + 0"),
  );
  assert.match(
    run([
      "act", forbiddenEdgePlan, "--root", forbiddenEdgeRoot,
      "--submit", `${forbiddenEdgeItem.id}=${forbiddenEdgeInvalid}`,
    ], 2).stderr.toString("utf8"),
    /fresh Verification has a failing constraint/,
  );
  assert.equal(
    fs.readFileSync(forbiddenEdgeConsumer, "utf8"),
    forbiddenEdgeBefore,
  );
  fs.writeFileSync(
    forbiddenEdgeSubmitted,
    "def consume():\n    return 1\n",
  );
  const forbiddenEdgePatch = run([
    "act", forbiddenEdgePlan, "--root", forbiddenEdgeRoot,
    "--submit", `${forbiddenEdgeItem.id}=${forbiddenEdgeSubmitted}`,
    "--format", "patch",
  ]).stdout.toString("utf8");
  assert.match(forbiddenEdgePatch, /-import provider/);
  assert.equal(
    fs.readFileSync(forbiddenEdgeConsumer, "utf8"),
    forbiddenEdgeBefore,
  );
  run([
    "act", forbiddenEdgePlan, "--root", forbiddenEdgeRoot,
    "--submit", `${forbiddenEdgeItem.id}=${forbiddenEdgeSubmitted}`,
    "--format", "json", "--output", forbiddenEdgeAct,
  ]);
  assert.deepEqual(fs.readFileSync(forbiddenEdgePlan), forbiddenEdgePlanBytes);
  const acceptedForbiddenEdge = JSON.parse(
    fs.readFileSync(forbiddenEdgeAct, "utf8"),
  );
  assert.equal(acceptedForbiddenEdge.acceptance.status, "satisfied");
  assert.deepEqual(
    acceptedForbiddenEdge.executors[0].reads,
    ["consumer.py"],
  );
  assert.deepEqual(
    acceptedForbiddenEdge.executors[0].writes,
    ["consumer.py"],
  );
  assert.equal(acceptedForbiddenEdge.transitions[0].kind, "modify");
  run(["apply", forbiddenEdgeAct, "--root", forbiddenEdgeRoot]);
  assert.equal(
    fs.readFileSync(forbiddenEdgeConsumer, "utf8"),
    fs.readFileSync(forbiddenEdgeSubmitted, "utf8"),
  );
  run(["verify", "--root", forbiddenEdgeRoot, "--check"]);

  fs.writeFileSync(
    path.join(root, "api.js"),
    "export function oldApi(value) {\n  return value + 1;\n}\n",
  );
  fs.writeFileSync(
    path.join(root, "consumer.js"),
    "import { oldApi } from \"./api.js\";\nexport const result = oldApi(1);\n",
  );
  fs.writeFileSync(
    path.join(root, "unrelated.js"),
    "export function oldApi(value) {\n  return value - 1;\n}\n",
  );
  fs.writeFileSync(
    path.join(root, "archbird.json"),
    JSON.stringify({
      project: "node-plan-act-rename",
      layers: [{
        name: "javascript",
        language: "javascript",
        globs: ["*.js"],
        import_roots: ["."],
      }],
      projections: {
        "api-symbols": {
          select: "symbols",
          paths: ["api.js"],
        },
      },
      constraints: {
        "API-SURFACE": {
          assert: "set_equal",
          actual: { projection: "api-symbols" },
          expected: { literal: ["newApi"] },
          owner: "architecture",
          rationale: "The reviewed API rename is complete.",
        },
      },
    }),
  );
  const renamePlan = path.join(artifacts, "rename-plan.json");
  const renameAct = path.join(artifacts, "rename-act.json");
  run([
    "plan", "API-SURFACE", "--root", root,
    "--rename", "oldApi=newApi", "--output", renamePlan,
  ]);
  const renameDocument = JSON.parse(fs.readFileSync(renamePlan, "utf8"));
  assert.equal(renameDocument.items.length, 1);
  assert.equal(renameDocument.items[0].operation.action, "rename_symbol");
  assert.equal(renameDocument.items[0].executable, true);
  assert.deepEqual(
    renameDocument.items[0].operation.source_paths,
    ["api.js", "consumer.js"],
  );
  assert.equal(
    renameDocument.items[0].operation.projection_id,
    "plan-symbol-occurrences",
  );
  assert.match(
    renameDocument.items[0].operation.projection_content_sha256,
    /^[0-9a-f]{64}$/,
  );
  assert.equal("sites" in renameDocument.items[0].operation, false);
  assert.equal("coverage" in renameDocument.items[0].operation, false);
  assert.deepEqual(renameDocument.items[0].operation.projection.paths, ["api.js"]);
  run([
    "act", renamePlan, "--root", root, "--format", "json",
    "--output", renameAct,
  ]);
  const acceptedRename = JSON.parse(fs.readFileSync(renameAct, "utf8"));
  assert.equal(acceptedRename.state, "accepted");
  assert.equal(acceptedRename.transitions.length, 2);
  run(["apply", renameAct, "--root", root]);
  assert.doesNotMatch(fs.readFileSync(path.join(root, "api.js"), "utf8"), /oldApi/);
  assert.doesNotMatch(
    fs.readFileSync(path.join(root, "consumer.js"), "utf8"),
    /oldApi/,
  );
  assert.match(
    fs.readFileSync(path.join(root, "unrelated.js"), "utf8"),
    /oldApi/,
  );
  run(["verify", "--root", root, "--check"]);

  fs.cpSync(
    path.join(repository, "test/fixtures/plan_act/surface_closure"),
    surfaceRoot,
    { recursive: true },
  );
  const surfacePlan = path.join(artifacts, "surface-plan.json");
  const surfaceAct = path.join(artifacts, "surface-act.json");
  run([
    "plan", "FFI-SURFACE", "--root", surfaceRoot,
    "--rename", "core_add=core_sum", "--output", surfacePlan,
  ]);
  const surfaceDocument = JSON.parse(fs.readFileSync(surfacePlan, "utf8"));
  assert.equal(surfaceDocument.items.length, 1);
  assert.equal(surfaceDocument.items[0].executable, true);
  assert.deepEqual(surfaceDocument.items[0].operation, {
    action: "rename_provider_capability",
    from: "core_add",
    provider: makeProvider,
    surface: "ffi",
    to: "core_sum",
  });
  run([
    "act", surfacePlan, "--root", surfaceRoot, "--format", "json",
    "--output", surfaceAct,
  ]);
  const acceptedSurface = JSON.parse(fs.readFileSync(surfaceAct, "utf8"));
  assert.equal(acceptedSurface.state, "accepted");
  assert.deepEqual(acceptedSurface.acceptance.constraints, [
    { id: "FFI-SURFACE", status: "pass" },
  ]);
  run(["apply", surfaceAct, "--root", surfaceRoot]);
  assert.match(
    fs.readFileSync(path.join(surfaceRoot, "Makefile"), "utf8"),
    /WASM_EXPORTS = _core_sum/,
  );
  run(["verify", "--root", surfaceRoot, "--check"]);

  fs.writeFileSync(
    path.join(surfaceRoot, "Makefile"),
    fs.readFileSync(path.join(surfaceRoot, "Makefile"), "utf8")
      .replace("WASM_EXPORTS = _core_sum", "WASM_EXPORTS = _core_sum _core_add"),
  );
  const removalPlan = path.join(artifacts, "surface-removal-plan.json");
  const removalAct = path.join(artifacts, "surface-removal-act.json");
  run([
    "plan", "FFI-SURFACE", "--root", surfaceRoot,
    "--output", removalPlan,
  ]);
  const removalDocument = JSON.parse(fs.readFileSync(removalPlan, "utf8"));
  assert.equal(removalDocument.items.length, 1);
  assert.equal(removalDocument.items[0].executable, true);
  assert.equal(removalDocument.items[0].provenance, "derived");
  assert.equal(
    removalDocument.items[0].statement,
    "Remove stale provider registration core_add from Makefile.",
  );
  assert.deepEqual(removalDocument.items[0].operation, {
    action: "remove_provider_capability",
    capability: "core_add",
    provider: makeProvider,
    surface: "ffi",
  });
  run([
    "act", removalPlan, "--root", surfaceRoot, "--format", "json",
    "--output", removalAct,
  ]);
  run(["apply", removalAct, "--root", surfaceRoot]);
  assert.doesNotMatch(
    fs.readFileSync(path.join(surfaceRoot, "Makefile"), "utf8"),
    /_core_add/,
  );
  run(["verify", "FFI-SURFACE", "--root", surfaceRoot, "--check"]);

  fs.cpSync(
    path.join(repository, "test/fixtures/plan_act/surface_registration"),
    registrationRoot,
    { recursive: true },
  );
  const registrationConfigurationPath = path.join(
    registrationRoot,
    "archbird.json",
  );
  const registrationConfiguration = JSON.parse(
    fs.readFileSync(registrationConfigurationPath, "utf8"),
  );
  registrationConfiguration.bridges[0].providers.push({
    kind: "file_pattern",
    path: "src/core.h",
    pattern: "\\b(core_[A-Za-z0-9_]+)\\s*\\(",
  });
  registrationConfiguration.constraints["FFI-SURFACE"]
    .require_all_providers = true;
  fs.writeFileSync(
    registrationConfigurationPath,
    JSON.stringify(registrationConfiguration),
  );
  const registrationPlan = path.join(artifacts, "registration-plan.json");
  const registrationAct = path.join(artifacts, "registration-act.json");
  const incompleteRegistration = run([
    "verify", "FFI-SURFACE", "--root", registrationRoot, "--check",
  ], 1);
  assert.match(
    Buffer.from(incompleteRegistration.stdout).toString("utf8"),
    /provider capability is absent from 1 of 2 configured providers: core_sum/,
  );
  run([
    "plan", "FFI-SURFACE", "--root", registrationRoot,
    "--output", registrationPlan,
  ]);
  const registrationDocument = JSON.parse(
    fs.readFileSync(registrationPlan, "utf8"),
  );
  assert.equal(registrationDocument.items.length, 1);
  assert.equal(registrationDocument.items[0].executable, true);
  assert.equal(registrationDocument.items[0].provenance, "derived");
  assert.equal(registrationDocument.items[0].origins.length, 1);
  assert.deepEqual(registrationDocument.items[0].operation, {
    action: "add_provider_capability",
    capability: "core_sum",
    provider: makeProvider,
    surface: "ffi",
  });
  run([
    "act", registrationPlan, "--root", registrationRoot, "--format", "json",
    "--output", registrationAct,
  ]);
  const acceptedRegistration = JSON.parse(
    fs.readFileSync(registrationAct, "utf8"),
  );
  assert.equal(acceptedRegistration.state, "accepted");
  run(["apply", registrationAct, "--root", registrationRoot]);
  assert.match(
    fs.readFileSync(path.join(registrationRoot, "Makefile"), "utf8"),
    /WASM_EXPORTS = _core_peer _core_sum/,
  );
  run(["verify", "FFI-SURFACE", "--root", registrationRoot, "--check"]);

  const surfaceFixture = path.join(
    repository,
    "test/fixtures/plan_act/surface_closure",
  );
  const registrationFixture = path.join(
    repository,
    "test/fixtures/plan_act/surface_registration",
  );
  fs.cpSync(registrationFixture, providerParityRoot, { recursive: true });
  const providerParityConfigurationPath = path.join(
    providerParityRoot,
    "archbird.json",
  );
  const providerParityConfiguration = JSON.parse(
    fs.readFileSync(providerParityConfigurationPath, "utf8"),
  );
  providerParityConfiguration.bridges[0].providers.push({
    kind: "file_pattern",
    path: "src/core.h",
    pattern: "\\b(core_[A-Za-z0-9_]+)\\s*\\(",
  });
  providerParityConfiguration.constraints["FFI-SURFACE"]
    .require_all_providers = true;
  fs.writeFileSync(
    providerParityConfigurationPath,
    JSON.stringify(providerParityConfiguration),
  );
  const providerParityHeader = path.join(providerParityRoot, "src/core.h");
  fs.writeFileSync(
    providerParityHeader,
    fs.readFileSync(providerParityHeader, "utf8").replace(
      "int core_sum(int left, int right);\n",
      "",
    ),
  );
  const providerParityPlan = path.join(
    artifacts,
    "provider-parity-plan.json",
  );
  const providerParityAct = path.join(
    artifacts,
    "provider-parity-act.json",
  );
  run([
    "plan", "FFI-SURFACE", "--root", providerParityRoot,
    "--output", providerParityPlan,
  ]);
  const providerParityDocument = JSON.parse(
    fs.readFileSync(providerParityPlan, "utf8"),
  );
  const providerParityByKind = Object.fromEntries(
    providerParityDocument.items.map(
      (item) => [item.operation.provider.kind, item],
    ),
  );
  assert.deepEqual(
    Object.keys(providerParityByKind).sort(),
    ["file_pattern", "make_variable"],
  );
  assert.deepEqual(providerParityByKind.file_pattern.depends_on, []);
  assert.deepEqual(
    providerParityByKind.make_variable.depends_on,
    [providerParityByKind.file_pattern.id],
  );
  assert.deepEqual(
    providerParityByKind.file_pattern.operation.source_paths,
    ["src/core.c", "src/core.h"],
  );
  run([
    "act", providerParityPlan, "--root", providerParityRoot,
    "--format", "json", "--output", providerParityAct,
  ]);
  const providerParityAccepted = JSON.parse(
    fs.readFileSync(providerParityAct, "utf8"),
  );
  assert.deepEqual(
    providerParityAccepted.executors
      .map((row) => row.capability)
      .sort(),
    [
      "archbird.native.c.provider-capability@1",
      "archbird.native.make.provider-capability@1",
    ],
  );
  run(["apply", providerParityAct, "--root", providerParityRoot]);
  run(["verify", "FFI-SURFACE", "--root", providerParityRoot, "--check"]);

  fs.cpSync(registrationFixture, coordinatedRoot, { recursive: true });
  const coordinatedHeader = path.join(coordinatedRoot, "src/core.h");
  fs.writeFileSync(
    coordinatedHeader,
    fs.readFileSync(coordinatedHeader, "utf8").replace(
      "int core_sum(int left, int right);\n",
      "",
    ),
  );
  const coordinatedPlan = path.join(artifacts, "coordinated-plan.json");
  const coordinatedAct = path.join(artifacts, "coordinated-act.json");
  run(["plan", "--root", coordinatedRoot, "--output", coordinatedPlan]);
  const coordinatedDocument = JSON.parse(
    fs.readFileSync(coordinatedPlan, "utf8"),
  );
  assert.deepEqual(
    coordinatedDocument.items
      .map((item) => item.operation.action)
      .sort(),
    ["add_provider_capability", "declare_symbol"],
  );
  const coordinatedDeclaration = coordinatedDocument.items.find(
    (item) => item.operation.action === "declare_symbol",
  );
  const coordinatedRegistration = coordinatedDocument.items.find(
    (item) => item.operation.action === "add_provider_capability",
  );
  assert.deepEqual(coordinatedDeclaration.depends_on, []);
  assert.deepEqual(
    coordinatedRegistration.depends_on,
    [coordinatedDeclaration.id],
  );
  assert.deepEqual(
    coordinatedDeclaration.operation,
    {
      action: "declare_symbol",
      path: "src/core.h",
      source_paths: ["src/core.c", "src/core.h"],
      symbol: "core_sum",
    },
  );
  run([
    "act", coordinatedPlan, "--root", coordinatedRoot, "--format", "json",
    "--output", coordinatedAct,
  ]);
  const coordinatedActDocument = JSON.parse(
    fs.readFileSync(coordinatedAct, "utf8"),
  );
  const coordinatedExecutors = Object.fromEntries(
    coordinatedActDocument.executors.map((row) => [row.capability, row]),
  );
  assert.deepEqual(
    Object.keys(coordinatedExecutors).sort(),
    [
      "archbird.native.c.declare-symbol@1",
      "archbird.native.make.provider-capability@1",
    ],
  );
  assert.deepEqual(
    coordinatedExecutors["archbird.native.c.declare-symbol@1"],
    {
      capability: "archbird.native.c.declare-symbol@1",
      deterministic: true,
      implementation_sha256:
        coordinatedActDocument.tool.implementation_sha256,
      item_ids: [coordinatedDeclaration.id],
      matches: 1,
      reads: ["src/core.c", "src/core.h"],
      skipped: 0,
      unsupported: 0,
      writes: ["src/core.h"],
    },
  );
  assert.deepEqual(
    coordinatedExecutors["archbird.native.make.provider-capability@1"],
    {
      capability: "archbird.native.make.provider-capability@1",
      deterministic: true,
      implementation_sha256:
        coordinatedActDocument.tool.implementation_sha256,
      item_ids: [coordinatedRegistration.id],
      matches: 1,
      reads: ["Makefile"],
      skipped: 0,
      unsupported: 0,
      writes: ["Makefile"],
    },
  );
  run(["apply", coordinatedAct, "--root", coordinatedRoot]);
  run(["verify", "--root", coordinatedRoot, "--check"]);

  fs.cpSync(
    path.join(repository, "test/fixtures/sample"),
    napiRoot,
    { recursive: true },
  );
  const napiSource = path.join(napiRoot, "native/addon.c");
  const missingNapiRegistration = fs.readFileSync(napiSource, "utf8").replace(
    '  {"core_mul", NULL, napi_core_mul},\n',
    "",
  );
  fs.writeFileSync(napiSource, missingNapiRegistration);
  const napiPlan = path.join(artifacts, "napi-plan.json");
  const napiAct = path.join(artifacts, "napi-act.json");
  run([
    "verify", "PROVIDER-SURFACE-JS-BINDING", "--root", napiRoot, "--check",
  ], 1);
  run([
    "plan", "PROVIDER-SURFACE-JS-BINDING", "--root", napiRoot,
    "--output", napiPlan,
  ]);
  const napiPlanDocument = JSON.parse(fs.readFileSync(napiPlan, "utf8"));
  assert.equal(napiPlanDocument.items.length, 1);
  assert.equal(napiPlanDocument.items[0].executable, true);
  assert.deepEqual(
    {
      action: napiPlanDocument.items[0].operation.action,
      capability: napiPlanDocument.items[0].operation.capability,
      kind: napiPlanDocument.items[0].operation.provider.kind,
      path: napiPlanDocument.items[0].operation.provider.path,
      source_paths: napiPlanDocument.items[0].operation.source_paths,
      surface: napiPlanDocument.items[0].operation.surface,
    },
    {
      action: "add_provider_capability",
      capability: "core_mul",
      kind: "exports",
      path: "native/addon.c",
      source_paths: ["native/addon.c"],
      surface: "js-binding",
    },
  );
  run([
    "act", napiPlan, "--root", napiRoot, "--format", "json",
    "--output", napiAct,
  ]);
  const napiActDocument = JSON.parse(fs.readFileSync(napiAct, "utf8"));
  assert.equal(napiActDocument.state, "accepted");
  assert.deepEqual(
    napiActDocument.executors.map((row) => row.capability),
    ["archbird.native.c.napi-export@1"],
  );
  assert.equal(
    fs.readFileSync(napiSource, "utf8"),
    missingNapiRegistration,
    "Node Act preview mutated the repository",
  );
  run(["apply", napiAct, "--root", napiRoot]);
  assert.match(
    fs.readFileSync(napiSource, "utf8"),
    /\{"core_mul", NULL, napi_core_mul\}/,
  );
  run([
    "verify", "PROVIDER-SURFACE-JS-BINDING", "--root", napiRoot, "--check",
  ]);

  const observedProject = path.join(observedRoot, "packages", "surface");
  fs.cpSync(surfaceFixture, observedProject, { recursive: true });
  for (const relative of [
    "src/core.c",
    "src/core.h",
    "src/test_core.c",
    "py/api.py",
    "py/test_api.py",
    "js/runtime.js",
    "js/test_api.js",
  ]) {
    const source = path.join(observedProject, relative);
    fs.writeFileSync(
      source,
      fs.readFileSync(source, "utf8").replaceAll("core_sum", "core_add"),
    );
  }
  git(["init", "-q"], observedRoot);
  git(["add", "."], observedRoot);
  git([
    "-c", "user.name=Archbird Test",
    "-c", "user.email=archbird@example.invalid",
    "commit", "-qm", "before migration",
  ], observedRoot);
  for (const relative of [
    "src/core.c",
    "src/core.h",
    "src/test_core.c",
    "py/api.py",
    "py/test_api.py",
    "js/runtime.js",
    "js/test_api.js",
  ]) {
    const source = path.join(observedProject, relative);
    fs.writeFileSync(
      source,
      fs.readFileSync(source, "utf8").replaceAll("core_add", "core_sum"),
    );
  }
  const observedStatus = git(
    ["status", "--porcelain=v1", "-z"],
    observedRoot,
  );
  const observedPlan = path.join(artifacts, "observed-plan.json");
  const observedAct = path.join(artifacts, "observed-act.json");
  run([
    "plan", "FFI-SURFACE", "--root", observedProject,
    "--git-diff", "HEAD", "--output", observedPlan,
  ]);
  assert.deepEqual(
    git(["status", "--porcelain=v1", "-z"], observedRoot),
    observedStatus,
    "Git-derived planning mutated the working tree",
  );
  const observedDocument = JSON.parse(fs.readFileSync(observedPlan, "utf8"));
  assert.equal(observedDocument.schema_version, 7);
  assert.equal(observedDocument.items.length, 1);
  assert.equal(observedDocument.items[0].provenance, "derived");
  assert.equal(observedDocument.items[0].executable, true);
  assert.deepEqual(observedDocument.items[0].operation, {
    action: "rename_provider_capability",
    from: "core_add",
    provider: makeProvider,
    surface: "ffi",
    to: "core_sum",
  });
  assert.ok(observedDocument.source.before_map);
  run([
    "act", observedPlan, "--root", observedProject, "--format", "json",
    "--output", observedAct,
  ]);
  run(["apply", observedAct, "--root", observedProject]);
  run(["verify", "FFI-SURFACE", "--root", observedProject, "--check"]);

  const redirectFixture = path.join(
    repository,
    "test/fixtures/plan_act/dependency_redirect",
  );
  fs.cpSync(redirectFixture, redirectRoot, { recursive: true });
  const redirectPlan = path.join(artifacts, "redirect-plan.json");
  const redirectAct = path.join(artifacts, "redirect-act.json");
  run([
    "plan", "UI-STORAGE-BOUNDARY", "--root", redirectRoot,
    "--redirect", "raw_value=service_value", "--output", redirectPlan,
  ]);
  const redirectDocument = JSON.parse(
    fs.readFileSync(redirectPlan, "utf8"),
  );
  assert.equal(
    redirectDocument.items[0].operation.action,
    "redirect_dependency",
  );
  assert.equal(
    Object.hasOwn(redirectDocument.items[0].operation, "replacement"),
    false,
  );
  run([
    "act", redirectPlan, "--root", redirectRoot, "--format", "json",
    "--output", redirectAct,
  ]);
  assert.equal(JSON.parse(fs.readFileSync(redirectAct)).artifact, "act");
  run(["apply", redirectAct, "--root", redirectRoot]);
  run(["verify", "--root", redirectRoot, "--check"]);

  configureEcmascriptRedirect(ecmascriptRedirectRoot, { typescript: true });
  const ecmascriptPlan = path.join(
    artifacts,
    "ecmascript-redirect-plan.json",
  );
  const ecmascriptAct = path.join(artifacts, "ecmascript-redirect-act.json");
  run([
    "plan", "APP-STORAGE-BOUNDARY", "--root", ecmascriptRedirectRoot,
    "--redirect", "rawValue=serviceValue", "--output", ecmascriptPlan,
  ]);
  const ecmascriptDocument = JSON.parse(
    fs.readFileSync(ecmascriptPlan, "utf8"),
  );
  assert.equal(
    ecmascriptDocument.items[0].operation.action,
    "redirect_dependency",
  );
  assert.deepEqual(
    ecmascriptDocument.items[0].operation.source_paths,
    [
      "app/alias.js",
      "app/main.js",
      "app/self-alias.js",
      "app/typed.ts",
      "app/typed.tsx",
    ],
  );
  assert.equal(
    Object.hasOwn(ecmascriptDocument.items[0].operation, "replacement"),
    false,
  );
  const ecmascriptPreview = run([
    "act", ecmascriptPlan, "--root", ecmascriptRedirectRoot,
    "--format", "patch",
  ]).stdout.toString("utf8");
  assert.match(
    ecmascriptPreview,
    /\+import \{ serviceValue as read \} from "\.\.\/service\/api\.js";/,
  );
  assert.match(
    ecmascriptPreview,
    /\+import \{ serviceValue as rawValue \} from "\.\.\/service\/api\.js";/,
  );
  assert.match(
    ecmascriptPreview,
    /\+import \{ serviceValue \} from "\.\.\/service\/api\.js";/,
  );
  assert.match(ecmascriptPreview, /\+\s+return serviceValue\(\);/);
  assert.doesNotMatch(ecmascriptPreview, /-\s+return read\(\);/);
  run([
    "act", ecmascriptPlan, "--root", ecmascriptRedirectRoot,
    "--format", "json", "--output", ecmascriptAct,
  ]);
  const acceptedEcmascript = JSON.parse(
    fs.readFileSync(ecmascriptAct, "utf8"),
  );
  assert.equal(acceptedEcmascript.artifact, "act");
  assert.deepEqual(
    acceptedEcmascript.transitions.map((row) => row.path),
    [
      "app/alias.js",
      "app/main.js",
      "app/self-alias.js",
      "app/typed.ts",
      "app/typed.tsx",
    ],
  );
  run(["apply", ecmascriptAct, "--root", ecmascriptRedirectRoot]);
  run(["verify", "--root", ecmascriptRedirectRoot, "--check"]);
  const behavior = spawnSync(
    process.execPath,
    [
      "--input-type=module",
      "-e",
      "import { render } from './app/main.js'; " +
        "import { aliasRender } from './app/alias.js'; " +
        "import { selfAliasRender } from './app/self-alias.js'; " +
        "if (render() !== 7 || aliasRender() !== 7 || " +
        "selfAliasRender() !== 7) process.exit(1);",
    ],
    { cwd: ecmascriptRedirectRoot, encoding: null },
  );
  assert.equal(
    behavior.status,
    0,
    Buffer.from(behavior.stderr || []).toString("utf8"),
  );

  configureEcmascriptRedirect(ecmascriptUnobservedRoot, { observed: false });
  const unobservedPlan = path.join(
    artifacts,
    "ecmascript-unobserved-plan.json",
  );
  run([
    "plan", "APP-STORAGE-BOUNDARY", "--root", ecmascriptUnobservedRoot,
    "--redirect", "rawValue=serviceValue", "--output", unobservedPlan,
  ]);
  assert.match(
    run([
      "act", unobservedPlan, "--root", ecmascriptUnobservedRoot,
      "--format", "patch",
    ], 2).stderr.toString("utf8"),
    /no unique observed ECMAScript module in the source directory/,
  );

  configureEcmascriptRedirect(ecmascriptMultiRoot, { multiple: true });
  const multiPlan = path.join(artifacts, "ecmascript-multi-plan.json");
  run([
    "plan", "APP-STORAGE-BOUNDARY", "--root", ecmascriptMultiRoot,
    "--redirect", "rawValue=serviceValue", "--output", multiPlan,
  ]);
  assert.match(
    run([
      "act", multiPlan, "--root", ecmascriptMultiRoot, "--format", "patch",
    ], 2).stderr.toString("utf8"),
    /requires one exact named import/,
  );

  configureEcmascriptRedirect(
    ecmascriptReferenceRoot,
    { referenceOnly: true },
  );
  const referencePlan = path.join(artifacts, "ecmascript-reference-plan.json");
  run([
    "plan", "APP-STORAGE-BOUNDARY", "--root", ecmascriptReferenceRoot,
    "--redirect", "rawValue=serviceValue", "--output", referencePlan,
  ]);
  assert.match(
    run([
      "act", referencePlan, "--root", ecmascriptReferenceRoot,
      "--format", "patch",
    ], 2).stderr.toString("utf8"),
    /no matching exact symbol call|non-call symbol reference|unhandled semantic reference/,
  );

  process.stdout.write("node Plan/Act CLI lifecycle passed\n");
} finally {
  fs.rmSync(rootLink, { force: true });
  fs.rmSync(root, { force: true, recursive: true });
  fs.rmSync(artifacts, { force: true, recursive: true });
  fs.rmSync(surfaceRoot, { force: true, recursive: true });
  fs.rmSync(registrationRoot, { force: true, recursive: true });
  fs.rmSync(providerParityRoot, { force: true, recursive: true });
  fs.rmSync(coordinatedRoot, { force: true, recursive: true });
  fs.rmSync(napiRoot, { force: true, recursive: true });
  fs.rmSync(observedRoot, { force: true, recursive: true });
  fs.rmSync(assertedRoot, { force: true, recursive: true });
  fs.rmSync(assertedCreateRoot, { force: true, recursive: true });
  fs.rmSync(requiredEdgeRoot, { force: true, recursive: true });
  fs.rmSync(forbiddenEdgeRoot, { force: true, recursive: true });
  fs.rmSync(redirectRoot, { force: true, recursive: true });
  fs.rmSync(ecmascriptRedirectRoot, { force: true, recursive: true });
  fs.rmSync(ecmascriptUnobservedRoot, { force: true, recursive: true });
  fs.rmSync(ecmascriptMultiRoot, { force: true, recursive: true });
  fs.rmSync(ecmascriptReferenceRoot, { force: true, recursive: true });
}
