"use strict";

const assert = require("node:assert/strict");
const { createHash } = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

if (process.argv.length !== 4) {
  throw new Error("usage: test_plan_act_cli.js NATIVE_ADDON REPOSITORY");
}

const addon = path.resolve(process.argv[2]);
const repository = path.resolve(process.argv[3]);
const root = fs.mkdtempSync(path.join(repository, "build/node-plan-act-"));
const artifacts = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-artifacts-"),
);
const surfaceRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-surface-"),
);
const registrationRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-registration-"),
);
const coordinatedRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-coordinated-"),
);
const observedRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-observed-"),
);
const redirectRoot = fs.mkdtempSync(
  path.join(repository, "build/node-plan-act-redirect-"),
);
const cli = path.join(repository, "js/src/cli.js");

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

try {
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
    "Result: applied-transitions=1\n",
  );
  assert.equal(fs.existsSync(legacy), false);
  const replay = run(["apply", act, "--root", root], 2);
  assert.match(replay.stderr.toString("utf8"), /legacy\.js/);

  fs.writeFileSync(
    path.join(root, "api.js"),
    "export function oldApi(value) {\n  return value + 1;\n}\n",
  );
  fs.writeFileSync(
    path.join(root, "consumer.js"),
    "import { oldApi } from \"./api.js\";\nexport const result = oldApi(1);\n",
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
    [...new Set(renameDocument.items[0].operation.sites.map(
      (site) => site.path,
    ))].sort(),
    ["api.js", "consumer.js"],
  );
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
  run(["verify", "--root", root, "--check"]);

  fs.cpSync(
    path.join(repository, "test/fixtures/plan_act/surface_closure"),
    surfaceRoot,
    { recursive: true },
  );
  const surfacePlan = path.join(artifacts, "surface-plan.json");
  const surfaceAct = path.join(artifacts, "surface-act.json");
  const surfaceMakefile = fs.readFileSync(path.join(surfaceRoot, "Makefile"));
  run([
    "plan", "FFI-SURFACE", "--root", surfaceRoot,
    "--rename", "core_add=core_sum", "--output", surfacePlan,
  ]);
  const surfaceDocument = JSON.parse(fs.readFileSync(surfacePlan, "utf8"));
  assert.equal(surfaceDocument.items.length, 1);
  assert.equal(surfaceDocument.items[0].executable, true);
  assert.deepEqual(surfaceDocument.items[0].operation, {
    action: "edit_make_variable_token",
    expected_token: "_core_add",
    path: "Makefile",
    replacement_token: "_core_sum",
    source_sha256: createHash("sha256").update(surfaceMakefile).digest("hex"),
    variable: "WASM_EXPORTS",
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
  assert.equal(removalDocument.items[0].operation.replacement_token, "");
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
  const registrationPlan = path.join(artifacts, "registration-plan.json");
  const registrationAct = path.join(artifacts, "registration-act.json");
  const registrationMakefile = fs.readFileSync(
    path.join(registrationRoot, "Makefile"),
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
  assert.equal(registrationDocument.items[0].origins.length, 2);
  assert.deepEqual(registrationDocument.items[0].operation, {
    action: "insert_make_variable_token",
    anchor_token: "_core_peer",
    path: "Makefile",
    position: "after",
    source_sha256: createHash("sha256")
      .update(registrationMakefile)
      .digest("hex"),
    token: "_core_sum",
    variable: "WASM_EXPORTS",
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
    ["insert_c_declaration", "insert_make_variable_token"],
  );
  run([
    "act", coordinatedPlan, "--root", coordinatedRoot, "--format", "json",
    "--output", coordinatedAct,
  ]);
  run(["apply", coordinatedAct, "--root", coordinatedRoot]);
  run(["verify", "--root", coordinatedRoot, "--check"]);

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
  assert.equal(observedDocument.schema_version, 2);
  assert.equal(observedDocument.items.length, 1);
  assert.equal(observedDocument.items[0].provenance, "derived");
  assert.equal(observedDocument.items[0].executable, true);
  assert.equal(
    observedDocument.items[0].operation.replacement_token,
    "_core_sum",
  );
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

  process.stdout.write("node Plan/Act CLI lifecycle passed\n");
} finally {
  fs.rmSync(root, { force: true, recursive: true });
  fs.rmSync(artifacts, { force: true, recursive: true });
  fs.rmSync(surfaceRoot, { force: true, recursive: true });
  fs.rmSync(registrationRoot, { force: true, recursive: true });
  fs.rmSync(coordinatedRoot, { force: true, recursive: true });
  fs.rmSync(observedRoot, { force: true, recursive: true });
  fs.rmSync(redirectRoot, { force: true, recursive: true });
}
