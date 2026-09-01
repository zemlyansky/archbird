export interface QueryRetrievalReason {
  field: string;
  match: string;
  term: string;
  value: string;
}

export interface QueryRetrievalHit {
  kind: string;
  line?: number;
  name: string;
  path?: string;
  reasons: QueryRetrievalReason[];
  score?: number;
  symbolKind?: string;
}

function object(value: unknown, label: string): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error(`${label} must be an object`);
  }
  return value as Record<string, unknown>;
}

function string(value: unknown, label: string): string {
  if (typeof value !== "string" || !value) throw new Error(`${label} must be a string`);
  return value;
}

export function queryRetrievalHits(document: Record<string, unknown>): QueryRetrievalHit[] {
  const query = object(document.query, "Query");
  const retrieval = object(query.retrieval, "Query retrieval");
  if (!Array.isArray(retrieval.hits)) throw new Error("Query retrieval hits must be an array");
  return retrieval.hits.map((value, index) => {
    const hit = object(value, `Query retrieval hit ${index}`);
    if (!Array.isArray(hit.reasons)) {
      throw new Error(`Query retrieval hit ${index} reasons must be an array`);
    }
    const line = hit.line;
    const score = hit.score;
    if (line !== undefined && (!Number.isSafeInteger(line) || Number(line) < 1)) {
      throw new Error(`Query retrieval hit ${index} line is invalid`);
    }
    if (score !== undefined && (!Number.isSafeInteger(score) || Number(score) < 0)) {
      throw new Error(`Query retrieval hit ${index} score is invalid`);
    }
    const kind = string(hit.kind, `Query retrieval hit ${index} kind`);
    const name = string(hit.name, `Query retrieval hit ${index} name`);
    const path = hit.path === undefined
      ? undefined
      : string(hit.path, `Query retrieval hit ${index} path`);
    if (kind !== "component" && path === undefined) {
      throw new Error(`Query retrieval hit ${index} path must be a string`);
    }
    return {
      kind,
      ...(line === undefined ? {} : { line: Number(line) }),
      name,
      ...(path === undefined ? {} : { path }),
      reasons: hit.reasons.map((reasonValue, reasonIndex) => {
        const reason = object(
          reasonValue,
          `Query retrieval hit ${index} reason ${reasonIndex}`,
        );
        return {
          field: string(reason.field, `Query retrieval reason ${reasonIndex} field`),
          match: string(reason.match, `Query retrieval reason ${reasonIndex} match`),
          term: string(reason.term, `Query retrieval reason ${reasonIndex} term`),
          value: string(reason.value, `Query retrieval reason ${reasonIndex} value`),
        };
      }),
      ...(score === undefined ? {} : { score: Number(score) }),
      ...(typeof hit.symbol_kind === "string" && hit.symbol_kind
        ? { symbolKind: hit.symbol_kind }
        : {}),
    };
  });
}
