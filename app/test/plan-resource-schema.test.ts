import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import Ajv2020 from "ajv/dist/2020.js";

const MAX_SAFE_INTEGER = 9_007_199_254_740_991;
const MAX_ARRAY_ITEMS = 4_096;
const MAX_METADATA_LENGTH = 65_536;
const MAX_OPERATION_TEXT_LENGTH = 16_777_216;

function loadSchema(name: string): Record<string, unknown> {
  return JSON.parse(
    readFileSync(
      new URL(`../../schema/${name}.schema.json`, import.meta.url),
      "utf8",
    ),
  );
}

function validatePlan() {
  return new Ajv2020({
    allErrors: true,
    strict: true,
    strictRequired: false,
    strictTypes: false,
  }).compile(loadSchema("plan"));
}

function plan() {
  const sha = (character: string): string => character.repeat(64);
  return {
    schema_version: 6,
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
        policy_sha256: sha("1"),
        producer_implementation_sha256: sha("2"),
      },
    },
    objective: "exercise resource bounds",
    items: [{
      id: "edit",
      statement: "edit",
      provenance: "derived",
      origins: [{
        constraint_id: "TEST",
        constraint_result_sha256: sha("3"),
        issue_fingerprint: sha("4"),
      }],
      evidence: [{
        provenance: "derived",
        project: "fixture",
        path: "source.txt",
        line: 1,
        sha256: sha("5"),
        detail: "source",
      }],
      depends_on: [],
      operation: {
        action: "replace_range",
        path: "source.txt",
        source_sha256: sha("5"),
        start_byte: 0,
        end_byte: 0,
        before: "",
        replacement: "",
      },
      acceptance: { constraints: ["TEST"] },
      unknowns: [],
      executable: true,
      non_executable_reasons: [],
    }],
    preserved_constraints: [],
    unknowns: [],
  };
}

test("Plan schema uses JavaScript-safe source coordinates", () => {
  const validate = validatePlan();
  const boundary = plan();
  boundary.items[0].evidence[0].line = MAX_SAFE_INTEGER;
  boundary.items[0].operation.start_byte = MAX_SAFE_INTEGER;
  boundary.items[0].operation.end_byte = MAX_SAFE_INTEGER;
  assert.equal(validate(boundary), true, JSON.stringify(validate.errors));

  const unsafeLine = structuredClone(boundary);
  unsafeLine.items[0].evidence[0].line = MAX_SAFE_INTEGER + 1;
  assert.equal(validate(unsafeLine), false);

  const unsafeOffset = structuredClone(boundary);
  unsafeOffset.items[0].operation.end_byte = MAX_SAFE_INTEGER + 1;
  assert.equal(validate(unsafeOffset), false);
});

test("Plan schema bounds metadata, operation text, and collections", () => {
  const validate = validatePlan();

  const metadata = plan();
  metadata.items[0].statement = "s".repeat(MAX_METADATA_LENGTH);
  assert.equal(validate(metadata), true, JSON.stringify(validate.errors));
  metadata.items[0].statement += "s";
  assert.equal(validate(metadata), false);

  const operationText = plan();
  operationText.items[0].operation.replacement =
    "r".repeat(MAX_OPERATION_TEXT_LENGTH + 1);
  assert.equal(validate(operationText), false);

  const candidates: any = plan();
  candidates.items[0].executable = false;
  candidates.items[0].non_executable_reasons = ["review"];
  candidates.items[0].operation = {
    action: "manual",
    instructions: "review",
    candidate_paths: Array.from(
      { length: MAX_ARRAY_ITEMS + 1 },
      (_, index) => `path-${index}`,
    ),
  };
  assert.equal(validate(candidates), false);
});

test("Plan and Act schemas publish the host byte budgets", () => {
  const planSchema = loadSchema("plan") as {
    $comment: string;
    properties: {
      items: { maxItems: number };
      unknowns: { maxItems: number };
    };
    $defs: {
      replaceRange: {
        properties: {
          before: { maxLength: number };
          replacement: { maxLength: number };
        };
      };
    };
  };
  const actSchema = loadSchema("act") as {
    $comment: string;
    properties: {
      transitions: { maxItems: number };
      executors: { maxItems: number };
    };
    $defs: {
      afterFile: {
        properties: {
          byte_length: { maximum: number };
          content_base64: { maxLength: number };
        };
      };
    };
  };

  assert.match(planSchema.$comment, /67108864 bytes/);
  assert.match(planSchema.$comment, /268435456 bytes/);
  assert.equal(planSchema.properties.items.maxItems, MAX_ARRAY_ITEMS);
  assert.equal(planSchema.properties.unknowns.maxItems, MAX_ARRAY_ITEMS);
  assert.equal(
    planSchema.$defs.replaceRange.properties.before.maxLength,
    MAX_OPERATION_TEXT_LENGTH,
  );
  assert.equal(
    planSchema.$defs.replaceRange.properties.replacement.maxLength,
    MAX_OPERATION_TEXT_LENGTH,
  );
  assert.match(actSchema.$comment, /268435456 bytes/);
  assert.equal(actSchema.properties.transitions.maxItems, MAX_ARRAY_ITEMS);
  assert.equal(actSchema.properties.executors.maxItems, MAX_ARRAY_ITEMS);
  assert.equal(
    actSchema.$defs.afterFile.properties.byte_length.maximum,
    67_108_864,
  );
  assert.equal(
    actSchema.$defs.afterFile.properties.content_base64.maxLength,
    89_478_488,
  );
});
