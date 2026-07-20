export interface VerificationFindingRow {
  check: string;
  comparison: string;
  fingerprint: string;
  key: string;
  message: string;
}

function record(value: unknown, label: string): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error(`${label} must be an object`);
  }
  return value as Record<string, unknown>;
}

function requiredString(value: unknown, label: string): string {
  if (typeof value !== "string" || !value.trim()) {
    throw new Error(`${label} must be non-empty`);
  }
  return value;
}

export function verificationFindings(
  document: Record<string, unknown>,
): VerificationFindingRow[] {
  if (document.artifact !== "verification" || !Array.isArray(document.checks)) {
    return [];
  }
  const result: VerificationFindingRow[] = [];
  for (const [checkIndex, rawCheck] of document.checks.entries()) {
    const check = record(rawCheck, `checks[${checkIndex}]`);
    const checkId = requiredString(check.id, `checks[${checkIndex}].id`);
    if (!Array.isArray(check.findings)) continue;
    for (const [findingIndex, rawFinding] of check.findings.entries()) {
      const finding = record(
        rawFinding,
        `checks[${checkIndex}].findings[${findingIndex}]`,
      );
      const fingerprint = requiredString(
        finding.fingerprint,
        `checks[${checkIndex}].findings[${findingIndex}].fingerprint`,
      );
      if (!/^[0-9a-f]{64}$/.test(fingerprint)) {
        throw new Error("verification finding fingerprint must be lowercase SHA-256");
      }
      result.push({
        check: checkId,
        comparison: requiredString(finding.comparison, "finding.comparison"),
        fingerprint,
        key: requiredString(finding.key, "finding.key"),
        message: typeof finding.message === "string" ? finding.message : "",
      });
    }
  }
  return result;
}

export function candidateSuite(
  document: Record<string, unknown>,
): Record<string, unknown> {
  if (document.schema_version !== 1 || typeof document.suite !== "string" ||
      !Array.isArray(document.checks) || !document.projects) {
    throw new Error("expected a schema-1 verification suite");
  }
  const result = structuredClone(document);
  result.candidate = true;
  for (const [index, rawCheck] of (result.checks as unknown[]).entries()) {
    const check = record(rawCheck, `checks[${index}]`);
    requiredString(check.id, `checks[${index}].id`);
    requiredString(check.owner, `checks[${index}].owner`);
    requiredString(check.rationale, `checks[${index}].rationale`);
    if (check.requirements !== undefined &&
        (!Array.isArray(check.requirements) ||
         check.requirements.some((item) => typeof item !== "string" || !item))) {
      throw new Error(`checks[${index}].requirements must contain non-empty IDs`);
    }
  }
  return result;
}

export function waiverCandidate(
  finding: VerificationFindingRow,
  { owner, rationale, expiresOn }: {
    owner: string;
    rationale: string;
    expiresOn: string;
  },
): Record<string, unknown> {
  const normalizedOwner = requiredString(owner, "waiver owner");
  const normalizedRationale = requiredString(rationale, "waiver rationale");
  if (!/^\d{4}-\d{2}-\d{2}$/.test(expiresOn)) {
    throw new Error("waiver expiration must be YYYY-MM-DD");
  }
  return {
    expires_on: expiresOn,
    fingerprint: finding.fingerprint,
    id: `WAIVE-${finding.fingerprint.slice(0, 12).toUpperCase()}`,
    owner: normalizedOwner,
    rationale: normalizedRationale,
  };
}
