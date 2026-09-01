export interface SourceSelection {
  end?: number;
  path: string;
  start?: number;
}

export interface SourceSegments {
  after: string;
  before: string;
  selected: string;
}

export function sourceSegments(
  source: Record<string, unknown> | null,
  selection: SourceSelection | null,
): SourceSegments | null {
  if (!source || !selection || source.path !== selection.path
      || source.encoding !== "utf-8" || typeof source.text !== "string"
      || !Number.isSafeInteger(selection.start) || !Number.isSafeInteger(selection.end)
      || Number(selection.start) < 0 || Number(selection.end) <= Number(selection.start)) return null;
  const encoded = new TextEncoder().encode(source.text);
  const start = Number(selection.start);
  const end = Number(selection.end);
  if (end > encoded.byteLength) return null;
  const decoder = new TextDecoder("utf-8", { fatal: true });
  try {
    return {
      after: decoder.decode(encoded.subarray(end)),
      before: decoder.decode(encoded.subarray(0, start)),
      selected: decoder.decode(encoded.subarray(start, end)),
    };
  } catch {
    return null;
  }
}
