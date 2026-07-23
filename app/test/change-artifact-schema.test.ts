import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import Ajv2020 from "ajv/dist/2020.js";

const artifacts = [
  {
    name: "change-proposal",
    artifact: "../../test/fuzz/corpus/act-proposal/proposal.json",
  },
  {
    name: "change-contract",
    artifact: "../../test/fuzz/corpus/act-contract/contract.json",
  },
  {
    name: "change-result",
    artifact: "../../test/fuzz/corpus/act-result/result.json",
  },
] as const;

test("canonical change artifacts conform to their public schemas", () => {
  for (const entry of artifacts) {
    const schema = JSON.parse(
      readFileSync(
        new URL(`../../schema/${entry.name}.schema.json`, import.meta.url),
        "utf8",
      ),
    );
    const artifact = JSON.parse(
      readFileSync(new URL(entry.artifact, import.meta.url), "utf8"),
    );
    const validator = new Ajv2020({
      allErrors: true,
      strict: true,
      strictRequired: false,
      strictTypes: false,
    }).compile(schema);
    assert.equal(
      validator(artifact),
      true,
      `${entry.name}: ${JSON.stringify(validator.errors)}`,
    );
  }
});
