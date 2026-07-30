"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

if (process.argv[2]) {
  process.env.ARCHBIRD_ENGINE = "native";
  process.env.ARCHBIRD_NATIVE_ADDON = path.resolve(process.argv[2]);
}

const repositoryRoot = process.argv[3]
  ? path.resolve(process.argv[3])
  : path.resolve(__dirname, "../..");
const native = require("../src/native");
const { Project } = require("../src/index");
const { generatePlan } = require("../src/planning");
const { applyPlan, planSha256, previewPlan } = require("../src/acting");

const BUILD = path.join(repositoryRoot, "build");
fs.mkdirSync(BUILD, { recursive: true });

function sha(value) {
  return crypto.createHash("sha256").update(value).digest("hex");
}

function canonicalDigest(value) {
  return sha(native.jsonCanonicalize(
    Buffer.from(JSON.stringify(value), "utf8"),
  ));
}

function operationItem(identifier, operation, options = {}) {
  const executable = options.executable ?? true;
  return {
    id: identifier,
    statement: options.statement || identifier,
    provenance: options.provenance || "derived",
    origins: [{
      constraint_id: options.constraintId || "TEST-CONSTRAINT",
      constraint_result_sha256: "1".repeat(64),
      issue_fingerprint: "2".repeat(64),
    }],
    evidence: [],
    depends_on: options.dependsOn || [],
    operation,
    acceptance: {
      constraints: [options.constraintId || "TEST-CONSTRAINT"],
    },
    unknowns: options.unknowns || [],
    executable,
    non_executable_reasons: executable
      ? []
      : options.reasons || ["input required"],
  };
}

function plan(...items) {
  return {
    schema_version: 1,
    artifact: "plan",
    provenance: "derived",
    tool: {
      name: "archbird",
      version: "test",
      implementation_sha256: "3".repeat(64),
    },
    source: {
      project: "test",
      map: {
        sha256: "4".repeat(64),
        input_sha256: "5".repeat(64),
        configuration_sha256: "6".repeat(64),
        producer_implementation_sha256: "7".repeat(64),
      },
      verification: {
        sha256: "8".repeat(64),
        policy_sha256: "9".repeat(64),
        producer_implementation_sha256: "a".repeat(64),
      },
    },
    objective: "exercise Act",
    items,
    preserved_constraints: [],
    unknowns: [],
  };
}

function satisfied(constraintIds = ["TEST-CONSTRAINT"]) {
  return {
    status: "satisfied",
    verification_sha256: "b".repeat(64),
    constraints: constraintIds.map((id) => ({ id, status: "pass" })),
  };
}

function temporary(name) {
  return fs.mkdtempSync(path.join(BUILD, `${name}-`));
}

function write(root, relative, value) {
  const target = path.join(root, ...relative.split("/"));
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, value);
  return sha(value);
}

function withTemporary(name, callback) {
  const root = temporary(name);
  try {
    callback(root);
  } finally {
    fs.rmSync(root, { force: true, recursive: true });
  }
}

