import type {
  GraphEdge,
  GraphNode,
  GraphView,
} from "../artifacts/model";
import type {
  ProjectionItem,
  ProjectionResult,
} from "../artifacts/projection";

export type GraphGrouping = "component" | "directory" | "language" | "layer";
export type MapView = "architecture" | "evidence" | "overview" | "tests";

export interface GraphPresentation {
  files: GraphView;
  grouping: GraphGrouping;
  hasConfiguredComponents: boolean;
  overview: GraphView;
  view: MapView;
}

export interface MapGraphProjectionPlan extends Record<string, unknown> {
  group_by: GraphGrouping;
  id: string;
  level: "file";
  overlays: string[];
  relations: string[];
  select: "graph";
}

const VIEW_RELATIONS: Readonly<Record<MapView, readonly string[]>> = {
  architecture: [
    "bridges",
    "calls",
    "declarations",
    "imports",
    "packages",
    "references",
  ],
  evidence: [],
  overview: ["bridges", "builds", "imports", "packages", "tests"],
  tests: ["tests"],
};

export function mapGraphProjectionPlan(
  view: MapView,
  grouping: GraphGrouping,
): MapGraphProjectionPlan {
  return {
    group_by: grouping,
    id: `app-${view}-${grouping}`,
    level: "file",
    overlays: ["diagnostics", "evidence-quality"],
    relations: [...VIEW_RELATIONS[view]],
    select: "graph",
  };
}

const textEncoder = new TextEncoder();

function utf8Compare(left: string, right: string): number {
  const a = textEncoder.encode(left);
  const b = textEncoder.encode(right);
  const common = Math.min(a.length, b.length);
  for (let index = 0; index < common; index += 1) {
    if (a[index] !== b[index]) return a[index] - b[index];
  }
  return a.length - b.length;
}

function presentationId(kind: string, ...segments: string[]): string {
  return `presentation:${kind}:${segments.map(encodeURIComponent).join(":")}`;
}

function strings(value: unknown): string[] {
  return Array.isArray(value)
    ? value.filter((item): item is string => typeof item === "string")
    : [];
}

function nonblank(value: unknown, label: string): string {
  if (typeof value !== "string" || !value.length) {
    throw new Error(`${label} must be a non-empty string`);
  }
  return value;
}

function recordKind(item: ProjectionItem): string {
  return nonblank(item.attributes.record_kind, `${item.key}.record_kind`);
}

function classification(state: string): string {
  return state === "current" ? "direct" : state;
}

function canonicalNode(item: ProjectionItem): GraphNode {
  const id = nonblank(item.attributes.id, `${item.key}.id`);
  const kind = nonblank(item.attributes.entity_kind, `${item.key}.entity_kind`);
  const path = typeof item.attributes.path === "string" ? item.attributes.path : null;
  return {
    attributes: {
      ...item.attributes,
      presentation_kind: kind,
      state: item.state,
    },
    evidence: [...item.evidence],
    id,
    identity: path || item.label,
    kind,
    label: item.label,
    parent: null,
  };
}

function canonicalEdge(item: ProjectionItem): GraphEdge {
  const source = nonblank(item.attributes.source, `${item.key}.source`);
  const target = nonblank(item.attributes.target, `${item.key}.target`);
  const kind = nonblank(
    item.attributes.relation_kind,
    `${item.key}.relation_kind`,
  );
  return {
    attributes: {
      ...item.attributes,
      projection_state: item.state,
    },
    classification: classification(item.state),
    evidence: [...item.evidence],
    id: item.key,
    kind,
    names: strings(item.attributes.names),
    omitted_names: 0,
    source,
    target,
  };
}

