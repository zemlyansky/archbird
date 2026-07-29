"use strict";

const assert = require("node:assert/strict");
const { spawnSync } = require("node:child_process");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");

if (process.argv[2]) {
  process.env.ARCHBIRD_ENGINE = "native";
  process.env.ARCHBIRD_NATIVE_ADDON = path.resolve(process.argv[2]);
}

const repositoryRoot = process.argv[3]
  ? path.resolve(process.argv[3])
  : path.resolve(__dirname, "../..");
const { previewPlan } = require("../src/acting");
const {
  MAX_COLLECTION_ITEMS,
  MAX_FILE_BYTES,
  MAX_METADATA_BYTES,
  MAX_OPERATION_TEXT_BYTES,
  MAX_PLAN_BYTES,
  MAX_SAFE_INTEGER,
} = require("../src/plan-limits");

function sha(character) {
  return character.repeat(64);
}

function item(operation) {
  return {
    id: "edit",
    statement: "edit",
    provenance: "derived",
    origins: [{
      constraint_id: "TEST",
      constraint_result_sha256: sha("1"),
      issue_fingerprint: sha("2"),
    }],
    evidence: [{
      provenance: "derived",
      project: "fixture",
      path: "source.txt",
      line: 1,
      sha256: sha("3"),
      detail: "source",
    }],
    depends_on: [],
    operation,
    acceptance: { constraints: ["TEST"] },
    unknowns: [],
    executable: true,
    non_executable_reasons: [],
  };
}

function plan(operation) {
  return {
    schema_version: 1,
    artifact: "plan",
    provenance: "derived",
    tool: {
      name: "archbird",
      version: "test",
      implementation_sha256: sha("a"),
    },
    source: {
      project: "fixture",
      map: {
        sha256: sha("b"),
        input_sha256: sha("c"),
        configuration_sha256: sha("d"),
        producer_implementation_sha256: sha("e"),
      },
      verification: {
        sha256: sha("f"),
        policy_sha256: sha("4"),
        producer_implementation_sha256: sha("5"),
      },
    },
    objective: "exercise resource bounds",
    items: [item(operation)],
    preserved_constraints: [],
    unknowns: [],
  };
}

