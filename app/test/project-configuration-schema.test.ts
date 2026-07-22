import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import test from "node:test";
import { fileURLToPath } from "node:url";
import Ajv2020 from "ajv/dist/2020.js";

interface ConformanceCase {
  id: string;
  valid: boolean;
  configuration: unknown;
}

const schema = JSON.parse(
  readFileSync(new URL("../../schema/archbird.schema.json", import.meta.url), "utf8"),
);
const corpus = JSON.parse(
  readFileSync(
    new URL("../../test/fixtures/project_configuration_conformance.json", import.meta.url),
    "utf8",
  ),
) as { schema_version: number; cases: ConformanceCase[] };

test("public project-configuration schema matches the conformance corpus", () => {
  assert.equal(corpus.schema_version, 1);
  const validator = new Ajv2020({
    allErrors: true,
    strict: true,
    strictRequired: false,
    strictTypes: false,
  }).compile(schema);
  for (const entry of corpus.cases) {
    const actual = validator(entry.configuration);
    assert.equal(
      actual,
      entry.valid,
      `${entry.id}: ${JSON.stringify(validator.errors)}`,
    );
  }
  const matrix = corpus.cases.find(
    (entry) => entry.id === "all-projection-operators",
  );
  assert.ok(matrix && typeof matrix.configuration === "object");
  const projections = (matrix.configuration as {
    projections: Record<string, Record<string, unknown>>;
  }).projections;
  for (const [id, projection] of Object.entries(projections)) {
    const foreign = projection.select === "file_metrics"
      ? { artifacts: ["library"] }
      : { metric: "bytes" };
    const mutated = structuredClone(matrix.configuration) as {
      projections: Record<string, Record<string, unknown>>;
    };
    mutated.projections = { [id]: { ...projection, ...foreign } };
    assert.equal(
      validator(mutated),
      false,
      `${id} accepted an option owned by another projection operator`,
    );
  }
  const constraintMatrix = corpus.cases.find(
    (entry) => entry.id === "all-constraint-forms",
  );
  assert.ok(constraintMatrix && typeof constraintMatrix.configuration === "object");
  const constraints = (constraintMatrix.configuration as {
    constraints: Record<string, Record<string, unknown>>;
  }).constraints;
  for (const [id, constraint] of Object.entries(constraints)) {
    const foreign = "kind" in constraint
      ? { actual: { literal: [] } }
      : { bridge: "unused" };
    const mutated = structuredClone(constraintMatrix.configuration) as {
      constraints: Record<string, Record<string, unknown>>;
    };
    mutated.constraints = { [id]: { ...constraint, ...foreign } };
    assert.equal(
      validator(mutated),
      false,
      `${id} accepted an option owned by another constraint contract`,
    );
  }
  const repository = fileURLToPath(new URL("../..", import.meta.url));
  const configuredExamples = [
    `${repository}/archbird.json`,
    ...readdirSync(`${repository}/examples`)
      .filter((name) => name.endsWith(".json"))
      .map((name) => `${repository}/examples/${name}`),
  ];
  for (const path of configuredExamples) {
    const configuration = JSON.parse(readFileSync(path, "utf8"));
    if (configuration.schema_version !== 2) continue;
    assert.equal(
      validator(configuration),
      true,
      `${path}: ${JSON.stringify(validator.errors)}`,
    );
  }
});