function graphBase(
  projection: ProjectionResult,
  view: "components" | "files",
): Omit<GraphView, "edges" | "nodes" | "summary"> {
  const diagnostics: Record<string, unknown>[] = projection.fact.items
    .filter((item) => ["coverage", "diagnostic"].includes(recordKind(item)))
    .map((item) => ({
      ...item.attributes,
      message: item.message || item.label,
      state: item.state,
    }));
  if (!projection.completeness.exhaustive
    || projection.completeness.classification !== "complete") {
    diagnostics.push({
      classification: projection.completeness.classification,
      message: "graph projection contains an explicit evidence frontier",
      projection: projection.id,
    });
  }
  return {
    artifact: "archbird-graph-view",
    diagnostics,
    omissions: projection.fact.items
      .filter((item) => recordKind(item) === "ledger")
      .map((item) => ({ ...item.attributes })),
    project: projection.fact.project,
    request: {
      max_edge_names: 0,
      max_nodes: 0,
      query: { projection: projection.definition },
      view,
    },
    schema_version: 1,
    source: {
      projection_definition_sha256:
        (projection as unknown as Record<string, unknown>)
          .projection_definition_sha256,
      projection_result_sha256:
        (projection as unknown as Record<string, unknown>)
          .projection_result_sha256,
    },
    tool: { name: "archbird-app", operation: "projection-presentation" },
  };
}

function groupingFor(projection: ProjectionResult): GraphGrouping {
  const group = projection.definition.group_by;
  if (
    group === "component"
    || group === "directory"
    || group === "language"
    || group === "layer"
  ) {
    return group;
  }
  throw new Error(`unsupported graph grouping ${String(group)}`);
}

function groupNode(
  item: ProjectionItem,
  members: GraphNode[],
): GraphNode {
  const id = nonblank(item.attributes.id, `${item.key}.id`);
  const groupBy = nonblank(item.attributes.group_by, `${item.key}.group_by`);
  const origin = nonblank(item.attributes.origin, `${item.key}.origin`);
  const paths = members.flatMap((node) =>
    typeof node.attributes.path === "string" ? [node.attributes.path] : []);
  const languages = new Set(members.flatMap((node) =>
    typeof node.attributes.language === "string" ? [node.attributes.language] : []));
  const inventory = groupBy === "inventory";
  const unassigned = groupBy === "component" && origin === "unassigned";
  const root = groupBy === "directory" && item.label === ".";
  const kind = inventory
    ? nonblank(item.attributes.inventory_kind, `${item.key}.inventory_kind`)
    : unassigned
    ? (item.label === "." ? "root" : "directory")
    : (root ? "root" : groupBy);
  const label = inventory
    ? item.label
    : unassigned
    ? (item.label === "."
      ? "Repository root · unassigned"
      : `${item.label}/ · unassigned`)
    : (root ? "Repository root" : groupBy === "directory"
      ? `${item.label}/`
      : item.label);
  return {
    attributes: {
      ...item.attributes,
      configured: origin === "configured",
      expanded: false,
      files: paths.length,
      languages: [...languages].sort(utf8Compare),
      member_files: paths.sort(utf8Compare),
      member_node_ids: inventory
        ? members.map((node) => node.id).sort(utf8Compare)
        : [],
      origin: origin === "configured" ? "configured" : "inferred",
      presentation_kind: kind,
      presentation_role: "group",
      root_path: groupBy === "directory"
        ? (item.label === "." ? "" : item.label)
        : null,
    },
    evidence: [...item.evidence],
    id: presentationId("group", id),
    identity: `${groupBy}:${item.label}`,
    kind,
    label,
    parent: null,
  };
}

// Compatibility for schema-1 ProjectionResults saved before the core emitted
// explicit inventory groups and memberships.
function peripheralGroupNode(kind: string, members: GraphNode[]): GraphNode {
  const labels: Readonly<Record<string, string>> = {
    build: "Builds",
    builtin: "Built-ins",
    external: "External",
    package: "Packages",
    unresolved: "Unresolved",
  };
  const id = presentationId("peripheral-group", kind);
  return {
    attributes: {
      configured: false,
      expanded: false,
      files: 0,
      member_count: members.length,
      member_files: [],
      member_node_ids: members.map((node) => node.id).sort(utf8Compare),
      origin: "derived",
      presentation_kind: kind,
      presentation_role: "group",
      root_path: null,
    },
    evidence: members.flatMap((node) => node.evidence),
    id,
    identity: `peripheral:${kind}`,
    kind,
    label: labels[kind] || kind,
    parent: null,
  };
}

