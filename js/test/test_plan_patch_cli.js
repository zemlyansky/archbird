"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

if (process.argv.length !== 4) {
  throw new Error("usage: test_plan_patch_cli.js NATIVE_ADDON REPOSITORY");
}

const addon = path.resolve(process.argv[2]);
const repository = path.resolve(process.argv[3]);
const root = fs.mkdtempSync(path.join(repository, "build/node-plan-patch-"));
const artifacts = fs.mkdtempSync(
  path.join(repository, "build/node-plan-patch-artifacts-"),
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

try {
  fs.writeFileSync(
    path.join(root, "archbird.json"),
    JSON.stringify({
      project: "node-plan-patch",
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
  const patch = path.join(artifacts, "patch.json");

  run(["plan", "NO-LEGACY", "--root", root, "--output", plan]);
  const preview = run([
    "act", plan, "--root", root, "--format", "patch",
  ]).stdout.toString("utf8");
  assert.match(preview, /--- a\/legacy\.js/);
  assert.match(preview, /\+\+\+ \/dev\/null/);
  assert.equal(fs.existsSync(legacy), true, "Act mutated the repository");

  run([
    "act", plan, "--root", root, "--format", "json", "--output", patch,
  ]);
  const document = JSON.parse(fs.readFileSync(patch, "utf8"));
  assert.equal(document.artifact, "patch");
  assert.equal(document.state, "accepted");
  assert.equal(document.acceptance.status, "satisfied");
  assert.deepEqual(document.acceptance.constraints, [
    { id: "NO-LEGACY", status: "pass" },
  ]);
  assert.equal(fs.existsSync(legacy), true, "Patch creation mutated the repository");

  assert.equal(
    run(["apply", patch, "--root", root]).stdout.toString("utf8"),
    "Result: applied-transitions=1\n",
  );
  assert.equal(fs.existsSync(legacy), false);
  const replay = run(["apply", patch, "--root", root], 2);
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
      project: "node-plan-patch-rename",
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
  const renamePatch = path.join(artifacts, "rename-patch.json");
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
    "--output", renamePatch,
  ]);
  const acceptedRename = JSON.parse(fs.readFileSync(renamePatch, "utf8"));
  assert.equal(acceptedRename.state, "accepted");
  assert.equal(acceptedRename.transitions.length, 2);
  run(["apply", renamePatch, "--root", root]);
  assert.doesNotMatch(fs.readFileSync(path.join(root, "api.js"), "utf8"), /oldApi/);
  assert.doesNotMatch(
    fs.readFileSync(path.join(root, "consumer.js"), "utf8"),
    /oldApi/,
  );
  run(["verify", "--root", root, "--check"]);

  process.stdout.write("node Plan/Patch CLI lifecycle passed\n");
} finally {
  fs.rmSync(root, { force: true, recursive: true });
  fs.rmSync(artifacts, { force: true, recursive: true });
}
