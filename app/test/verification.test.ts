import assert from "node:assert/strict";
import test from "node:test";

import { parseArtifact } from "../src/artifacts/model";
import {
  candidateSuite,
  verificationFindings,
  waiverCandidate,
} from "../src/artifacts/verification";

const encoder = new TextEncoder();

test("verification suites load as editable asserted documents", () => {
  const suite = {
    schema_version: 1,
    suite: "demo",
    projects: { subject: { config: "archbird.json" } },
    extractors: {},
    checks: [{
      id: "ARCH-1",
      assert: "acyclic",
      actual: "actual.edges",
      owner: "architecture",
      rationale: "Keep dependencies acyclic.",
    }],
  };
  const loaded = parseArtifact(encoder.encode(JSON.stringify(suite)), "demo.verify.json");
  assert.equal(loaded.artifact, "verification-suite");
  const candidate = candidateSuite(suite);
  assert.equal(candidate.candidate, true);
  assert.equal((candidate.checks as Record<string, unknown>[])[0].id, "ARCH-1");
});

test("derived findings produce explicit expiring waiver candidates", () => {
  const fingerprint = "a".repeat(64);
  const result = {
    artifact: "verification",
    checks: [{
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
