import type { GraphNode } from "../artifacts/model";

export type GraphNotation = "architecture" | "compact";

export interface ArchitectureCardPalette {
  accent: string;
  danger?: string;
  success?: string;
  text: string;
  warning?: string;
}

export interface ArchitectureCardSize {
  height: number;
  width: number;
}

const stereotypes: Readonly<Record<string, string>> = {
  build: "build",
  builtin: "runtime",
  component: "component",
  directory: "directory",
  external: "external",
  file: "file",
  language: "language",
  layer: "layer",
  inventory: "inventory",
  package: "package",
  symbol: "symbol",
  unknown: "unknown",
  unresolved: "unresolved",
};

function architectureNodeType(node: GraphNode): string {
  const symbolKind = node.kind === "symbol" && typeof node.attributes.kind === "string"
    ? node.attributes.kind.trim()
    : "";
  return /^[A-Za-z][A-Za-z0-9_-]{0,31}$/.test(symbolKind)
    ? symbolKind
    : stereotypes[node.kind] || "entity";
}

function xml(value: string): string {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&apos;");
}

function boundedText(value: string, limit = 25): string {
  const normalized = value.replace(/\s+/g, " ").trim();
  const points = Array.from(normalized);
  return points.length <= limit
    ? normalized
    : `${points.slice(0, Math.max(0, limit - 1)).join("")}…`;
}

function boundedFilename(value: string, limit: number): string {
  const points = Array.from(value);
  if (points.length <= limit) return value;
  const dot = value.lastIndexOf(".");
  const extension = dot > 0 ? value.slice(dot) : "";
  const extensionPoints = Array.from(extension);
  if (extensionPoints.length && extensionPoints.length < limit - 2) {
    const stem = Array.from(value.slice(0, dot));
    return `${stem.slice(0, limit - extensionPoints.length - 1).join("")}…${extension}`;
  }
  return boundedText(value, limit);
}

export function architectureNodeDisplayLabel(
  node: GraphNode,
  limit = 25,
): string {
  if (node.kind !== "file") return boundedText(node.label, limit);
  const configuredPath = typeof node.attributes.canonical_path === "string"
    ? node.attributes.canonical_path
    : typeof node.attributes.path === "string"
      ? node.attributes.path
      : node.identity;
  const path = configuredPath.replace(/^file:/, "").replace(/^\/+|\/+$/g, "");
  if (Array.from(path).length <= limit) return path;
  const segments = path.split("/").filter(Boolean);
  const filename = boundedFilename(segments.pop() || node.label, limit);
  if (Array.from(filename).length >= limit) return filename;
  let tail = filename;
  while (segments.length) {
    const candidate = `${segments.pop()}/${tail}`;
    if (Array.from(candidate).length + 2 > limit) break;
    tail = candidate;
  }
  return tail === filename ? filename : `…/${tail}`;
}

export function architectureNodeLabel(node: GraphNode): string {
  return `«${architectureNodeType(node)}»\n${node.label}`;
}

/** Render separate type and identity typography without a graph-label plugin. */
export function architectureNodeCard(
  node: GraphNode,
  palette: ArchitectureCardPalette,
  size: ArchitectureCardSize,
): string {
  const type = xml(architectureNodeType(node).toUpperCase());
  const label = xml(architectureNodeDisplayLabel(node));
  const status = String(node.attributes.verification_status || "");
  const statusAccent = status === "pass"
    ? palette.success
    : status === "unknown"
      ? palette.warning
      : status === "fail"
        ? palette.danger
        : undefined;
  const accent = xml(statusAccent || palette.accent);
  const text = xml(palette.text);
  const width = Math.max(1, Math.round(size.width));
  const height = Math.max(1, Math.round(size.height));
  const headerHeight = Math.max(10, Math.round(height * 0.32));
  const typeSize = Math.max(5, Math.round(height * 0.17));
  const typeY = Math.round(headerHeight * 0.62);
  const labelSize = Math.max(9, Math.round(height * 0.32));
  const inset = Math.max(5, Math.round(width * 0.045));
  const labelY = headerHeight + Math.round((height - headerHeight) / 2);
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" `
    + `viewBox="0 0 ${width} ${height}" preserveAspectRatio="none" overflow="hidden">`
    + `<rect width="${width}" height="${headerHeight}" fill="${accent}" fill-opacity=".14"/>`
    + `<text x="${inset}" y="${typeY}" fill="${accent}" font-family="IBM Plex Mono,monospace" `
    + `font-size="${typeSize}" font-weight="400" letter-spacing=".8" `
    + `dominant-baseline="middle">${type}</text>`
    + `<text x="${inset}" y="${labelY}" fill="${text}" font-family="Inter,Arial,sans-serif" `
    + `font-size="${labelSize}" font-weight="600" `
    + `dominant-baseline="middle">${label}</text>`
    + "</svg>";
  return `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}`;
}
