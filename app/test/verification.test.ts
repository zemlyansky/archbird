import assert from "node:assert/strict";
import test from "node:test";

import { parseArtifact } from "../src/artifacts/model";
import {
  reviewedProjectConfiguration,
  verificationFindings,
  waiverCandidate,
} from "../src/artifacts/verification";

const encoder = new TextEncoder();

test("project constraints load as one editable schema-2 configuration", () => {
  const configuration = {
    schema_version: 2,
    project: "demo",
    layers: [{ name: "core", language: "c", globs: ["src/**"] }],
    constraints: [{
      id: "ARCH-1",
      assert: "acyclic",
      actual: { projection: { select: "component_edges" } },
      owner: "architecture",
      rationale: "Keep dependencies acyclic.",
    }],
  };
  const loaded = parseArtifact(encoder.encode(JSON.stringify(configuration)), "archbird.json");
  assert.equal(loaded.artifact, "project-configuration");
  const reviewed = reviewedProjectConfiguration(configuration);
  assert.equal(
    (reviewed.constraints as Record<string, Record<string, unknown>>)["ARCH-1"].assert,
    "acyclic",
  );
});

test("derived findings produce explicit expiring waiver candidates", () => {
  const fingerprint = "a".repeat(64);
  const result = {
    artifact: "verification",
    schema_version: 2,
    constraints: [{
      id: "ARCH-1",
      findings: [{
        comparison: "extra",
        fingerprint,
        key: "ui->storage",
        message: "forbidden edge",
      }],
    }],
  };
  const findings = verificationFindings(result);
  assert.equal(findings.length, 1);
  assert.deepEqual(waiverCandidate(findings[0], {
    owner: "architecture",
    rationale: "Remove after migration.",
    expiresOn: "2026-08-01",
  }), {
    expires_on: "2026-08-01",
    fingerprint,
    id: "WAIVE-AAAAAAAAAAAA",
    owner: "architecture",
    rationale: "Remove after migration.",
  });
  assert.throws(() => waiverCandidate(findings[0], {
    owner: "architecture",
    rationale: "Remove after migration.",
    expiresOn: "soon",
  }), /YYYY-MM-DD/);
});
