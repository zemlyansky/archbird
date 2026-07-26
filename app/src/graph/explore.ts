import type { GraphEdge, GraphNode, GraphView } from "../artifacts/model";

export interface ExplorerState {
  expandedGroups: ReadonlySet<string>;
  expandedFiles: ReadonlySet<string>;
  hidden: ReadonlySet<string>;
  revealedSymbols: ReadonlySet<string>;
}

export interface ComponentDetails {
  description: string;
  files: string[];
  name: string;
  symbolCount: number;
}

export interface EdgePresentation {
  eyebrow: string;
  subtitle: string;
  summary: string;
  title: string;
}

export type GraphActivation = "file" | "group" | "symbol";
export type GraphEdgeFlow = "flow" | "uses";

interface PresentationGroup {
  description: string;
  files: string[];
  id: string;
  kind: string;
  label: string;
  memberNodeIds: string[];
  rootPath: string | null;
  symbolCount: number;
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

export function edgePresentation(
  graph: GraphView,
  edge: GraphEdge,
): EdgePresentation {
  const nodes = new Map(graph.nodes.map((node) => [node.id, node]));
  const sourceNode = nodes.get(edge.source);
  const targetNode = nodes.get(edge.target);
  const source = sourceNode?.label || edge.source;
  const target = targetNode?.label || edge.target;
  const relation = edge.kind.replaceAll("-", " ");
  const componentDependency = sourceNode?.kind === "component"
    && targetNode?.kind === "component";
  const groupedDependency = componentDependency
    || (sourceNode?.attributes.presentation_role === "group"
      && targetNode?.attributes.presentation_role === "group");
  const dependencyFlow = edge.attributes?.presentation_direction === "flow";
  return {
    eyebrow: dependencyFlow
      ? "dependency flow"
      : componentDependency
      ? "component dependency"
      : groupedDependency
        ? "grouped dependency"
        : `${edge.classification} relation`,
    subtitle: relation,
    summary: dependencyFlow
      ? groupedDependency
        ? `${source} supplies ${target} through ${relation} relations between their member files; ${target} depends on ${source}.`
        : `${source} supplies ${target} through a ${relation} relation; ${target} depends on ${source}.`
      : componentDependency
      ? `${source} depends on ${target} through ${relation} relations between their member files.`
      : groupedDependency
        ? `${source} relates to ${target} through ${relation} relations between their member files.`
      : `${source} has a ${relation} relation to ${target}.`,
    title: `${source} → ${target}`,
  };
}

export function orientGraphEdges(
  graph: GraphView,
  flow: GraphEdgeFlow,
): GraphView {
  if (flow === "uses") return graph;
  const edges = graph.edges.map((edge) => ({
    ...edge,
    attributes: {
      ...edge.attributes,
      canonical_source: edge.source,
      canonical_target: edge.target,
      presentation_direction: "flow",
    },
    source: edge.target,
    target: edge.source,
  }));
  return {
    ...graph,
    edges,
    summary: { edges: edges.length, nodes: graph.nodes.length },
  };
}

function presentationId(kind: string, ...segments: string[]): string {
  return `presentation:${kind}:${segments.map(encodeURIComponent).join(":")}`;
}

export function filePresentationId(group: string, path: string): string {
  return presentationId("file", group, path);
}

export function directoryPresentationId(group: string, path: string): string {
  return presentationId("directory", group, path);
}

export function graphActivation(node: GraphNode): GraphActivation | null {
  if (node.attributes.presentation_role === "group") return "group";
  if (node.kind === "file" || node.kind === "symbol") return node.kind;
  return null;
}

function symbolPresentationId(fileId: string, symbolId: string): string {
  return presentationId("symbol", fileId, symbolId);
}

function cloneNode(
  node: GraphNode,
  id: string,
  parent: string | null,
  attributes: Record<string, unknown>,
): GraphNode {
  return {
    ...node,
    attributes: { ...node.attributes, ...attributes },
    id,
    parent,
  };
}

function aggregateEdge(
  edges: Map<string, GraphEdge>,
  edge: GraphEdge,
  source: string,
  target: string,
): void {
  if (source === target) return;
  const key = JSON.stringify([edge.kind, source, target, edge.classification]);
  const current = edges.get(key);
  if (!current) {
    edges.set(key, {
      ...edge,
      attributes: {
        ...edge.attributes,
        canonical_edge_ids: [edge.id],
        presentation_aggregate: true,
      },
      evidence: [...edge.evidence],
      id: presentationId("edge", key),
      names: [...edge.names],
      source,
      target,
    });
    return;
  }
  current.names = [...new Set([...current.names, ...edge.names])].sort(utf8Compare);
  current.omitted_names += edge.omitted_names;
  current.evidence.push(...edge.evidence);
  const ids = current.attributes?.canonical_edge_ids;
  if (Array.isArray(ids) && !ids.includes(edge.id)) ids.push(edge.id);
}

function presentationGroups(graph: GraphView): PresentationGroup[] {
  return graph.nodes.flatMap((node) => {
    if (node.attributes.presentation_role !== "group") return [];
    const files = Array.isArray(node.attributes.member_files)
      ? node.attributes.member_files.filter((value): value is string =>
        typeof value === "string")
      : [];
    const memberNodeIds = Array.isArray(node.attributes.member_node_ids)
      ? node.attributes.member_node_ids.filter((value): value is string =>
        typeof value === "string")
      : [];
    const description = typeof node.attributes.description === "string"
      ? node.attributes.description
      : node.attributes.origin === "inferred"
        ? "Inferred from complete mapped file evidence; not a configured component."
        : "";
    return [{
      description,
      files,
      id: node.id,
      kind: node.kind,
      label: node.label,
      memberNodeIds,
      rootPath: typeof node.attributes.root_path === "string"
        ? node.attributes.root_path
        : null,
      symbolCount: typeof node.attributes.symbol_count === "number"
        ? node.attributes.symbol_count
        : Number(node.attributes.symbols || 0),
    }];
  });
}

function visibleGroupNode(
  row: PresentationGroup,
  canonical: GraphNode,
  state: ExplorerState,
): GraphNode | null {
  if (state.hidden.has(canonical.id)) return null;
  const expanded = state.expandedGroups.has(canonical.id);
  return cloneNode(canonical, canonical.id, null, {
    description: row.description,
    expanded,
    files: row.files.length,
    member_node_ids: row.memberNodeIds,
    member_files: row.files,
    presentation_kind: row.kind,
    presentation_role: "group",
    symbols: row.symbolCount,
  });
}

function collapsedGraph(
  overviewGraph: GraphView,
  groups: ReadonlyMap<string, PresentationGroup>,
  state: ExplorerState,
): GraphView {
  const nodes = overviewGraph.nodes.flatMap((canonical) => {
    if (state.hidden.has(canonical.id)) return [];
    const group = groups.get(canonical.id);
    const node = group
      ? visibleGroupNode(group, canonical, state)
      : cloneNode(canonical, canonical.id, canonical.parent, {
        presentation_kind: canonical.kind,
      });
    return node ? [node] : [];
  });
  const ids = new Set(nodes.map((node) => node.id));
  const edges = overviewGraph.edges.filter((edge) =>
    ids.has(edge.source) && ids.has(edge.target) && !state.hidden.has(edge.id));
  return {
    ...overviewGraph,
    edges,
    nodes,
    summary: { edges: edges.length, nodes: nodes.length },
  };
}

function directFileSymbols(graph: GraphView, path: string): {
  root: GraphNode | null;
  symbols: GraphNode[];
} {
  const root = graph.nodes.find((node) => node.kind === "file" && node.identity === path) || null;
  if (!root) return { root: null, symbols: [] };
  return {
    root,
    symbols: graph.nodes.filter((node) => node.parent === root.id),
  };
}

export function composeArchitectureGraph(
  overviewGraph: GraphView,
  fileGraph: GraphView | null,
  symbolGraphs: ReadonlyMap<string, GraphView>,
  state: ExplorerState,
): GraphView {
  const groups = presentationGroups(overviewGraph);
  const groupsById = new Map(groups.map((group) => [group.id, group]));
  if (!state.expandedGroups.size || !fileGraph) {
    return collapsedGraph(overviewGraph, groupsById, state);
  }

  const fileNodes = new Map(fileGraph.nodes
    .filter((node) => node.kind === "file")
    .map((node) => [node.identity, node]));
  const fileById = new Map(fileGraph.nodes.map((node) => [node.id, node]));
  const owners = new Map<string, string[]>();
  function addOwner(nodeId: string, groupId: string): void {
    const rows = owners.get(nodeId) || [];
    if (!rows.includes(groupId)) rows.push(groupId);
    owners.set(nodeId, rows);
  }
  for (const group of groups) {
    for (const file of group.files) {
      const canonical = fileNodes.get(file);
      if (canonical) addOwner(canonical.id, group.id);
    }
    for (const nodeId of group.memberNodeIds) addOwner(nodeId, group.id);
  }

  const nodes = new Map<string, GraphNode>();
  const groupIds = new Set<string>();
  for (const canonical of overviewGraph.nodes) {
    if (state.hidden.has(canonical.id)) continue;
    const group = groupsById.get(canonical.id);
    const node = group
      ? visibleGroupNode(group, canonical, state)
      : cloneNode(canonical, canonical.id, canonical.parent, {
        presentation_kind: canonical.kind,
      });
    if (!node) continue;
    nodes.set(node.id, node);
    if (group) groupIds.add(node.id);
  }

  const fileInstances = new Map<string, string[]>();
  const pathBoundaries = new Map<string, string>();
  function boundaryKey(groupId: string, path: string): string {
    return `${groupId}\0${path}`;
  }

  function ensureFile(
    path: string,
    groupId: string,
    parent: string,
  ): string | null {
    const canonical = fileNodes.get(path);
    const group = groupsById.get(groupId);
    if (!canonical || !group || !groupIds.has(groupId)) return null;
    const instanceKey = group.kind === "component" ? group.label : group.id;
    const id = filePresentationId(instanceKey, path);
    if (state.hidden.has(id)) return null;
    if (!nodes.has(id)) {
      nodes.set(id, cloneNode(canonical, id, parent, {
        canonical_id: canonical.id,
        canonical_path: path,
        group: groupId,
        root_group: groupId,
        expanded: state.expandedFiles.has(id),
        presentation_kind: "file",
      }));
    }
    const instances = fileInstances.get(path) || [];
    if (!instances.includes(id)) instances.push(id);
    fileInstances.set(path, instances);
    return id;
  }

  function ensureDirectory(
    group: PresentationGroup,
    path: string,
    label: string,
    parent: string,
  ): string | null {
    const id = directoryPresentationId(group.id, path);
    if (state.hidden.has(id)) return null;
    if (!nodes.has(id)) {
      const prefix = `${path}/`;
      const memberFiles = group.files.filter((file) => file.startsWith(prefix));
      nodes.set(id, {
        attributes: {
          expanded: state.expandedGroups.has(id),
          files: memberFiles.length,
          member_files: memberFiles,
          origin: "inferred",
          path,
          presentation_kind: "directory",
          presentation_role: "group",
          root_group: group.id,
        },
        evidence: [],
        id,
        identity: `${path}/`,
        kind: "directory",
        label,
        parent,
      });
    }
    return id;
  }

  function ensureGroupContents(group: PresentationGroup): void {
    for (const path of group.files) {
      const segments = path.split("/");
      let parent = group.id;
      let prefix = group.rootPath || "";
      let visible: string | null = group.id;
      let reachedFile = true;
      const firstDirectory = group.rootPath && segments[0] === group.rootPath ? 1 : 0;
      for (const segment of segments.slice(firstDirectory, -1)) {
        prefix = prefix ? `${prefix}/${segment}` : segment;
        const directory = ensureDirectory(group, prefix, segment, parent);
        if (!directory) {
          visible = null;
          reachedFile = false;
          break;
        }
        visible = directory;
        if (!state.expandedGroups.has(directory)) {
          reachedFile = false;
          break;
        }
        parent = directory;
      }
      if (reachedFile) visible = ensureFile(path, group.id, parent);
      if (visible) pathBoundaries.set(boundaryKey(group.id, path), visible);
    }
    for (const id of group.memberNodeIds) {
      const canonical = fileById.get(id);
      if (!canonical || state.hidden.has(id)) continue;
      nodes.set(id, cloneNode(canonical, id, group.id, {
        root_group: group.id,
        presentation_kind: canonical.kind,
      }));
    }
  }

  for (const group of groups) {
    if (!state.expandedGroups.has(group.id)) continue;
    ensureGroupContents(group);
  }

  function hasExpandedOwner(id: string): boolean {
    return (owners.get(id) || []).some((owner) =>
      state.expandedGroups.has(owner));
  }

  function endpoints(id: string): string[] {
    const file = fileById.get(id);
    if (!file) return [];
    if (file.kind !== "file") {
      if (state.hidden.has(file.id)) return [];
      const result: string[] = [];
      for (const owner of owners.get(file.id) || []) {
        if (!groupIds.has(owner)) continue;
        if (state.expandedGroups.has(owner)) {
          if (!nodes.has(file.id)) {
            nodes.set(file.id, cloneNode(file, file.id, owner, {
              root_group: owner,
              presentation_kind: file.kind,
            }));
          }
          result.push(file.id);
        } else {
          result.push(owner);
        }
      }
      if (!result.length) {
        if (!nodes.has(file.id)) {
          nodes.set(file.id, cloneNode(file, file.id, null, {
            presentation_kind: file.kind,
          }));
        }
        result.push(file.id);
      }
      return result;
    }
    const result: string[] = [];
    const fileOwners = owners.get(file.id) || [];
    for (const owner of fileOwners) {
      if (!groupIds.has(owner)) continue;
      if (state.expandedGroups.has(owner)) {
        const boundary = pathBoundaries.get(boundaryKey(owner, file.identity));
        if (boundary) result.push(boundary);
      } else {
        result.push(owner);
      }
    }
    if (!fileOwners.length && !state.hidden.has(file.id)) {
      if (!nodes.has(file.id)) {
        nodes.set(file.id, cloneNode(file, file.id, null, {
          presentation_kind: "file",
        }));
      }
      result.push(file.id);
    }
    return result;
  }

  const edges = new Map<string, GraphEdge>();
  for (const edge of overviewGraph.edges) {
    if (
      state.hidden.has(edge.id)
      || state.expandedGroups.has(edge.source)
      || state.expandedGroups.has(edge.target)
    ) continue;
    edges.set(edge.id, {
      ...edge,
      attributes: { ...edge.attributes },
      evidence: [...edge.evidence],
      names: [...edge.names],
    });
  }
  for (const edge of fileGraph.edges) {
    if (
      state.hidden.has(edge.id)
      || (!hasExpandedOwner(edge.source) && !hasExpandedOwner(edge.target))
    ) continue;
    for (const source of endpoints(edge.source)) {
      for (const target of endpoints(edge.target)) {
        aggregateEdge(edges, edge, source, target);
      }
    }
  }

  function presentationGroup(id: string): string | null {
    const node = nodes.get(id);
    if (!node) return null;
    if (typeof node.attributes.root_group === "string") {
      return node.attributes.root_group;
    }
    if (node.attributes.presentation_role === "group") return node.id;
    return node.parent && groupIds.has(node.parent) ? node.parent : null;
  }

  for (const edge of overviewGraph.edges) {
    if (
      state.hidden.has(edge.id)
      || (!state.expandedGroups.has(edge.source)
        && !state.expandedGroups.has(edge.target))
    ) continue;
    const replaced = [...edges.values()].some((candidate) =>
      candidate.kind === edge.kind
      && presentationGroup(candidate.source) === edge.source
      && presentationGroup(candidate.target) === edge.target);
    if (!replaced && nodes.has(edge.source) && nodes.has(edge.target)) {
      edges.set(edge.id, {
        ...edge,
        attributes: { ...edge.attributes, presentation_fallback: true },
        evidence: [...edge.evidence],
        names: [...edge.names],
      });
    }
  }

  for (const [path, graph] of symbolGraphs) {
    const details = directFileSymbols(graph, path);
    if (!details.root) continue;
    const symbolIds = new Set(details.symbols.map((node) => node.id));
    const queryNodes = new Map(graph.nodes.map((node) => [node.id, node]));
    const externalRelations = new Map<string, number>();
    for (const edge of graph.edges) {
      const sourceLocal = symbolIds.has(edge.source);
      const targetLocal = symbolIds.has(edge.target);
      if (sourceLocal === targetLocal) continue;
      const local = sourceLocal ? edge.source : edge.target;
      externalRelations.set(local, (externalRelations.get(local) || 0) + 1);
    }
    function boundary(nodeId: string): string[] {
      let node = queryNodes.get(nodeId);
      if (node?.kind === "symbol" && node.parent) node = queryNodes.get(node.parent);
      if (!node) return [];
      if (node.kind === "file") {
        const canonical = fileNodes.get(node.identity);
        return canonical ? endpoints(canonical.id) : [];
      }
      if (!state.hidden.has(node.id) && !nodes.has(node.id)) {
        nodes.set(node.id, cloneNode(node, node.id, null, {
          presentation_kind: node.kind,
        }));
      }
      return state.hidden.has(node.id) ? [] : [node.id];
    }
    for (const fileId of fileInstances.get(path) || []) {
      if (!state.expandedFiles.has(fileId)) continue;
      const clones = new Map<string, string>();
      for (const symbol of details.symbols) {
        const id = symbolPresentationId(fileId, symbol.id);
        if (state.hidden.has(id)) continue;
        nodes.set(id, cloneNode(symbol, id, fileId, {
          canonical_id: symbol.id,
          canonical_path: path,
          external_relations: externalRelations.get(symbol.id) || 0,
          presentation_kind: "symbol",
          relations_revealed: state.revealedSymbols.has(id),
        }));
        clones.set(symbol.id, id);
      }
      for (const edge of graph.edges) {
        if (state.hidden.has(edge.id)) continue;
        const sourceLocal = symbolIds.has(edge.source);
        const targetLocal = symbolIds.has(edge.target);
        if (!sourceLocal && !targetLocal) continue;
        if (sourceLocal !== targetLocal) {
          const localId = clones.get(sourceLocal ? edge.source : edge.target);
          if (!localId || !state.revealedSymbols.has(localId)) continue;
        }
        const sources = sourceLocal
          ? [clones.get(edge.source)].filter((id): id is string => Boolean(id))
          : boundary(edge.source);
        const targets = targetLocal
          ? [clones.get(edge.target)].filter((id): id is string => Boolean(id))
          : boundary(edge.target);
        for (const source of sources) {
          for (const target of targets) aggregateEdge(edges, edge, source, target);
        }
      }
    }
  }

  const visibleNodes = [...nodes.values()];
  const visibleIds = new Set(visibleNodes.map((node) => node.id));
  const visibleEdges = [...edges.values()].filter((edge) =>
    visibleIds.has(edge.source) && visibleIds.has(edge.target)
    && !state.hidden.has(edge.id));
  return {
    ...overviewGraph,
    edges: visibleEdges,
    nodes: visibleNodes,
    summary: { edges: visibleEdges.length, nodes: visibleNodes.length },
  };
}