function cli(arguments_, expected = 0) {
  const completed = spawnSync(
    process.execPath,
    [path.join(repositoryRoot, "js/src/cli.js"), ...arguments_],
    {
      encoding: null,
      env: {
        ...process.env,
        ARCHBIRD_ENGINE: "native",
        ARCHBIRD_NATIVE_ADDON: path.resolve(process.argv[2]),
      },
    },
  );
  assert.equal(
    completed.status,
    expected,
    `${arguments_.join(" ")} exited ${completed.status}\n` +
    `stdout:\n${Buffer.from(completed.stdout || []).toString("utf8")}\n` +
    `stderr:\n${Buffer.from(completed.stderr || []).toString("utf8")}`,
  );
  return completed;
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

run("canonical Plan digest ignores object insertion order", () => {
  const document = plan();
  const reordered = Object.fromEntries(Object.entries(document).reverse());
  assert.equal(planSha256(document), planSha256(reordered));
  assert.equal(
    planSha256(document),
    "92ba81f518d269e861af2aabd06e195f3c4f56b84766e7ba8d9ab1a9daf7d1d6",
  );
});

run("preview uses exact UTF-8 byte offsets and does not mutate", () => {
  withTemporary("node-act-unicode", (root) => {
    const source = Buffer.from("const café = 'old';\nreturn café;\n");
    const sourceSha256 = write(root, "src/main.js", source);
    const name = source.indexOf(Buffer.from("café"));
    const old = source.indexOf(Buffer.from("old"));
    const document = plan(
      operationItem("rename", {
        action: "replace_range",
        path: "src/main.js",
        source_sha256: sourceSha256,
        start_byte: name,
        end_byte: name + Buffer.byteLength("café"),
        before: "café",
        replacement: "coffee",
      }),
      operationItem("value", {
        action: "replace_range",
        path: "src/main.js",
        source_sha256: sourceSha256,
        start_byte: old,
        end_byte: old + 3,
        before: "old",
        replacement: "new",
      }),
    );
    const result = previewPlan(document, root);
    assert.equal(result.status, "preview");
    assert.equal(result.changes.length, 1);
    assert.equal(result.changes[0].kind, "modify");
    assert.match(result.changes[0].unified_diff, /\+const coffee = 'new';/);
    assert.deepEqual(fs.readFileSync(path.join(root, "src/main.js")), source);

    const splitCodePoint = structuredClone(document);
    splitCodePoint.items[0].operation.start_byte = name + 4;
    splitCodePoint.items[0].operation.before = "";
    const blocked = previewPlan(splitCodePoint, root);
    assert.equal(blocked.status, "blocked");
    assert.match(
      blocked.diagnostics[0].message,
      /offsets must bound valid UTF-8 text/,
    );
  });
});

run("preview and apply cover create delete move and modify", () => {
  withTemporary("node-act-mixed", (root) => {
    const edited = Buffer.from("alpha = 1\n");
    const deleted = Buffer.from("obsolete\n");
    const moved = Buffer.from("move me\n");
    const editedSha = write(root, "edited.py", edited);
    const deletedSha = write(root, "deleted.txt", deleted);
    const movedSha = write(root, "old/name.txt", moved);
    const document = plan(
      operationItem("edit", {
        action: "replace_range",
        path: "edited.py",
        source_sha256: editedSha,
        start_byte: 8,
        end_byte: 9,
        before: "1",
        replacement: "2",
      }),
      operationItem("create", {
        action: "create_file",
        path: "new/file.txt",
        content: "new\n",
      }),
      operationItem("delete", {
        action: "delete_file",
        path: "deleted.txt",
        source_sha256: deletedSha,
      }),
      operationItem("move", {
        action: "move_file",
        source_path: "old/name.txt",
        destination_path: "new/name.txt",
        source_sha256: movedSha,
      }),
    );
    const preview = previewPlan(document, root);
    assert.equal(preview.status, "preview");
    assert.deepEqual(
      new Set(preview.changes.map((row) => row.kind)),
      new Set(["modify", "create", "delete", "move"]),
    );
    for (const change of preview.changes) {
      assert.match(change.unified_diff, /--- [^\n]+\n\+\+\+ [^\n]+/);
    }
    assert.equal(fs.readFileSync(path.join(root, "edited.py"), "utf8"), "alpha = 1\n");

    const applied = applyPlan(document, root, () => satisfied());
    assert.equal(applied.status, "applied");
    assert.equal(applied.plan_sha256, preview.plan_sha256);
    assert.equal(applied.acceptance.status, "satisfied");
    assert.equal(fs.readFileSync(path.join(root, "edited.py"), "utf8"), "alpha = 2\n");
    assert.equal(fs.readFileSync(path.join(root, "new/file.txt"), "utf8"), "new\n");
    assert.deepEqual(fs.readFileSync(path.join(root, "new/name.txt")), moved);
    assert.equal(fs.existsSync(path.join(root, "deleted.txt")), false);
    assert.equal(fs.existsSync(path.join(root, "old/name.txt")), false);
  });
});

run("JSON Pointer edits preserve layout and require asserted intent", () => {
  withTemporary("node-act-json-pointer", (root) => {
    const packageSource = Buffer.from(
      '{\n  "name": "demo",\n  "exports": {\n' +
      '    ".": "./old.js"\n  }\n}\n',
    );
    const buildSource = Buffer.from(
      '{"scripts":{"test":"node test.js"},"name":"demo"}\n',
    );
    const packageSha = write(root, "package.json", packageSource);
    const buildSha = write(root, "build.json", buildSource);
    const document = plan(
      operationItem("replace-export", {
        action: "edit_json_pointer",
        path: "package.json",
        source_sha256: packageSha,
        pointer: "/exports/.",
        expected_absent: false,
        expected: "./old.js",
        replacement: "./dist/index.js",
      }, { provenance: "asserted" }),
      operationItem("insert-build", {
        action: "edit_json_pointer",
        path: "build.json",
        source_sha256: buildSha,
        pointer: "/scripts/build",
        expected_absent: true,
        replacement: "node build.js",
      }, { provenance: "asserted" }),
    );
    const preview = previewPlan(document, root);
    assert.equal(preview.status, "preview");
    assert.ok(
      preview.changes.find((row) => row.path === "package.json").unified_diff
        .includes('+    ".": "./dist/index.js"'),
    );
    const applied = applyPlan(document, root, () => satisfied());
    assert.equal(applied.status, "applied");
    assert.equal(
      JSON.parse(fs.readFileSync(path.join(root, "package.json"))).exports["."],
      "./dist/index.js",
    );
    assert.equal(
      JSON.parse(fs.readFileSync(path.join(root, "build.json"))).scripts.build,
      "node build.js",
    );
    assert.ok(
      fs.readFileSync(path.join(root, "package.json"), "utf8")
        .includes('\n  "name": "demo",\n  "exports": {\n'),
    );

    const changed = fs.readFileSync(path.join(root, "package.json"));
    const unreviewed = plan(operationItem("unreviewed", {
      action: "edit_json_pointer",
      path: "package.json",
      source_sha256: sha(changed),
      pointer: "/exports/.",
      expected_absent: false,
      expected: "./dist/index.js",
      replacement: "./other.js",
    }));
    const blocked = previewPlan(unreviewed, root);
    assert.equal(blocked.status, "blocked");
    assert.match(
      blocked.diagnostics[0].message,
      /requires asserted edit_json_pointer intent/,
    );
  });
});

run("invalid and overlapping operations block every write", () => {
  withTemporary("node-act-blocked", (root) => {
    const source = Buffer.from("abcdef\n");
    const sourceSha = write(root, "source.txt", source);
    const base = {
      action: "replace_range",
      path: "source.txt",
      source_sha256: sourceSha,
    };
    const overlap = previewPlan(plan(
      operationItem("left", {
        ...base,
        start_byte: 1,
        end_byte: 4,
        before: "bcd",
        replacement: "B",
      }),
      operationItem("right", {
        ...base,
        start_byte: 3,
        end_byte: 5,
        before: "de",
        replacement: "D",
      }),
    ), root);
    assert.equal(overlap.status, "blocked");
    assert(
      overlap.diagnostics.some((row) => row.code === "overlapping_edits"),
    );
    assert.deepEqual(fs.readFileSync(path.join(root, "source.txt")), source);

    const stale = previewPlan(plan(operationItem("stale", {
      action: "delete_file",
      path: "source.txt",
      source_sha256: "0".repeat(64),
    })), root);
    assert.equal(stale.status, "blocked");
    assert.match(stale.diagnostics[0].message, /source SHA-256 is stale/);

    const wrongBefore = previewPlan(plan(operationItem("before", {
      ...base,
      start_byte: 0,
      end_byte: 5,
      before: "other",
      replacement: "new",
    })), root);
    assert.equal(wrongBefore.status, "blocked");
    assert.match(wrongBefore.diagnostics[0].message, /does not match source bytes/);
  });
});

run("path escapes, symlinks, and non-regular sources are blocked", () => {
  withTemporary("node-act-path", (root) => {
    const source = Buffer.from("value\n");
    const sourceSha = write(root, "real/source.txt", source);
    fs.symlinkSync("real/source.txt", path.join(root, "link.txt"));
    fs.symlinkSync("real", path.join(root, "linked-dir"));
    fs.mkdirSync(path.join(root, "directory"));
    const cases = [
      ["escape", "../outside"],
      ["absolute", path.join(root, "real/source.txt")],
      ["windows", "C:/outside"],
      ["backslash", "real\\source.txt"],
      ["empty-segment", "real//source.txt"],
      ["symlink", "link.txt"],
      ["symlink-parent", "linked-dir/source.txt"],
      ["directory", "directory"],
    ];
    for (const [identifier, filePath] of cases) {
      const blocked = previewPlan(plan(operationItem(identifier, {
        action: "delete_file",
        path: filePath,
        source_sha256: sourceSha,
      })), root);
      assert.equal(blocked.status, "blocked", identifier);
      assert.equal(fs.existsSync(path.join(root, "real/source.txt")), true);
    }
    const linkedRoot = path.join(root, "root-link");
    fs.symlinkSync(".", linkedRoot);
    const blockedRoot = previewPlan(plan(operationItem("root", {
      action: "delete_file",
      path: "real/source.txt",
      source_sha256: sourceSha,
    })), linkedRoot);
    assert.equal(blockedRoot.status, "blocked");
    assert.match(blockedRoot.diagnostics[0].message, /root must not be a symlink/);
  });
});

run("manual items, cycles, duplicate values, and dangling IDs are rejected", () => {
  withTemporary("node-act-plan-shape", (root) => {
    const manual = operationItem("manual", {
      action: "manual",
      instructions: "Choose reviewed code.",
      candidate_paths: [],
    }, {
      executable: false,
      reasons: ["Verification does not define code."],
      unknowns: ["unknown-1"],
    });
    const manualPlan = plan(manual);
    manualPlan.unknowns = [{
      id: "unknown-1",
      statement: "Code is absent.",
      item_id: "manual",
      constraint_id: "TEST-CONSTRAINT",
    }];
    const blocked = previewPlan(manualPlan, root);
    assert.equal(blocked.status, "blocked");
    assert.equal(blocked.diagnostics[0].code, "non_executable");

    const cyclic = plan(
      operationItem("one", {
        action: "create_file",
        path: "one",
        content: "",
      }, { dependsOn: ["two"] }),
      operationItem("two", {
        action: "create_file",
        path: "two",
        content: "",
      }, { dependsOn: ["one"] }),
    );
    assert.match(
      previewPlan(cyclic, root).diagnostics[0].message,
      /dependency graph contains a cycle/,
    );

    const duplicateEvidence = plan(operationItem("duplicate", {
      action: "create_file",
      path: "new",
      content: "",
    }));
    const row = {
      provenance: "derived",
      project: "test",
      path: "",
      line: 0,
      sha256: "",
      detail: "same",
    };
    duplicateEvidence.items[0].evidence = [row, structuredClone(row)];
    assert.match(
      previewPlan(duplicateEvidence, root).diagnostics[0].message,
      /evidence must contain unique values/,
    );

    const dangling = plan(operationItem("dangling", {
      action: "create_file",
      path: "new",
      content: "",
    }, { dependsOn: ["absent"] }));
    assert.match(
      previewPlan(dangling, root).diagnostics[0].message,
      /dangling dependency/,
    );
  });
});

run("apply rolls back filesystem writes when commit fails", () => {
  withTemporary("node-act-rollback", (root) => {
    const source = Buffer.from("old\n");
    const sourceSha = write(root, "a.txt", source);
    const document = plan(
      operationItem("edit", {
        action: "replace_range",
        path: "a.txt",
        source_sha256: sourceSha,
        start_byte: 0,
        end_byte: 3,
        before: "old",
        replacement: "new",
      }),
      operationItem("create", {
        action: "create_file",
        path: "z.txt",
        content: "created\n",
      }),
    );
    const originalRename = fs.renameSync;
    let stagedNew = 0;
    fs.renameSync = function injectedRename(sourcePath, destinationPath) {
      if (
        sourcePath.includes(".archbird-act-") &&
        path.basename(sourcePath).startsWith("new-")
      ) {
        stagedNew += 1;
        if (stagedNew === 2) {
          const error = new Error("injected commit failure");
          error.code = "EIO";
          throw error;
        }
      }
      return originalRename(sourcePath, destinationPath);
    };
    let applied;
    try {
      applied = applyPlan(document, root, () => satisfied());
    } finally {
      fs.renameSync = originalRename;
    }
    assert.equal(applied.status, "failed");
    assert.match(applied.diagnostics[0].message, /injected commit failure/);
    assert.deepEqual(fs.readFileSync(path.join(root, "a.txt")), source);
    assert.equal(fs.existsSync(path.join(root, "z.txt")), false);
  });
});

run("acceptance must exactly cover changed and preserved constraints", () => {
  withTemporary("node-act-acceptance", (root) => {
    const document = plan(operationItem("create", {
      action: "create_file",
      path: "new.txt",
      content: "new\n",
    }));
    document.preserved_constraints = ["PRESERVED"];
    const applied = applyPlan(document, root, () => ({
      status: "satisfied",
      verification_sha256: "b".repeat(64),
      constraints: [],
    }));
    assert.equal(applied.status, "failed");
    assert.match(
      applied.diagnostics[0].message,
      /do not exactly cover the Plan/,
    );
    assert.equal(fs.existsSync(path.join(root, "new.txt")), false);
  });
});

run("rejected acceptance is evaluated and rolled back", () => {
  for (const [acceptanceStatus, constraintStatus] of [
    ["not_satisfied", "fail"],
    ["unknown", "unknown"],
  ]) {
    withTemporary(`node-act-${acceptanceStatus}`, (root) => {
      const document = plan(operationItem("create", {
        action: "create_file",
        path: "new.txt",
        content: "new\n",
      }));
      const result = applyPlan(document, root, () => ({
        status: acceptanceStatus,
        verification_sha256: "b".repeat(64),
        constraints: [{
          id: "TEST-CONSTRAINT",
          status: constraintStatus,
        }],
      }));
      assert.equal(result.status, "rejected");
      assert.equal(result.acceptance.status, acceptanceStatus);
      assert.equal(fs.existsSync(path.join(root, "new.txt")), false);
    });
  }
});

run("acceptance cannot mutate the consumed Plan", () => {
  withTemporary("node-act-mutated-plan", (root) => {
    const document = plan(operationItem("create", {
      action: "create_file",
      path: "new.txt",
      content: "new\n",
    }));
    const result = applyPlan(document, root, (callbackPlan) => {
      callbackPlan.items[0].acceptance.constraints = ["OTHER"];
      return {
        status: "satisfied",
        verification_sha256: "b".repeat(64),
        constraints: [{ id: "OTHER", status: "pass" }],
      };
    });
    assert.equal(result.status, "failed");
    assert.equal(fs.existsSync(path.join(root, "new.txt")), false);
    assert.deepEqual(
      document.items[0].acceptance.constraints,
      ["TEST-CONSTRAINT"],
    );
  });
});

run("an empty Plan applies only after explicit empty acceptance", () => {
  withTemporary("node-act-noop", (root) => {
    let calls = 0;
    const applied = applyPlan(plan(), root, () => {
      calls += 1;
      return satisfied([]);
    });
    assert.equal(applied.status, "applied");
    assert.deepEqual(applied.changes, []);
    assert.equal(applied.acceptance.status, "satisfied");
    assert.deepEqual(applied.acceptance.constraints, []);
    assert.equal(calls, 1);
  });
});

function mapArtifact(files, collections = {}) {
  return {
    artifact: "map",
    schema_version: 9,
    project: "node-planning-test",
    description: "Node Plan generation fixture.",
    files,
    edges: [],
    symbol_calls: [],
    symbol_references: [],
    call_resolutions: [],
    packages: [],
    surfaces: [],
    evidence: {
      absolute_paths_included: false,
      input_sha256: sha("input"),
      config_sha256: sha("configuration"),
    },
    tool: {
      name: "archbird",
      version: "0.0.1",
      implementation_sha256: sha("producer"),
    },
    ...collections,
  };
}

function finding(key, filePath, sourceSha, comparison = "extra") {
  return {
    fingerprint: sha(`finding:${key}:${filePath}`),
    comparison,
    evidence_state: "current",
    applicability: "applicable",
    disposition: "open",
    key,
    message: `${comparison} actual fact ${key}`,
    evidence: [{
      provenance: filePath ? "derived" : "asserted",
      project: filePath ? "node-planning-test" : "",
      path: filePath,
      line: filePath ? 1 : 0,
      sha256: sourceSha || sha(`evidence:${key}`),
      detail: `evidence for ${key}`,
    }],
  };
}

function forbiddenPathOperand(name, paths) {
  const operandSha256 = sha(`operand:${name}:${paths.join(",")}`);
  const operand = {
    completeness: {
      classification: "complete",
      counts: {
        evaluated: paths.length,
        excluded: 0,
        selected: paths.length,
        universe: paths.length,
        unknown: 0,
        unsupported: 0,
      },
      exhaustive: true,
      truncated: false,
      unit: "repository_file",
    },
    items: paths.map((filePath) => ({
      attributes: {},
      evidence: [{
        provenance: "derived",
        project: "node-planning-test",
        path: filePath,
        line: 0,
        sha256: operandSha256,
        detail: `repository inventory path ${filePath}`,
      }],
      key: filePath,
      label: filePath,
      message: "",
      state: "current",
      value: null,
    })),
    message: "",
    name,
    project: "node-planning-test",
    provenance: "derived",
    sha256: operandSha256,
    shape: "set",
    state: "current",
  };
  const issue = finding("cardinality", "", operandSha256, "different");
  issue.message = `fact cardinality is ${paths.length}; expected exactly 0`;
  Object.assign(issue.evidence[0], {
    provenance: "derived",
    project: "node-planning-test",
    detail: `fact ${name} shape=set items=${paths.length}`,
  });
  return [operand, issue];
}

function check(identifier, assertion, actual, findings, exact = null) {
  return {
    id: identifier,
    assert: assertion,
    status: "fail",
    owner: "architecture",
    rationale: "Exercise deterministic Plan derivation.",
    severity: "error",
    coverage: [],
    findings,
    operands: {
      actual,
      expected: "",
      mapping: "",
      exact,
      min: null,
      max: null,
      reference_route: "",
      required_routes: [],
    },
    requirements: [],
    tags: [],
    witnesses: [],
  };
}

function verificationArtifact(mapDocument, checks, definitions, operands = []) {
  const policyRows = checks.map((row) => ({
    id: row.id,
    constraint_definition_sha256: sha(`definition:${row.id}`),
    constraint_plan_sha256: sha(`plan:${row.id}`),
    constraint_result_sha256: sha(`result:${row.id}`),
  }));
  const document = {
    artifact: "verification",
    schema_version: 2,
    constraints: checks,
    diagnostics: [],
    evaluations: [{
      id: "current",
      project: mapDocument.project,
      map_input_sha256: mapDocument.evidence.input_sha256,
      map_config_sha256: mapDocument.evidence.config_sha256,
      map_producer_implementation_sha256:
        mapDocument.tool.implementation_sha256,
      resolution_sha256: null,
    }],
    mappings: {},
    observations: [],
    operand_definitions: definitions,
    operands,
    policy: {
      kind: "all",
      project: mapDocument.project,
      configured_count: checks.length,
      evaluated_count: checks.length,
      omitted_count: 0,
      requested_ids: [],
      constraint_policy_sha256: sha("policy"),
      project_configuration_sha256: mapDocument.evidence.config_sha256,
      constraints: policyRows,
    },
    summary: { blocking: true },
    tool: {
      name: "archbird",
      version: "0.0.1",
      implementation_sha256: sha("producer"),
    },
  };
  document.verification_result_sha256 = canonicalDigest(document);
  return document;
}

run("noncanonical Map fixtures cannot authorize destructive Plan items", () => {
  withTemporary("node-plan-delete", (root) => {
    const legacySha = write(root, "legacy.txt", Buffer.from("obsolete\n"));
    const map = mapArtifact([{
      path: "legacy.txt",
      sha256: legacySha,
      symbols: [],
      exports: [],
    }]);
    const [operand, issue] = forbiddenPathOperand(
      "p.paths",
      ["legacy.txt"],
    );
    const forbidden = check(
      "NO-LEGACY",
      "cardinality",
      "p.paths",
      [issue],
      0,
    );
    const preserved = {
      ...check("KEEP-API", "cardinality", "p.api", []),
      status: "pass",
    };
    const verification = verificationArtifact(
      map,
      [forbidden, preserved],
      {
        "p.paths": {
          select: "inventory_paths",
          include: ["legacy.txt"],
        },
        "p.api": { select: "provider_surface" },
      },
      [operand],
    );
    const generated = generatePlan(
      map,
      verification,
      ["NO-LEGACY"],
      root,
    );
    assert.deepEqual(generated.preserved_constraints, ["KEEP-API"]);
    assert.equal(generated.items.length, 1);
    assert.equal(generated.items[0].executable, false);
    assert.match(
      generated.items[0].non_executable_reasons[0],
      /could not be evaluated/,
    );
    assert.deepEqual(generated.items[0].operation, {
      action: "delete_file",
      path: "legacy.txt",
      source_sha256: legacySha,
    });
    assert.equal(
      generated.source.map.sha256,
      canonicalDigest(map),
    );
    assert.equal(
      generated.source.verification.sha256,
      verification.verification_result_sha256,
    );
    assert.equal(previewPlan(generated, root).status, "blocked");
    assert.equal(fs.existsSync(path.join(root, "legacy.txt")), true);
    assert.equal(
      generatePlan(map, verification, ["NO-LEGACY"], root).items[0].id,
      generated.items[0].id,
    );
  });
});

run("one exhaustive path projection emits one sorted item per path", () => {
  withTemporary("node-plan-multiple", (root) => {
    const hashes = {
      "legacy-z.js": write(root, "legacy-z.js", Buffer.from("z\n")),
      "legacy-a.js": write(root, "legacy-a.js", Buffer.from("a\n")),
    };
    const map = mapArtifact(Object.entries(hashes).map(
      ([filePath, sourceSha]) => ({
        path: filePath,
        sha256: sourceSha,
        symbols: [],
        exports: [],
      }),
    ));
    const [operand, issue] = forbiddenPathOperand(
      "p.paths",
      ["legacy-z.js", "legacy-a.js"],
    );
    const constraint = check(
      "NO-LEGACY",
      "cardinality",
      "p.paths",
      [issue],
      0,
    );
    const verification = verificationArtifact(
      map,
      [constraint],
      { "p.paths": { select: "inventory_paths", include: ["legacy-*.js"] } },
      [operand],
    );
    const generated = generatePlan(map, verification, null, root);
    assert.deepEqual(
      generated.items.map((row) => row.operation.path),
      ["legacy-a.js", "legacy-z.js"],
    );
    assert(generated.items.every((row) => !row.executable));
    assert.equal(
      new Set(generated.items.map(
        (row) => row.origins[0].issue_fingerprint,
      )).size,
      1,
    );
  });
});

run("incomplete or uncorrelated path projections never produce deletes", () => {
  withTemporary("node-plan-incomplete", (root) => {
    const sourceSha = write(root, "legacy.js", Buffer.from("old\n"));
    const map = mapArtifact([{
      path: "legacy.js",
      sha256: sourceSha,
      symbols: [],
      exports: [],
    }]);
    const [operand, issue] = forbiddenPathOperand("p.paths", ["legacy.js"]);
    const constraint = check(
      "NO-LEGACY",
      "cardinality",
      "p.paths",
      [issue],
      0,
    );
    operand.completeness.classification = "partial";
    operand.completeness.exhaustive = false;
    const verification = verificationArtifact(
      map,
      [constraint],
      { "p.paths": { select: "inventory_paths" } },
      [operand],
    );
    const generated = generatePlan(map, verification, null, root);
    assert.equal(generated.items.length, 1);
    assert.equal(generated.items[0].executable, false);
    assert.equal(generated.items[0].operation.action, "manual");
    assert.match(
      generated.items[0].non_executable_reasons[0],
      /not current, complete, exhaustive, and untruncated/,
    );
  });
});

run("generation never deletes a path with known consumers", () => {
  withTemporary("node-plan-consumer", (root) => {
    const legacySha = write(root, "legacy.js", Buffer.from("exports.x = 1;\n"));
    const consumerSha = write(
      root,
      "consumer.js",
      Buffer.from("require('./legacy');\n"),
    );
    const map = mapArtifact([
      { path: "consumer.js", sha256: consumerSha, symbols: [], exports: [] },
      { path: "legacy.js", sha256: legacySha, symbols: [], exports: [] },
    ], {
      edges: [{
        kind: "import",
        source: "consumer.js",
        target: "legacy.js",
        names: ["x"],
      }],
    });
    const [operand, issue] = forbiddenPathOperand(
      "p.paths",
      ["legacy.js"],
    );
    const constraint = check(
      "NO-LEGACY",
      "cardinality",
      "p.paths",
      [issue],
      0,
    );
    const verification = verificationArtifact(
      map,
      [constraint],
      { "p.paths": { select: "inventory_paths", include: ["legacy.js"] } },
      [operand],
    );
    const generated = generatePlan(map, verification, null, root);
    assert.equal(generated.items[0].executable, false);
    assert.match(
      generated.items[0].non_executable_reasons[0],
      /could not be evaluated/,
    );
    assert.equal(previewPlan(generated, root).status, "blocked");
  });
});

run("one issue fingerprint produces one Plan item", () => {
  withTemporary("node-plan-coalesced-finding", (root) => {
    const makeSha = write(
      root,
      "Makefile",
      Buffer.from("WASM_EXPORTS = _core_add\n"),
    );
    const headerSha = write(
      root,
      "src/core.h",
      Buffer.from("int core_sum(int left, int right);\n"),
    );
    const map = mapArtifact([
      { path: "Makefile", sha256: makeSha, symbols: [], exports: [] },
      { path: "src/core.h", sha256: headerSha, symbols: [], exports: [] },
    ]);
    const unavailable = finding(
      "core_sum",
      "Makefile",
      makeSha,
      "different",
    );
    unavailable.evidence_state = "unknown";
    const current = finding(
      "core_sum",
      "src/core.h",
      headerSha,
      "different",
    );
    current.fingerprint = unavailable.fingerprint;
    const constraint = check(
      "PROVIDER-PARITY",
      "mapped_values_equal",
      "p.surface",
      [unavailable, current],
    );
    const verification = verificationArtifact(
      map,
      [constraint],
      {
        "p.surface": {
          name: "ffi",
          select: "provider_surface",
        },
      },
    );

    const generated = generatePlan(map, verification, null, root);
    assert.equal(generated.items.length, 1);
    assert.deepEqual(
      generated.items[0].operation.candidate_paths,
      ["Makefile", "src/core.h"],
    );
    assert.equal(
      generated.items[0].non_executable_reasons.includes(
        "Finding evidence is waived, stale, inapplicable, or otherwise " +
        "not current executable evidence.",
      ),
      false,
    );
    assert.deepEqual(
      generated.items[0].evidence.map((row) => row.path),
      ["Makefile", "src/core.h"],
    );
    assert.equal(generated.unknowns.length, 1);
    assert.equal(previewPlan(generated, root).status, "blocked");

    const inconsistent = structuredClone(verification);
    inconsistent.constraints[0].findings[1].key = "other";
    const unsigned = structuredClone(inconsistent);
    delete unsigned.verification_result_sha256;
    inconsistent.verification_result_sha256 = canonicalDigest(unsigned);
    assert.throws(
      () => generatePlan(map, inconsistent, null, root),
      /one issue fingerprint across distinct/,
    );
  });
});

run("generation uses exact declaration extents from a native Map", () => {
  withTemporary("node-plan-symbol", (root) => {
    const source = Buffer.from(
      "function _obsolete() {}\nfunction keep() {}\n",
    );
    const sourceSha = write(root, "src/api.js", source);
    const end = source.indexOf(Buffer.from("\n"));
    const config = Buffer.from(JSON.stringify({
      project: "node-plan-symbol",
      constraints: {
        "NO-OBSOLETE": {
          kind: "forbidden_symbols",
          symbols: ["_obsolete"],
          paths: ["src/api.js"],
          owner: "architecture",
          rationale: "Obsolete internals stay absent.",
        },
      },
    }));
    const project = Project.fromRepository(root, {
      config,
      cacheDir: null,
      mapCache: false,
    });
    const map = JSON.parse(project.mapJson());
    const verification = JSON.parse(project.verifyJson());
    const generated = generatePlan(map, verification, null, root);
    assert.equal(generated.items[0].executable, true);
    assert.deepEqual(generated.items[0].operation, {
      action: "replace_range",
      path: "src/api.js",
      source_sha256: sourceSha,
      start_byte: 0,
      end_byte: end,
      before: "function _obsolete() {}",
      replacement: "",
    });
    assert.match(
      previewPlan(generated, root).changes[0].unified_diff,
      /-function _obsolete\(\) \{\}/,
    );
  });
});

run("primitive symbol constraints remove or propose exact rename", () => {
  withTemporary("node-plan-symbol-extras", (root) => {
    const source = Buffer.from(
      "function keep() {}\nfunction obsolete() {}\n",
    );
    const sourceSha = write(root, "src/api.js", source);
    const config = Buffer.from(JSON.stringify({
      project: "node-plan-symbol-extras",
      projections: {
        "api-symbols": {
          select: "symbols",
          paths: ["src/api.js"],
        },
      },
      constraints: {
        "SYMBOL-SUBSET": {
          assert: "subset",
          actual: { projection: "api-symbols" },
          expected: { literal: ["keep"] },
          owner: "architecture",
          rationale: "Only reviewed symbols remain.",
        },
        "SYMBOL-DISJOINT": {
          assert: "disjoint",
          actual: { projection: "api-symbols" },
          expected: { literal: ["obsolete"] },
          owner: "architecture",
          rationale: "Obsolete symbols remain absent.",
        },
        "SYMBOL-EQUAL": {
          assert: "set_equal",
          actual: { projection: "api-symbols" },
          expected: { literal: ["keep", "missing"] },
          owner: "architecture",
          rationale: "The reviewed symbol set remains exact.",
        },
      },
    }));
    const project = Project.fromRepository(root, {
      config,
      cacheDir: null,
      mapCache: false,
    });
    const map = JSON.parse(project.mapJson());
    const verification = JSON.parse(project.verifyJson());

    for (const constraintId of ["SYMBOL-SUBSET", "SYMBOL-DISJOINT"]) {
      const generated = generatePlan(
        map,
        verification,
        [constraintId],
        root,
      );
      assert.equal(generated.items.length, 1);
      const item = generated.items[0];
      assert.equal(item.executable, true);
      assert.equal(item.operation.action, "replace_range");
      assert.equal(item.operation.path, "src/api.js");
      assert.equal(item.operation.source_sha256, sourceSha);
      assert.match(item.operation.before, /obsolete/);
    }

    const equalPlan = generatePlan(
      map,
      verification,
      ["SYMBOL-EQUAL"],
      root,
    );
    assert.equal(equalPlan.items.length, 1);
    const suggestion = equalPlan.items[0];
    assert.equal(suggestion.origins.length, 2);
    assert.equal(suggestion.executable, false);
    assert.equal(suggestion.operation.action, "rename_symbol");
    assert.equal(suggestion.operation.symbol, "obsolete");
    assert.equal(suggestion.operation.new_name, "missing");
    assert.equal(suggestion.operation.sites.length, 1);
    assert.match(
      suggestion.non_executable_reasons.at(-1),
      /review it with --rename/,
    );
    assert.equal(previewPlan(equalPlan, root, map).status, "blocked");
    const intentLaundering = structuredClone(equalPlan);
    intentLaundering.items[0].executable = true;
    intentLaundering.items[0].non_executable_reasons = [];
    const blocked = previewPlan(intentLaundering, root, map);
    assert.equal(blocked.status, "blocked");
    assert.match(
      blocked.diagnostics[0].message,
      /requires asserted rename intent/,
    );

    const incomplete = structuredClone(verification);
    const subset = incomplete.constraints.find(
      (row) => row.id === "SYMBOL-SUBSET",
    );
    const actual = incomplete.operands.find(
      (row) => row.name === subset.operands.actual,
    );
    actual.completeness.classification = "partial";
    actual.completeness.exhaustive = false;
    delete incomplete.verification_result_sha256;
    incomplete.verification_result_sha256 = canonicalDigest(incomplete);
    const guarded = generatePlan(
      map,
      incomplete,
      ["SYMBOL-SUBSET"],
      root,
    ).items[0];
    assert.equal(guarded.executable, false);
    assert.equal(guarded.operation.action, "manual");
    assert.match(
      guarded.non_executable_reasons[0],
      /not current, complete/,
    );
  });
});

run("generation rejects tampered Verification seals and stale roots", () => {
  withTemporary("node-plan-seal", (root) => {
    const sourceSha = write(root, "legacy", Buffer.from("old\n"));
    const map = mapArtifact([
      { path: "legacy", sha256: sourceSha, symbols: [], exports: [] },
    ]);
    const [operand, issue] = forbiddenPathOperand("p.paths", ["legacy"]);
    const constraint = check(
      "NO-LEGACY",
      "cardinality",
      "p.paths",
      [issue],
      0,
    );
    const verification = verificationArtifact(
      map,
      [constraint],
      { "p.paths": { select: "inventory_paths" } },
      [operand],
    );
    verification.summary.blocking = false;
    assert.throws(
      () => generatePlan(map, verification, null, root),
      /digest does not match/,
    );
    assert.throws(
      () => generatePlan(map, {
        ...verificationArtifact(
          map,
          [constraint],
          { "p.paths": { select: "inventory_paths" } },
          [operand],
        ),
        evaluations: [],
      }, null, root),
      /digest does not match|exactly one current/,
    );
  });
});

run("binary file changes retain schema-valid diff headers", () => {
  withTemporary("node-act-binary", (root) => {
    const source = Buffer.from([0xff, 0x00, 0x01]);
    const sourceSha = write(root, "binary.dat", source);
    const preview = previewPlan(plan(operationItem("delete", {
      action: "delete_file",
      path: "binary.dat",
      source_sha256: sourceSha,
    })), root);
    assert.equal(preview.status, "preview");
    assert.match(
      preview.changes[0].unified_diff,
      /--- a\/binary\.dat\n\+\+\+ \/dev\/null/,
    );
    assert.match(preview.changes[0].unified_diff, /Binary files/);
  });
});

run("Node CLI plans, previews, applies, and freshly verifies an exact delete", () => {
  withTemporary("node-plan-act-cli", (root) => {
    fs.writeFileSync(
      path.join(root, "archbird.json"),
      JSON.stringify({
        project: "node-plan-act-cli",
        constraints: {
          "NO-LEGACY": {
            kind: "forbidden_paths",
            paths: ["legacy.js"],
            owner: "architecture",
            rationale: "Obsolete implementation stays absent.",
          },
        },
      }),
    );
    write(root, "legacy.js", Buffer.from("export const obsolete = true;\n"));
    const planPath = path.join(root, "plan.json");
    const planResult = cli(["plan", "--root", root, "--output", planPath]);
    assert.equal(
      Buffer.from(planResult.stdout).toString("utf8"),
      "Result: items=1; executable=1; non-executable=0; " +
      "unknowns=0; preserved-constraints=0\n",
    );
    const generated = JSON.parse(fs.readFileSync(planPath));
    assert.equal(generated.items.length, 1);
    assert.equal(generated.items[0].operation.action, "delete_file");
    cli(["plan", "--root", root, "--output", planPath]);
    assert.deepEqual(
      JSON.parse(fs.readFileSync(planPath)).source,
      generated.source,
    );

    const preview = cli([
      "act", planPath, "--root", root, "--format", "patch",
    ]);
    assert.match(Buffer.from(preview.stdout).toString("utf8"), /--- a\/legacy\.js/);
    assert.equal(fs.existsSync(path.join(root, "legacy.js")), true);

    const applied = cli([
      "act", planPath, "--root", root, "--apply", "--format", "json",
    ]);
    const result = JSON.parse(Buffer.from(applied.stdout).toString("utf8"));
    assert.equal(result.status, "applied");
    assert.equal(result.acceptance.status, "satisfied");
    assert.deepEqual(result.acceptance.constraints, [{
      id: "NO-LEGACY",
      status: "pass",
    }]);
    assert.equal(fs.existsSync(path.join(root, "legacy.js")), false);
  });
});

run("Node CLI closes Map Query Verify Plan Act symbol rename loop", () => {
  withTemporary("node-plan-act-rename", (root) => {
    fs.writeFileSync(
      path.join(root, "archbird.json"),
      JSON.stringify({
        project: "node-plan-act-rename",
        layers: [{
          name: "javascript",
          language: "javascript",
          globs: ["*.js"],
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
            rationale: "The reviewed JavaScript API rename is complete.",
          },
        },
      }),
    );
    write(
      root,
      "api.js",
      Buffer.from("export function oldApi(value) { return value + 1; }\n"),
    );
    write(
      root,
      "consumer.js",
      Buffer.from(
        'import { oldApi } from "./api.js";\n' +
        "export const result = oldApi(1);\n",
      ),
    );
    const beforeQuery = cli([
      "query",
      "--root",
      root,
      "--symbol",
      "oldApi",
      "--format",
      "json",
      "--check",
    ]);
    assert.match(beforeQuery.stdout.toString("utf8"), /"oldApi"/);
    cli(["verify", "--root", root, "--check"], 1);

    const planPath = path.join(root, "rename-plan.json");
    cli([
      "plan",
      "API-SURFACE",
      "--root",
      root,
      "--output",
      planPath,
    ]);
    const suggestion = JSON.parse(fs.readFileSync(planPath));
    assert.equal(suggestion.items[0].operation.action, "rename_symbol");
    assert.equal(suggestion.items[0].executable, false);
    cli([
      "act",
      planPath,
      "--root",
      root,
      "--format",
      "json",
    ], 2);
    cli([
      "plan",
      "API-SURFACE",
      "--root",
      root,
      "--rename",
      "oldApi=newApi",
      "--output",
      planPath,
    ]);
    const generated = JSON.parse(fs.readFileSync(planPath));
    assert.equal(generated.items.length, 1);
    assert.equal(generated.items[0].executable, true);
    assert.equal(generated.items[0].operation.action, "rename_symbol");
    assert.deepEqual(
      new Set(generated.items[0].operation.sites.map((row) => row.path)),
      new Set(["api.js", "consumer.js"]),
    );
    assert.equal(generated.items[0].operation.sites.length, 3);

    const patch = cli([
      "act",
      planPath,
      "--root",
      root,
      "--format",
      "patch",
    ]).stdout.toString("utf8");
    assert.match(patch, /--- a\/api\.js/);
    assert.match(patch, /--- a\/consumer\.js/);
    cli([
      "act",
      planPath,
      "--root",
      root,
      "--apply",
      "--format",
      "json",
    ]);
    assert.doesNotMatch(fs.readFileSync(path.join(root, "api.js"), "utf8"), /oldApi/);
    assert.doesNotMatch(
      fs.readFileSync(path.join(root, "consumer.js"), "utf8"),
      /oldApi/,
    );
    cli(["verify", "--root", root, "--check"]);
    const afterQuery = cli([
      "query",
      "--root",
      root,
      "--symbol",
      "newApi",
      "--format",
      "json",
      "--check",
    ]);
    assert.match(afterQuery.stdout.toString("utf8"), /"newApi"/);
  });
});

run("Node CLI renames one TypeScript symbol across TS TSX and aliases", () => {
  withTemporary("node-plan-act-typescript-rename", (root) => {
    fs.writeFileSync(
      path.join(root, "archbird.json"),
      JSON.stringify({
        project: "node-plan-act-typescript-rename",
        layers: [{
          name: "typescript",
          language: "typescript",
          globs: ["*.ts", "*.tsx"],
        }],
        projections: {
          "api-symbols": {
            select: "symbols",
            paths: ["api.ts"],
          },
        },
        constraints: {
          "API-SURFACE": {
            assert: "set_equal",
            actual: { projection: "api-symbols" },
            expected: { literal: ["newApi"] },
            owner: "architecture",
            rationale: "The reviewed TypeScript API rename is complete.",
          },
        },
      }),
    );
    write(
      root,
      "api.ts",
      Buffer.from("export function oldApi(value: number) { return value + 1; }\n"),
    );
    write(
      root,
      "alias.ts",
      Buffer.from(
        'import { oldApi as keptAlias } from "./api";\n' +
        "export const aliasResult = keptAlias(2);\n",
      ),
    );
    write(
      root,
      "consumer.ts",
      Buffer.from(
        'import { oldApi } from "./api";\n' +
        "export const result = oldApi(1);\n",
      ),
    );
    write(
      root,
      "view.tsx",
      Buffer.from(
        'import { oldApi } from "./api";\n' +
        "export const View = () => <output>{oldApi(3)}</output>;\n",
      ),
    );
    write(
      root,
      "unrelated.ts",
      Buffer.from(
        "function oldApi(value: number) { return value - 1; }\n" +
        "export const unrelated = oldApi(4);\n",
      ),
    );

    cli(["verify", "--root", root, "--check"], 1);
    const planPath = path.join(root, "rename-plan.json");
    cli([
      "plan",
      "API-SURFACE",
      "--root",
      root,
      "--rename",
      "oldApi=newApi",
      "--output",
      planPath,
    ]);
    const plan = JSON.parse(fs.readFileSync(planPath));
    const operation = plan.items[0].operation;
    assert.equal(plan.items[0].executable, true);
    assert.equal(operation.action, "rename_symbol");
    assert.deepEqual(
      new Set(operation.sites.map((row) => row.path)),
      new Set(["alias.ts", "api.ts", "consumer.ts", "view.tsx"]),
    );
    assert.equal(
      operation.sites.some((row) => row.path === "unrelated.ts"),
      false,
    );

    const patch = cli([
      "act",
      planPath,
      "--root",
      root,
      "--format",
      "patch",
    ]).stdout.toString("utf8");
    assert.match(patch, /--- a\/api\.ts/);
    assert.match(patch, /--- a\/view\.tsx/);
    assert.doesNotMatch(patch, /--- a\/unrelated\.ts/);
    cli([
      "act",
      planPath,
      "--root",
      root,
      "--apply",
      "--format",
      "json",
    ]);
    assert.match(
      fs.readFileSync(path.join(root, "alias.ts"), "utf8"),
      /newApi as keptAlias/,
    );
    assert.match(
      fs.readFileSync(path.join(root, "alias.ts"), "utf8"),
      /keptAlias\(2\)/,
    );
    assert.doesNotMatch(
      fs.readFileSync(path.join(root, "api.ts"), "utf8"),
      /oldApi/,
    );
    assert.match(
      fs.readFileSync(path.join(root, "unrelated.ts"), "utf8"),
      /oldApi/,
    );
    cli(["verify", "--root", root, "--check"]);
  });
});

run("Node CLI rolls back a selected fix that breaks preserved policy", () => {
  withTemporary("node-plan-act-rejected", (root) => {
    fs.writeFileSync(
      path.join(root, "archbird.json"),
      JSON.stringify({
        project: "node-plan-act-rejected",
        constraints: {
          "NO-LEGACY": {
            kind: "forbidden_paths",
            paths: ["legacy.js"],
            owner: "architecture",
            rationale: "Obsolete implementation stays absent.",
          },
          "KEEP-API": {
            kind: "required_symbols",
            symbols: ["requiredApi"],
            paths: ["legacy.js"],
            owner: "architecture",
            rationale: "The existing API remains available.",
          },
        },
      }),
    );
    const source = "export function requiredApi() { return 1; }\n";
    fs.writeFileSync(path.join(root, "legacy.js"), source);
    fs.mkdirSync(path.join(root, ".archbird"));
    const planPath = path.join(root, ".archbird/plan.json");
    cli([
      "plan", "NO-LEGACY", "--root", root, "--output", planPath,
    ]);
    const generated = JSON.parse(fs.readFileSync(planPath));
    assert.deepEqual(generated.preserved_constraints, ["KEEP-API"]);
    assert.equal(generated.items[0].executable, true);

    const completed = cli([
      "act", planPath, "--root", root, "--apply", "--format", "json",
    ], 1);
    const result = JSON.parse(Buffer.from(completed.stdout).toString("utf8"));
    assert.equal(result.status, "rejected");
    assert.equal(result.acceptance.status, "not_satisfied");
    assert.equal(fs.readFileSync(path.join(root, "legacy.js"), "utf8"), source);

    const markdown = Buffer.from(cli([
      "act", planPath, "--root", root, "--apply",
    ], 1).stdout).toString("utf8");
    assert.match(markdown, /- `NO-LEGACY`: `pass`/);
    assert.match(markdown, /- `KEEP-API`: `fail`/);
  });
});

process.stdout.write("Node Plan/Act tests passed.\n");
