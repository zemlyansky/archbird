export type GraphViewName = "components" | "files" | "symbols";

export interface Evidence {
  [key: string]: unknown;
}

export interface GraphNode {
  id: string;
  identity: string;
  kind: string;
  label: string;
  parent: string | null;
  attributes: Record<string, unknown>;
  evidence: Evidence[];
}

export interface GraphEdge {
  id: string;
  source: string;
  target: string;
  kind: string;
  classification: string;
  names: string[];
  omitted_names: number;
  evidence: Evidence[];
}

export interface GraphView {
  artifact: "archbird-graph-view";
  schema_version: 1;
  project: string;
  source: Record<string, unknown>;
  tool: Record<string, unknown>;
  request: {
    view: GraphViewName;
    max_nodes: number;
    max_edge_names: number;
    query?: Record<string, unknown>;
  };
  nodes: GraphNode[];
  edges: GraphEdge[];
  summary: { nodes: number; edges: number };
  omissions: Record<string, unknown>[];
  diagnostics: Record<string, unknown>[];
}

export interface ParsedArtifact {
  artifact: string;
  document: Record<string, unknown>;
  bytes: Uint8Array;
  name: string;
}

function object(value: unknown, label: string): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error(`${label} must be a JSON object`);
  }
  return value as Record<string, unknown>;
}

function string(value: unknown, label: string): string {
  if (typeof value !== "string" || value.length === 0) {
    throw new Error(`${label} must be a non-empty string`);
  }
  return value;
}

export function parseArtifact(bytes: Uint8Array, name: string): ParsedArtifact {
  let value: unknown;
  try {
    value = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  } catch (error) {
    throw new Error(`${name} is not valid UTF-8 JSON: ${(error as Error).message}`);
  }
  const document = object(value, name);
  const artifact = typeof document.artifact === "string" && document.artifact.length
    ? document.artifact
    : document.schema_version === 2
      && typeof document.project === "string"
      && Array.isArray(document.layers)
      ? "project-configuration"
      : string(document.artifact, `${name}.artifact`);
  return { artifact, bytes, document, name };
}

export function parseGraphView(value: unknown): GraphView {
  const document = object(value, "graph view");
  if (document.artifact !== "archbird-graph-view" || document.schema_version !== 1) {
    throw new Error("expected archbird-graph-view schema version 1");
  }
  string(document.project, "graph view project");
  if (!Array.isArray(document.nodes) || !Array.isArray(document.edges)) {
    throw new Error("graph view nodes and edges must be arrays");
  }
  const ids = new Set<string>();
  for (const [index, raw] of document.nodes.entries()) {
    const node = object(raw, `nodes[${index}]`);
    const id = string(node.id, `nodes[${index}].id`);
    if (ids.has(id)) throw new Error(`duplicate graph node id ${id}`);
    ids.add(id);
    string(node.identity, `nodes[${index}].identity`);
    string(node.kind, `nodes[${index}].kind`);
    string(node.label, `nodes[${index}].label`);
  }
  for (const [index, raw] of document.edges.entries()) {
    const edge = object(raw, `edges[${index}]`);
    string(edge.id, `edges[${index}].id`);
    const source = string(edge.source, `edges[${index}].source`);
    const target = string(edge.target, `edges[${index}].target`);
    if (!ids.has(source) || !ids.has(target)) {
      throw new Error(`edges[${index}] references an unknown node`);
    }
  }
  return document as unknown as GraphView;
}

export function graphViewFromArtifact(artifact: ParsedArtifact): GraphView | null {
  return artifact.artifact === "archbird-graph-view"
    ? parseGraphView(artifact.document)
    : null;
}

export function supportedViews(artifact: string): GraphViewName[] {
  if (artifact === "map") return ["components", "files"];
  if (artifact === "query") return ["symbols"];
  if (artifact === "archbird-graph-view") return [];
  return [];
}
