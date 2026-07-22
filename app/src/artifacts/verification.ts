export interface VerificationFindingRow {
  constraint: string;
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
  if (document.artifact !== "verification" ||
      document.schema_version !== 2 ||
      !Array.isArray(document.constraints)) {
    return [];
  }
  const result: VerificationFindingRow[] = [];
  for (const [constraintIndex, rawConstraint] of document.constraints.entries()) {
    const constraint = record(rawConstraint, `constraints[${constraintIndex}]`);
    const constraintId = requiredString(
      constraint.id,
      `constraints[${constraintIndex}].id`,
    );
    if (!Array.isArray(constraint.findings)) continue;
    for (const [findingIndex, rawFinding] of constraint.findings.entries()) {
      const finding = record(
        rawFinding,
        `constraints[${constraintIndex}].findings[${findingIndex}]`,
      );
      const fingerprint = requiredString(
        finding.fingerprint,
        `constraints[${constraintIndex}].findings[${findingIndex}].fingerprint`,
      );
      if (!/^[0-9a-f]{64}$/.test(fingerprint)) {
        throw new Error("verification finding fingerprint must be lowercase SHA-256");
      }
      result.push({
        constraint: constraintId,
        comparison: requiredString(finding.comparison, "finding.comparison"),
        fingerprint,
        key: requiredString(finding.key, "finding.key"),
        message: typeof finding.message === "string" ? finding.message : "",
      });
    }
  }
  return result;
}

export function reviewedProjectConfiguration(
  document: Record<string, unknown>,
): Record<string, unknown> {
  if (document.schema_version !== 2 || typeof document.project !== "string" ||
      !Array.isArray(document.layers)) {
    throw new Error("expected a schema-2 Archbird project configuration");
  }
  const result = structuredClone(document);
  const rawConstraints = result.constraints ?? {};
  const rows: [string, Record<string, unknown>][] = [];
  if (Array.isArray(rawConstraints)) {
    for (const [index, rawConstraint] of rawConstraints.entries()) {
      const constraint = record(rawConstraint, `constraints[${index}]`);
      rows.push([
        requiredString(constraint.id, `constraints[${index}].id`),
        constraint,
      ]);
    }
  } else {
    const constraints = record(rawConstraints, "constraints");
    for (const [id, rawConstraint] of Object.entries(constraints)) {
      rows.push([requiredString(id, "constraint ID"), record(rawConstraint, `constraints.${id}`)]);
    }
  }
  const normalized: Record<string, unknown> = {};
  for (const [id, constraint] of rows.sort(([left], [right]) => left.localeCompare(right))) {
    if (normalized[id]) throw new Error(`duplicate constraint ID ${id}`);
    requiredString(constraint.owner, `constraints.${id}.owner`);
    requiredString(constraint.rationale, `constraints.${id}.rationale`);
    if (constraint.requirement !== undefined) {
      const requirements = Array.isArray(constraint.requirement)
        ? constraint.requirement
        : [constraint.requirement];
      if (requirements.some((item) => typeof item !== "string" || !item)) {
        throw new Error(`constraints.${id}.requirement must contain non-empty IDs`);
      }
    }
    const value = structuredClone(constraint);
    delete value.id;
    normalized[id] = value;
  }
  result.constraints = normalized;
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
