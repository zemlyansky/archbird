export interface SymbolDocumentation {
  kind: string;
  span: { end: number; start: number } | null;
  text: string;
}

export function symbolDocumentation(value: unknown): SymbolDocumentation | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const row = value as Record<string, unknown>;
  if (typeof row.kind !== "string" || !row.kind
      || typeof row.text !== "string" || !row.text) return null;
  const span = row.span;
  if (span === undefined) return { kind: row.kind, span: null, text: row.text };
  if (!span || typeof span !== "object" || Array.isArray(span)) return null;
  const range = span as Record<string, unknown>;
  if (!Number.isSafeInteger(range.start) || !Number.isSafeInteger(range.end)
      || Number(range.start) < 0 || Number(range.end) <= Number(range.start)) return null;
  return {
    kind: row.kind,
    span: { end: Number(range.end), start: Number(range.start) },
    text: row.text,
  };
}

export function documentationPreview(value: unknown): string {
  const documentation = symbolDocumentation(value);
  if (!documentation) return "";
  return documentation.text.split(/\r?\n\s*\r?\n/, 1)[0]
    .replace(/\s+/g, " ")
    .trim();
}