interface EdgeAggregation {
  canonicalIds: Set<string>;
  edge: GraphEdge;
  names: Set<string>;
}

function aggregateEdge(
  output: Map<string, EdgeAggregation>,
  edge: GraphEdge,
  source: string,
  target: string,
): void {
  if (source === target) return;
  const key = JSON.stringify([edge.kind, source, target, edge.classification]);
  const existing = output.get(key);
  if (existing) {
    for (const name of edge.names) existing.names.add(name);
    existing.canonicalIds.add(edge.id);
    existing.edge.evidence.push(...edge.evidence);
    existing.edge.attributes = {
      ...existing.edge.attributes,
      witness_count: Number(existing.edge.attributes?.witness_count || 0)
        + Number(edge.attributes?.witness_count || 1),
    };
    return;
  }
  output.set(key, {
    canonicalIds: new Set([edge.id]),
    edge: {
      ...edge,
      attributes: {
        ...edge.attributes,
        canonical_edge_ids: [edge.id],
        presentation_aggregate: true,
      },
      evidence: [...edge.evidence],
      id: presentationId("group-edge", key),
      names: [...edge.names],
      source,
      target,
    },
    names: new Set(edge.names),
  });
}

function finishAggregatedEdge(aggregation: EdgeAggregation): GraphEdge {
  return {
    ...aggregation.edge,
    attributes: {
      ...aggregation.edge.attributes,
      canonical_edge_ids: [...aggregation.canonicalIds],
    },
    names: [...aggregation.names].sort(utf8Compare),
  };
}

