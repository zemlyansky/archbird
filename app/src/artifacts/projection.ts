export interface ProjectionCompleteness {
  classification: "complete" | "incomplete" | "unknown";
  exhaustive: boolean;
  truncated: boolean;
}

export interface ProjectionEvidence {
  [key: string]: unknown;
}

export interface ProjectionItem {
  attributes: Record<string, unknown>;
  evidence: ProjectionEvidence[];
  key: string;
  label: string;
  message: string;
  state: "current" | "stale" | "unknown";
  value: unknown;
}

export interface ProjectionResult {
  artifact: "projection-result";
  completeness: ProjectionCompleteness;
  definition: Record<string, unknown>;
  fact: {
    completeness: ProjectionCompleteness;
    items: ProjectionItem[];
    message: string;
    name: string;
    project: string;
    shape: "graph" | "relation" | "set" | "values";
    state: "current" | "stale" | "unknown";
  };
  id: string;
  schema_version: 1;
}

function object(value: unknown, label: string): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error(`${label} must be an object`);
  }
  return value as Record<string, unknown>;
}

function nonblank(value: unknown, label: string): string {
  if (typeof value !== "string" || !value.length) {
    throw new Error(`${label} must be a non-empty string`);
  }
  return value;
}

function completeness(value: unknown, label: string): ProjectionCompleteness {
  const row = object(value, label);
  if (!["complete", "incomplete", "unknown"].includes(String(row.classification))) {
    throw new Error(`${label}.classification is invalid`);
  }
  if (typeof row.exhaustive !== "boolean" || typeof row.truncated !== "boolean") {
    throw new Error(`${label} must declare exhaustive and truncated`);
  }
  return row as unknown as ProjectionCompleteness;
}

export function parseProjectionResult(
  bytes: Uint8Array,
  expectedSelect: string,
): ProjectionResult {
  let value: unknown;
  try {
    value = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  } catch (error) {
    throw new Error(`ProjectionResult is not valid UTF-8 JSON: ${(error as Error).message}`);
  }
  const document = object(value, "ProjectionResult");
  if (document.artifact !== "projection-result" || document.schema_version !== 1) {
    throw new Error("expected projection-result schema version 1");
  }
  const definition = object(document.definition, "ProjectionResult.definition");
  if (definition.select !== expectedSelect) {
    throw new Error(`expected ${expectedSelect} projection, received ${String(definition.select)}`);
  }
  nonblank(document.id, "ProjectionResult.id");
  const fact = object(document.fact, "ProjectionResult.fact");
  nonblank(fact.project, "ProjectionResult.fact.project");
  if (!Array.isArray(fact.items)) throw new Error("ProjectionResult.fact.items must be an array");
  const ids = new Set<string>();
  for (const [index, raw] of fact.items.entries()) {
    const item = object(raw, `ProjectionResult.fact.items[${index}]`);
    const key = nonblank(item.key, `ProjectionResult.fact.items[${index}].key`);
    if (ids.has(key)) throw new Error(`ProjectionResult contains duplicate item ${key}`);
    ids.add(key);
    nonblank(item.label, `ProjectionResult.fact.items[${index}].label`);
    object(item.attributes, `ProjectionResult.fact.items[${index}].attributes`);
    if (!Array.isArray(item.evidence)) {
      throw new Error(`ProjectionResult.fact.items[${index}].evidence must be an array`);
    }
    if (!["current", "stale", "unknown"].includes(String(item.state))) {
      throw new Error(`ProjectionResult.fact.items[${index}].state is invalid`);
    }
  }
  const outerCompleteness = completeness(document.completeness, "ProjectionResult.completeness");
  const factCompleteness = completeness(fact.completeness, "ProjectionResult.fact.completeness");
  if (
    outerCompleteness.classification !== factCompleteness.classification
    || outerCompleteness.exhaustive !== factCompleteness.exhaustive
    || outerCompleteness.truncated !== factCompleteness.truncated
  ) {
    throw new Error("ProjectionResult completeness summaries disagree");
  }
  return document as unknown as ProjectionResult;
}
