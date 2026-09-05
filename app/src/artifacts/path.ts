import type { Evidence, GraphEdge, GraphNode, GraphView } from "./model";

type JsonObject = Record<string, unknown>;

const encoder = new TextEncoder();

function object(value: unknown): JsonObject {
  return value && typeof value === "object" && !Array.isArray(value)
    ? value as JsonObject
    : {};
}

function objects(value: unknown): JsonObject[] {
  return Array.isArray(value)
    ? value.filter((row): row is JsonObject =>
      Boolean(row) && typeof row === "object" && !Array.isArray(row))
    : [];
}

function strings(value: unknown): string[] {
  return Array.isArray(value)
    ? value.filter((row): row is string => typeof row === "string" && row.length > 0)
    : [];
}

function utf8Compare(left: string, right: string): number {
  const a = encoder.encode(left);
  const b = encoder.encode(right);
  const length = Math.min(a.length, b.length);
  for (let index = 0; index < length; index += 1) {
    if (a[index] !== b[index]) return a[index] - b[index];
  }
  return a.length - b.length;
}

function canonicalIdentity(value: string): {
  kind: string;
  label: string;
  path: string | null;
} {
  if (value.startsWith("symbol:")) {
    try {
      const parts = JSON.parse(value.slice("symbol:".length));
      if (Array.isArray(parts) && parts.length === 2
          && parts.every((part) => typeof part === "string")) {
        return { kind: "symbol", label: parts[1], path: parts[0] };
      }
    } catch {
      // Retain the complete identity below for older Path artifacts.
    }
  }
  if (value.startsWith("file:")) {
    const path = value.slice("file:".length);
    return { kind: "file", label: path.split("/").at(-1) || path, path };
  }
  const separator = value.indexOf(":");
  const kind = separator > 0 ? value.slice(0, separator) : "entity";
  const label = separator > 0 ? value.slice(separator + 1) : value;
  return { kind, label, path: null };
}

function endpointCandidates(document: JsonObject, role: "source" | "target") {
  const endpoint = object(document[role]);
  return objects(endpoint.candidates).flatMap((candidate) => {
    const attributes = object(candidate.attributes);
    const identity = typeof attributes.id === "string" ? attributes.id : "";
    return identity ? [{ attributes, candidate, identity, role }] : [];
  });
}

function unresolvedEndpointNodes(
  document: JsonObject,
  role: "source" | "target",
): GraphNode[] {
  const endpoint = object(document[role]);
  if (objects(endpoint.candidates).length) return [];
  const request = object(document.request);
  const selector = object(request[role]);
  const patterns = strings(selector.patterns);
  return (patterns.length ? patterns : [role]).map((pattern, index) => ({
    attributes: {
      path_endpoint_roles: [role],
      path_endpoint_state: String(endpoint.state || "unresolved"),
      path_presentation_only: true,
    },
    evidence: [],
    id: `path-request:${role}:${index}`,
    identity: pattern,
    kind: "unresolved",
    label: pattern,
    parent: null,
  }));
}

export function pathStatusFromArtifact(document: JsonObject): {
  detail: string;
  outcome: string;
} {
  const outcome = String(document.outcome || "unknown");
  const reason = String(document.reason || "unknown");
  const classification = String(object(document.graph).classification || "unknown")
    .replaceAll("-", " ");
  const count = Array.isArray(document.paths) ? document.paths.length : 0;
  let result: string;
  if (outcome === "found") {
    result = `${count} witness${count === 1 ? "" : "es"}`;
  } else if (reason === "candidate-witnesses") {
    result = `${count} candidate witness${count === 1 ? "" : "es"}`;
  } else if (reason === "endpoint-unresolved") {
    result = "Endpoint unresolved";
  } else if (reason === "depth-frontier") {
    result = "Depth limit reached";
  } else if (reason === "path-limit") {
    result = "Path limit reached";
  } else if (outcome === "absent") {
    result = "No connection path";
  } else if (reason === "graph-incomplete") {
    result = "No connection witness";
  } else {
    result = reason.replaceAll("-", " ").replaceAll("_", " ");
  }
  return { detail: `${result} · graph ${classification}`, outcome };
}