export function presentGraphProjection(
  projection: ProjectionResult,
  view: MapView,
): GraphPresentation {
  if (projection.fact.shape !== "graph") {
    throw new Error("graph presentation requires a graph ProjectionResult");
  }
  if (projection.definition.level !== "file") {
    throw new Error("graph presentation requires a file-level graph projection");
  }
  const grouping = groupingFor(projection);
  const nodes = projection.fact.items
    .filter((item) => recordKind(item) === "node")
    .map(canonicalNode);
  const nodeById = new Map(nodes.map((node) => [node.id, node]));
  const edges = projection.fact.items
    .filter((item) => recordKind(item) === "relation")
    .map(canonicalEdge);
  for (const edge of edges) {
    if (!nodeById.has(edge.source) || !nodeById.has(edge.target)) {
      throw new Error(`graph projection relation ${edge.id} has an unknown endpoint`);
    }
  }

  const groupItems = projection.fact.items
    .filter((item) => recordKind(item) === "group");
  const groupItemById = new Map(groupItems.map((item) => [
    nonblank(item.attributes.id, `${item.key}.id`),
    item,
  ]));
  if (groupItemById.size !== groupItems.length) {
    throw new Error("graph projection contains duplicate group identities");
  }
  const groupMembers = new Map<string, GraphNode[]>();
  const groupMemberIds = new Map<string, Set<string>>();
  const nodeGroups = new Map<string, string[]>();
  for (const item of projection.fact.items
    .filter((candidate) => recordKind(candidate) === "membership")) {
    const group = nonblank(item.attributes.group, `${item.key}.group`);
    const node = nonblank(item.attributes.node, `${item.key}.node`);
    if (!groupItemById.has(group)) {
      throw new Error(`graph projection membership ${item.key} has an unknown group`);
    }
    const member = nodeById.get(node);
    if (!member) {
      throw new Error(`graph projection membership ${item.key} has an unknown node`);
    }
    const memberIds = groupMemberIds.get(group) || new Set<string>();
    const members = groupMembers.get(group) || [];
    if (memberIds.has(node)) {
      throw new Error(`graph projection repeats membership ${group} -> ${node}`);
    }
    memberIds.add(node);
    members.push(member);
    groupMemberIds.set(group, memberIds);
    groupMembers.set(group, members);
  }
  const groups = groupItems.map((item) => {
    const id = nonblank(item.attributes.id, `${item.key}.id`);
    const node = groupNode(item, groupMembers.get(id) || []);
    for (const member of groupMembers.get(id) || []) {
      const owners = nodeGroups.get(member.id) || [];
      owners.push(node.id);
      nodeGroups.set(member.id, owners);
    }
    return node;
  });
  const peripheral = nodes.filter((node) => node.kind !== "file");
  const ungroupedPeripheral = peripheral.filter((node) => !nodeGroups.has(node.id));
  const peripheralByKind = new Map<string, GraphNode[]>();
  for (const node of ungroupedPeripheral) {
    const members = peripheralByKind.get(node.kind) || [];
    members.push(node);
    peripheralByKind.set(node.kind, members);
  }
  const peripheralGroups = [...peripheralByKind.entries()]
    .sort(([left], [right]) => utf8Compare(left, right))
    .map(([kind, members]) => {
      const group = peripheralGroupNode(kind, members);
      for (const member of members) nodeGroups.set(member.id, [group.id]);
      return group;
    });
  for (const node of nodes) {
    if (node.kind === "file" && !nodeGroups.has(node.id)) {
      throw new Error(`graph projection file ${node.id} has no group membership`);
    }
  }
  const groupNodeId = new Map(
    [...groups, ...peripheralGroups].map((node) => [
      String(node.attributes.id),
      node.id,
    ]),
  );
  const groupRelationItems = projection.fact.items
    .filter((item) => recordKind(item) === "group_relation");
  const projectedGroupEdges = groupRelationItems.map((item) => {
    const edge = canonicalEdge(item);
    const source = groupNodeId.get(edge.source);
    const target = groupNodeId.get(edge.target);
    if (!source || !target) {
      throw new Error(`graph projection group relation ${edge.id} has an unknown endpoint`);
    }
    return {
      ...edge,
      attributes: {
        ...edge.attributes,
        canonical_edge_ids: strings(
          edge.attributes?.canonical_relation_keys,
        ),
        presentation_aggregate: true,
      },
      source,
      target,
    };
  });
  let overviewEdges: GraphEdge[] = projectedGroupEdges;
  const hasUnrepresentedCrossGroupRelation = !groupRelationItems.length
    && edges.some((edge) =>
      (nodeGroups.get(edge.source) || []).some((source) =>
        (nodeGroups.get(edge.target) || []).some((target) => source !== target)));
  if (hasUnrepresentedCrossGroupRelation) {
    const aggregated = new Map<string, EdgeAggregation>();
    for (const edge of edges) {
      for (const source of nodeGroups.get(edge.source) || []) {
        for (const target of nodeGroups.get(edge.target) || []) {
          aggregateEdge(aggregated, edge, source, target);
        }
      }
    }
    overviewEdges = [...aggregated.values()].map(finishAggregatedEdge);
  }
  const overviewNodes = [...groups, ...peripheralGroups];
  const files: GraphView = {
    ...graphBase(projection, "files"),
    edges,
    nodes,
    summary: { edges: edges.length, nodes: nodes.length },
  };
  const overview: GraphView = {
    ...graphBase(projection, "components"),
    edges: overviewEdges,
    nodes: overviewNodes,
    request: {
      max_edge_names: 0,
      max_nodes: 0,
      query: { grouping, projection: projection.definition, view },
      view: "components",
    },
    summary: { edges: overviewEdges.length, nodes: overviewNodes.length },
  };
  return {
    files,
    grouping,
    hasConfiguredComponents: nodes.some((node) =>
      node.kind === "file" && strings(node.attributes.components).length > 0),
    overview,
    view,
  };
}