function assertInvalidPlan(document) {
  const root = fs.mkdtempSync(path.join(repositoryRoot, "build/plan-limit-"));
  try {
    const result = previewPlan(document, root);
    assert.equal(result.status, "blocked", JSON.stringify(result));
    assert.equal(result.changes.length, 0);
    assert.equal(result.diagnostics[0]?.code, "invalid_plan", JSON.stringify(result));
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
}

function run(name, callback) {
  try {
    callback();
    process.stdout.write(`ok - ${name}\n`);
  } catch (error) {
    error.message = `${name}: ${error.message}`;
    throw error;
  }
}

run("Plan coordinates stop at the JavaScript safe integer", () => {
  const document = plan({
    action: "replace_range",
    path: "source.txt",
    source_sha256: sha("3"),
    start_byte: MAX_SAFE_INTEGER,
    end_byte: MAX_SAFE_INTEGER,
    before: "",
    replacement: "",
  });
  document.items[0].evidence[0].line = MAX_SAFE_INTEGER;
  const root = fs.mkdtempSync(path.join(repositoryRoot, "build/plan-limit-"));
  try {
    const portable = previewPlan(document, root);
    assert.equal(portable.status, "blocked");
    assert.notEqual(portable.diagnostics[0]?.code, "invalid_plan");
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }

  for (const field of ["start_byte", "end_byte"]) {
    const invalid = structuredClone(document);
    invalid.items[0].operation[field] = MAX_SAFE_INTEGER + 1;
    assertInvalidPlan(invalid);
  }
  document.items[0].evidence[0].line = MAX_SAFE_INTEGER + 1;
  assertInvalidPlan(document);
});

run("Plan metadata, operation text, and arrays are bounded", () => {
  const metadata = plan({
    action: "create_file",
    path: "created.txt",
    content: "",
  });
  metadata.items[0].statement = "s".repeat(MAX_METADATA_BYTES + 1);
  assertInvalidPlan(metadata);

  const content = plan({
    action: "create_file",
    path: "created.txt",
    content: "c".repeat(MAX_OPERATION_TEXT_BYTES + 1),
  });
  assertInvalidPlan(content);

  const candidates = plan({
    action: "manual",
    instructions: "review",
    candidate_paths: Array.from(
      { length: MAX_COLLECTION_ITEMS + 1 },
      (_, index) => `path-${index}`,
    ),
  });
  candidates.items[0].executable = false;
  candidates.items[0].non_executable_reasons = ["review"];
  assertInvalidPlan(candidates);
});

run("oversized sparse source is rejected before reading", () => {
  const root = fs.mkdtempSync(path.join(repositoryRoot, "build/plan-limit-"));
  try {
    fs.writeFileSync(path.join(root, "source.txt"), "");
    fs.truncateSync(path.join(root, "source.txt"), MAX_FILE_BYTES + 1);
    const document = plan({
      action: "delete_file",
      path: "source.txt",
      source_sha256: crypto.createHash("sha256").update("").digest("hex"),
    });
    const result = previewPlan(document, root);
    assert.equal(result.status, "blocked", JSON.stringify(result));
    assert.equal(result.changes.length, 0);
    assert.ok(
      result.diagnostics.some((row) => row.message.includes("67108864")),
      JSON.stringify(result),
    );
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

run("move plans count both source and destination against touched files", () => {
  const root = fs.mkdtempSync(path.join(repositoryRoot, "build/plan-limit-"));
  try {
    const emptySha = crypto.createHash("sha256").update("").digest("hex");
    const document = plan({
      action: "move_file",
      source_path: "source-0",
      destination_path: "destination-0",
      source_sha256: emptySha,
    });
    document.items = Array.from(
      { length: Math.floor(MAX_COLLECTION_ITEMS / 2) + 1 },
      (_, index) => {
        fs.writeFileSync(path.join(root, `source-${index}`), "");
        const row = item({
          action: "move_file",
          source_path: `source-${index}`,
          destination_path: `destination-${index}`,
          source_sha256: emptySha,
        });
        row.id = `move-${index}`;
        return row;
      },
    );
    const result = previewPlan(document, root);
    assert.equal(result.status, "blocked", JSON.stringify(result));
    assert.equal(result.changes.length, 0);
    assert.ok(
      result.diagnostics.some((row) =>
        row.message.includes("more than 4096 files")
      ),
      JSON.stringify(result),
    );
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

run("Act CLI rejects an oversized Plan before reading or parsing it", () => {
  const root = fs.mkdtempSync(path.join(repositoryRoot, "build/plan-limit-"));
  try {
    const planPath = path.join(root, "oversized-plan.json");
    fs.writeFileSync(planPath, "");
    fs.truncateSync(planPath, MAX_PLAN_BYTES + 1);
    const completed = spawnSync(
      process.execPath,
      [path.join(repositoryRoot, "js/src/cli.js"), "act", planPath],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          ARCHBIRD_ENGINE: "native",
          ARCHBIRD_NATIVE_ADDON: path.resolve(process.argv[2]),
        },
      },
    );
    assert.notEqual(completed.status, 0);
    assert.match(completed.stderr, /Plan JSON exceeds 67108864 bytes/);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

run("Act CLI rejects a symlink Plan locator before opening it", () => {
  const root = fs.mkdtempSync(path.join(repositoryRoot, "build/plan-limit-"));
  try {
    fs.writeFileSync(path.join(root, "plan.json"), "{}");
    fs.symlinkSync("plan.json", path.join(root, "plan-link.json"));
    const completed = spawnSync(
      process.execPath,
      [
        path.join(repositoryRoot, "js/src/cli.js"),
        "act",
        path.join(root, "plan-link.json"),
      ],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          ARCHBIRD_ENGINE: "native",
          ARCHBIRD_NATIVE_ADDON: path.resolve(process.argv[2]),
        },
      },
    );
    assert.notEqual(completed.status, 0);
    assert.match(completed.stderr, /Plan JSON is not a regular file/);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});
