import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import Ajv2020 from "ajv/dist/2020.js";

const sha = (character: string): string => character.repeat(64);

const plan = {
  schema_version: 1,
  artifact: "plan",
  provenance: "derived",
  tool: {
    name: "archbird",
    version: "0.0.1",
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
      policy_sha256: sha("1"),
      producer_implementation_sha256: sha("2"),
    },
  },
  objective: "Remove the forbidden declaration.",
  items: [
    {
      id: "remove-legacy",
      statement: "Remove legacy from src/api.py.",
      provenance: "derived",
      origins: [
        {
          constraint_id: "NO-LEGACY",
          constraint_result_sha256: sha("3"),
          issue_fingerprint: sha("4"),
        },
      ],
      evidence: [
        {
          provenance: "derived",
          project: "fixture",
          path: "src/api.py",
          line: 3,
          sha256: sha("5"),
          detail: "function legacy",
        },
      ],
      depends_on: [],
      operation: {
        action: "replace_range",
        path: "src/api.py",
        source_sha256: sha("5"),
        start_byte: 12,
        end_byte: 25,
        before: "def legacy():",
        replacement: "",
      },
      acceptance: { constraints: ["NO-LEGACY"] },
      unknowns: [],
      executable: true,
      non_executable_reasons: [],
    },
  ],
  preserved_constraints: ["NO-CYCLES"],
  unknowns: [],
};

const result = {
  schema_version: 1,
  artifact: "act-result",
  provenance: "derived",
  tool: {
    name: "archbird",
    version: "0.0.1",
    implementation_sha256: sha("6"),
  },
  plan_sha256: sha("7"),
  status: "applied",
  changes: [
    {
      item_ids: ["remove-legacy"],
      kind: "modify",
      path: "src/api.py",
      source_path: null,
      before_sha256: sha("5"),
      after_sha256: sha("8"),
      unified_diff:
        "diff --git a/src/api.py b/src/api.py\n" +
        "--- a/src/api.py\n+++ b/src/api.py\n@@ -1 +1 @@\n-old\n+new\n",
    },
  ],
  acceptance: {
    status: "satisfied",
    verification_sha256: sha("9"),
    constraints: [
      { id: "NO-LEGACY", status: "pass" },
      { id: "NO-CYCLES", status: "pass" },
    ],
  },
  diagnostics: [] as Array<Record<string, unknown>>,
};

function validator(name: string) {
  const schema = JSON.parse(
    readFileSync(
      new URL(`../../schema/${name}.schema.json`, import.meta.url),
      "utf8",
    ),
  );
  return new Ajv2020({
    allErrors: true,
    strict: true,
    strictRequired: false,
    strictTypes: false,
  }).compile(schema);
}

test("Plan and ActResult examples satisfy their public schemas", () => {
  const validatePlan = validator("plan");
  const validateResult = validator("act-result");
  assert.equal(
    validatePlan(plan),
    true,
    JSON.stringify(validatePlan.errors),
  );
  assert.equal(
    validateResult(result),
    true,
    JSON.stringify(validateResult.errors),
  );

  const noOp = structuredClone(plan);
  noOp.items = [];
  noOp.preserved_constraints = [];
  assert.equal(validatePlan(noOp), true, JSON.stringify(validatePlan.errors));
});

test("Plan schema rejects ambiguous operations and unsafe paths", () => {
  const validate = validator("plan");
  const ambiguous = structuredClone(plan);
  Object.assign(ambiguous.items[0].operation, {
    action: "delete_file",
  });
  assert.equal(validate(ambiguous), false);

  const escaped = structuredClone(plan);
  escaped.items[0].operation.path = "../src/api.py";
  assert.equal(validate(escaped), false);
});

test("ActResult acceptance cannot claim success before evaluation", () => {
  const validate = validator("act-result");
  const preview = structuredClone(result) as {
    status: string;
    acceptance: {
      status: string;
      verification_sha256: string | null;
      constraints: Array<{ id: string; status: string }>;
    };
  };
  preview.status = "preview";
  preview.acceptance = {
    status: "not_evaluated",
    verification_sha256: null,
    constraints: [],
  };
  assert.equal(validate(preview), true, JSON.stringify(validate.errors));

  preview.acceptance.status = "satisfied";
  assert.equal(validate(preview), false);

  const rejected = structuredClone(result);
  rejected.status = "rejected";
  rejected.acceptance = {
    status: "not_satisfied",
    verification_sha256: sha("9"),
    constraints: [
      { id: "NO-LEGACY", status: "fail" },
      { id: "NO-CYCLES", status: "pass" },
    ],
  };
  rejected.diagnostics = [
    {
      code: "acceptance_rejected",
      severity: "error",
      message: "Fresh verification rejected the change; edits were rolled back.",
      item_id: null,
      path: null,
    },
  ];
  assert.equal(validate(rejected), true, JSON.stringify(validate.errors));
});
