import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import Ajv2020 from "ajv/dist/2020.js";

const sha = (character: string): string => character.repeat(64);

const plan = {
  schema_version: 6,
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
      } as Record<string, unknown>,
      acceptance: { constraints: ["NO-LEGACY"] },
      unknowns: [],
      executable: true,
      non_executable_reasons: [] as string[],
    },
  ],
  preserved_constraints: ["NO-CYCLES"],
  unknowns: [],
};

const act = {
  schema_version: 2,
  artifact: "act",
  provenance: "derived",
  tool: {
    name: "archbird",
    version: "0.0.1",
    implementation_sha256: sha("6"),
  },
  plan_sha256: sha("7"),
  source: plan.source,
  state: "accepted",
  after: {
    map: {
      sha256: sha("8"),
      input_sha256: sha("9"),
      configuration_sha256: sha("a"),
      producer_implementation_sha256: sha("b"),
    },
    verification: {
      sha256: sha("c"),
      policy_sha256: sha("d"),
      producer_implementation_sha256: sha("e"),
    },
  },
  executors: [{
    capability: "core.replace_range@1",
    implementation_sha256: sha("f"),
    deterministic: true,
    item_ids: ["remove-legacy"],
    matches: 1,
    skipped: 0,
    unsupported: 0,
    reads: ["src/api.py"],
    writes: ["src/api.py"],
  }],
  transitions: [
    {
      item_ids: ["remove-legacy"],
      kind: "modify",
      path: "src/api.py",
      source_path: null,
      before: {
        sha256: sha("5"),
        byte_length: 4,
        executable: false,
      },
      after: {
        sha256: sha("8"),
        byte_length: 4,
        executable: false,
        content_base64: "bmV3Cg==",
      },
    },
  ],
  acceptance: {
    status: "satisfied",
    verification_sha256: sha("c"),
    constraints: [
      { id: "NO-LEGACY", status: "pass" },
      { id: "NO-CYCLES", status: "pass" },
    ],
  },
  seal: { content_sha256: sha("0") },
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

test("Plan and Act examples satisfy their public schemas", () => {
  const validatePlan = validator("plan");
  const validateAct = validator("act");
  assert.equal(
    validatePlan(plan),
    true,
    JSON.stringify(validatePlan.errors),
  );
  assert.equal(
    validateAct(act),
    true,
    JSON.stringify(validateAct.errors),
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

test("Plan schema represents neutral missing-symbol and test-route work", () => {
  const validate = validator("plan");
  const neutral = structuredClone(plan);
  neutral.items[0].executable = false;
  neutral.items[0].non_executable_reasons = [
    "Architecture evidence does not determine implementation source.",
  ];
  neutral.items[0].operation = {
    action: "add_symbol",
    kinds: ["function"],
    path: "src/api.c",
    symbol: "future_api",
  };
  assert.equal(validate(neutral), true, JSON.stringify(validate.errors));

  neutral.items[0].operation = {
    action: "add_test_route",
    group: "c",
    path: "test/test_api.c",
    selectors: ["api"],
    target: "src/api.c",
  };
  assert.equal(validate(neutral), true, JSON.stringify(validate.errors));

  neutral.items[0].executable = true;
  neutral.items[0].non_executable_reasons = [];
  assert.equal(validate(neutral), false);
});

test("Plan create_file is a path-only input-required objective", () => {
  const validate = validator("plan");
  const create = structuredClone(plan);
  create.items[0].executable = false;
  create.items[0].non_executable_reasons = [
    "Verification does not define file content.",
  ];
  create.items[0].operation = {
    action: "create_file",
    path: "generated/entry.py",
  };
  assert.equal(validate(create), true, JSON.stringify(validate.errors));

  create.items[0].operation.content = "ENTRY = True\n";
  assert.equal(validate(create), false);
  delete create.items[0].operation.content;
  create.items[0].operation.path = "generated/*.py";
  assert.equal(validate(create), false);
});

test("Plan add_dependency is an exact input-required objective", () => {
  const validate = validator("plan");
  const dependency = structuredClone(plan);
  dependency.items[0].executable = false;
  dependency.items[0].non_executable_reasons = [
    "The required dependency does not define its source edit.",
  ];
  dependency.items[0].operation = {
    action: "add_dependency",
    name: "provider",
    relation: "import",
    source_path: "src/consumer.py",
    target_path: "src/provider.py",
  };
  assert.equal(
    validate(dependency),
    true,
    JSON.stringify(validate.errors),
  );

  dependency.items[0].executable = true;
  dependency.items[0].non_executable_reasons = [];
  assert.equal(validate(dependency), false);
  dependency.items[0].executable = false;
  dependency.items[0].non_executable_reasons = ["review"];
  dependency.items[0].operation.relation = "imp*";
  assert.equal(validate(dependency), false);
  dependency.items[0].operation.relation = "import";
  dependency.items[0].operation.source_path = "src/*.py";
  assert.equal(validate(dependency), false);
});

test("Act state cannot claim acceptance before evaluation", () => {
  const validate = validator("act");
  const materialized = structuredClone(act) as {
    state: string;
    after: unknown;
    seal: unknown;
    acceptance: {
      status: string;
      verification_sha256: string | null;
      constraints: Array<{ id: string; status: string }>;
    };
  };
  materialized.state = "materialized";
  materialized.after = null;
  materialized.seal = null;
  materialized.acceptance = {
    status: "not_evaluated",
    verification_sha256: null,
    constraints: [
      { id: "NO-LEGACY", status: "not_evaluated" },
      { id: "NO-CYCLES", status: "not_evaluated" },
    ],
  };
  assert.equal(validate(materialized), true, JSON.stringify(validate.errors));

  materialized.acceptance.status = "satisfied";
  assert.equal(validate(materialized), false);

  const invalidTransition = structuredClone(act);
  invalidTransition.transitions[0].kind = "delete";
  assert.equal(validate(invalidTransition), false);
});
