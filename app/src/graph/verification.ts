import type { GraphEdge, GraphNode, GraphView } from "../artifacts/model";

export interface VerificationFinding {
  evidence: Array<Record<string, unknown>>;
  fingerprint: string;
  key: string;
  message: string;
}

export interface VerificationConstraint {
  assert: string;
  coverage: string[];
  findings: VerificationFinding[];
  id: string;
  status: string;
}

export interface VerificationOverlay {
  constraints: VerificationConstraint[];
  graph: GraphView;
  unmappedFindings: VerificationFinding[];
}

const statusWeight: Record<string, number> = {
  fail: 5,
  unknown: 4,
  waived: 3,
  pass: 2,
  not_applicable: 1,
};

function object(value: unknown): Record<string, unknown> | null {
  return value && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

function strings(value: unknown): string[] {
  return Array.isArray(value)
    ? value.filter((item): item is string => typeof item === "string")
    : [];
}

function finding(value: unknown): VerificationFinding | null {
  const row = object(value);
  if (!row || typeof row.message !== "string") return null;
  return {
    evidence: Array.isArray(row.evidence)
      ? row.evidence.flatMap((entry) => {
        const evidence = object(entry);
        return evidence ? [evidence] : [];
      })
      : [],
    fingerprint: typeof row.fingerprint === "string" ? row.fingerprint : "",
    key: typeof row.key === "string" ? row.key : "",
    message: row.message,
  };
}

export function verificationConstraints(
  document: Record<string, unknown> | null,
): VerificationConstraint[] {
  if (!document || document.artifact !== "verification" || !Array.isArray(document.constraints)) {
    return [];
  }
  return document.constraints.flatMap((value) => {
    const row = object(value);
    if (!row || typeof row.id !== "string") return [];
    return [{
      assert: typeof row.assert === "string" ? row.assert : "",
      coverage: strings(row.coverage),
      findings: Array.isArray(row.findings)
        ? row.findings.flatMap((entry) => {
          const parsed = finding(entry);
          return parsed ? [parsed] : [];
        })
        : [],
      id: row.id,
      status: typeof row.status === "string" ? row.status : "unknown",
    }];
  });
}

function relation(value: string): { kind: string; source: string; target: string } | null {
  if (!value.startsWith("{")) return null;
  try {
    const row = object(JSON.parse(value));
    if (
      !row || typeof row.kind !== "string"
      || typeof row.source !== "string" || typeof row.target !== "string"
    ) return null;
    return { kind: row.kind, source: row.source, target: row.target };
  } catch {
    return null;
  }
}

function parentComponent(node: GraphNode, nodes: ReadonlyMap<string, GraphNode>): string | null {
  let current: GraphNode | undefined = node;
  while (current) {
    if (current.kind === "component") return current.label;
    if (typeof current.attributes.component === "string") {
      return current.attributes.component;
    }
    current = current.parent ? nodes.get(current.parent) : undefined;
  }
  return null;
}

function nodeMatchesPath(node: GraphNode, path: string): boolean {
  if (node.identity === path || node.attributes.canonical_path === path) return true;
  const members = node.attributes.member_files;
  return Array.isArray(members) && members.includes(path);
}

function mergeStatus(current: unknown, candidate: string): string {
  const existing = typeof current === "string" ? current : "";
  return (statusWeight[candidate] || 0) > (statusWeight[existing] || 0)
    ? candidate
    : existing;
}

function annotate(
  attributes: Record<string, unknown> | undefined,
  constraint: VerificationConstraint,
  findingCount: number,
): Record<string, unknown> {
  const rows = Array.isArray(attributes?.verification_constraints)
    ? attributes.verification_constraints.filter((id): id is string => typeof id === "string")
    : [];
  if (!rows.includes(constraint.id)) rows.push(constraint.id);
  return {
    ...attributes,
    verification_constraints: rows,
    verification_covered: true,
    verification_findings: Number(attributes?.verification_findings || 0) + findingCount,
    verification_status: mergeStatus(attributes?.verification_status, constraint.status),
  };
}

function matchingNodes(
  graph: GraphView,
  key: string,
  evidence: Array<Record<string, unknown>>,
): GraphNode[] {
  const paths = evidence.flatMap((row) =>
    typeof row.path === "string" && row.path ? [row.path] : []);
  if (paths.length) {
    return graph.nodes.filter((node) => paths.some((path) => nodeMatchesPath(node, path)));
  }
  if (!key) return [];
  return graph.nodes.filter((node) =>
    node.kind === "symbol" && (node.label === key || node.identity === key));
}

function matchingEdges(
  graph: GraphView,
  nodes: ReadonlyMap<string, GraphNode>,
  value: string,
): GraphEdge[] {
  const parsed = relation(value);
  if (!parsed) return [];
  return graph.edges.filter((edge) => {
    const source = nodes.get(edge.source);
    const target = nodes.get(edge.target);
    return source && target && edge.kind === parsed.kind
      && parentComponent(source, nodes) === parsed.source
      && parentComponent(target, nodes) === parsed.target;
  });
}

export function applyVerificationOverlay(
  graph: GraphView,
  document: Record<string, unknown> | null,
  selectedConstraintId: string | null = null,
): VerificationOverlay {
  const constraints = verificationConstraints(document);
  const active = selectedConstraintId
    ? constraints.filter((constraint) => constraint.id === selectedConstraintId)
    : constraints;
  const nodes = new Map(graph.nodes.map((node) => [
    node.id,
    { ...node, attributes: { ...node.attributes } },
  ]));
  const edges = new Map(graph.edges.map((edge) => [
    edge.id,
    { ...edge, attributes: { ...edge.attributes } },
  ]));
  const nodeIndex = new Map(nodes);
  const unmappedFindings: VerificationFinding[] = [];

  for (const constraint of active) {
    for (const coverage of constraint.coverage) {
      const relationEdges = matchingEdges(graph, nodeIndex, coverage);
      if (relationEdges.length) {
        for (const edge of relationEdges) {
          const target = edges.get(edge.id);
          if (target) target.attributes = annotate(target.attributes, constraint, 0);
        }
        continue;
      }
      for (const node of nodes.values()) {
        if (nodeMatchesPath(node, coverage)) {
          node.attributes = annotate(node.attributes, constraint, 0);
        }
      }
    }
    for (const row of constraint.findings) {
      const relationEdges = matchingEdges(graph, nodeIndex, row.key);
      const findingNodes = matchingNodes(graph, row.key, row.evidence);
      if (!relationEdges.length && !findingNodes.length) {
        unmappedFindings.push(row);
        continue;
      }
      for (const edge of relationEdges) {
        const target = edges.get(edge.id);
        if (target) target.attributes = annotate(target.attributes, constraint, 1);
      }
      for (const node of findingNodes) {
        const target = nodes.get(node.id);
        if (target) target.attributes = annotate(target.attributes, constraint, 1);
      }
    }
  }

  return {
    constraints,
    graph: {
      ...graph,
      edges: [...edges.values()],
      nodes: [...nodes.values()],
    },
    unmappedFindings,
  };
}