/** Render canonical Path evidence as a focused graph without changing proof state. */
export function pathGraphFromArtifact(document: JsonObject): GraphView {
  if (document.artifact !== "path") throw new Error("expected a Path artifact");

  const nodes = new Map<string, GraphNode>();
  const metadata = new Map<string, JsonObject>();
  const endpointRoles = new Map<string, Set<string>>();
  const endpointStates = new Map<string, JsonObject>();

  for (const role of ["source", "target"] as const) {
    const endpointState = String(object(document[role]).state || "unresolved");
    for (const row of endpointCandidates(document, role)) {
      metadata.set(row.identity, row.candidate);
      const roles = endpointRoles.get(row.identity) || new Set<string>();
      roles.add(role);
      endpointRoles.set(row.identity, roles);
      endpointStates.set(row.identity, {
        ...endpointStates.get(row.identity),
        [role]: endpointState,
      });
    }
  }

  const ensureNode = (identity: string): GraphNode => {
    const current = nodes.get(identity);
    if (current) return current;
    const item = metadata.get(identity) || {};
    const itemAttributes = object(item.attributes);
    const parsed = canonicalIdentity(identity);
    const path = typeof itemAttributes.path === "string"
      ? itemAttributes.path
      : parsed.path;
    const label = typeof itemAttributes.symbol === "string"
      ? itemAttributes.symbol
      : typeof item.label === "string" && !item.label.startsWith("node:[")
        ? item.label
        : parsed.label;
    const node: GraphNode = {
      attributes: {
        ...itemAttributes,
        ...(path ? { path } : {}),
        path_endpoint_roles: [...(endpointRoles.get(identity) || [])].sort(utf8Compare),
        path_endpoint_state: endpointStates.get(identity) || {},
        path_graph_classification: String(object(document.graph).classification || "unknown"),
        path_outcome: String(document.outcome || "unknown"),
        path_reason: String(document.reason || "unknown"),
      },
      evidence: Array.isArray(item.evidence) ? item.evidence as Evidence[] : [],
      id: identity,
      identity,
      kind: typeof itemAttributes.entity_kind === "string"
        ? itemAttributes.entity_kind
        : parsed.kind,
      label,
      parent: null,
    };
    nodes.set(identity, node);
    return node;
  };

  for (const identity of metadata.keys()) ensureNode(identity);
  for (const role of ["source", "target"] as const) {
    for (const node of unresolvedEndpointNodes(document, role)) nodes.set(node.id, node);
  }

  const edges: GraphEdge[] = [];
  for (const [pathIndex, path] of objects(document.paths).entries()) {
    const identities = strings(path.nodes);
    identities.forEach(ensureNode);
    const steps = objects(path.steps);
    for (let stepIndex = 0; stepIndex + 1 < identities.length; stepIndex += 1) {
      const step = steps[stepIndex] || {};
      const relation = object(step.relation);
      const attributes = object(relation.attributes);
      const state = String(relation.state || path.state || "unknown");
      edges.push({
        attributes: {
          ...attributes,
          canonical_source: attributes.source,
          canonical_target: attributes.target,
          path_state: String(path.state || "unknown"),
          path_step: stepIndex,
          path_traversal: String(step.traversal || "unknown"),
          path_witness: pathIndex,
        },
        classification: state === "current" ? "direct" : "unresolved",
        evidence: Array.isArray(relation.evidence)
          ? relation.evidence as Evidence[]
          : [],
        id: `${String(relation.key || "path-relation")}:witness:${pathIndex}:step:${stepIndex}`,
        kind: String(attributes.relation_kind || attributes.family || "relation"),
        names: strings(attributes.names),
        omitted_names: 0,
        source: identities[stepIndex],
        target: identities[stepIndex + 1],
      });
    }
  }

  const orderedNodes = [...nodes.values()].sort((left, right) =>
    utf8Compare(left.id, right.id));
  edges.sort((left, right) => utf8Compare(left.id, right.id));
  const graph = object(document.graph);
  const search = object(document.search);
  return {
    artifact: "archbird-graph-view",
    diagnostics: [{
      classification: String(graph.classification || "unknown"),
      message: edges.length
        ? `${edges.length} connection witness relation${edges.length === 1 ? "" : "s"}`
        : "No connection witness is established",
      outcome: String(document.outcome || "unknown"),
      reason: String(document.reason || "unknown"),
    }],
    edges,
    nodes: orderedNodes,
    omissions: search.truncated === true
      ? [{ kind: "paths", reason: "path-limit" }]
      : [],
    project: String(document.project || "project"),
    request: {
      max_edge_names: 0,
      max_nodes: 0,
      query: {
        graph_classification: String(graph.classification || "unknown"),
        outcome: String(document.outcome || "unknown"),
        reason: String(document.reason || "unknown"),
        view: "path",
      },
      view: "symbols",
    },
    schema_version: 1,
    source: {
      artifact: "path",
      path_sha256: document.path_sha256,
    },
    summary: { edges: edges.length, nodes: orderedNodes.length },
    tool: object(document.source_tool),
  };
}
