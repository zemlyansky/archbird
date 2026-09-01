import type { GraphView } from "../artifacts/model";
import type { GraphNotation } from "./notation";

export type GraphLayoutMode = "flow" | "force" | "radial";

export interface ElkEdge {
  id: string;
  sources: string[];
  targets: string[];
}

export interface ElkNode {
  children?: ElkNode[];
  height?: number;
  id: string;
  layoutOptions?: Record<string, string>;
  width?: number;
  x?: number;
  y?: number;
}

export interface ElkLayoutGraph {
  children: ElkNode[];
  edges: ElkEdge[];
}

const LEAF_HEIGHT = 52;
const LEAF_WIDTH = 170;
const PACKED_COMPOUND_MIN_CHILDREN = 16;

function scaled(value: number, spacingPercent: number, minimum: number): number {
  return Math.max(minimum, Math.round(value * spacingPercent / 100));
}

export function elkLeafSize(
  spacingPercent: number,
  notation: GraphNotation = "compact",
): {
  height: number;
  width: number;
} {
  if (notation === "architecture") {
    return {
      height: scaled(40, spacingPercent, 28),
      width: scaled(186, spacingPercent, 112),
    };
  }
  return {
    height: scaled(LEAF_HEIGHT, spacingPercent, 30),
    width: scaled(LEAF_WIDTH, spacingPercent, 40),
  };
}

export function elkLayoutOptions(
  direction: "BT" | "LR" | "RL" | "TB",
  spacingPercent: number,
  mode: GraphLayoutMode = "flow",
): Record<string, string> {
  const nodeSpacing = String(scaled(44, spacingPercent, 2));
  const componentSpacing = String(scaled(80, spacingPercent, 8));
  if (mode === "force") {
    return {
      "elk.algorithm": "force",
      "elk.force.iterations": "300",
      "elk.randomSeed": "1",
      "elk.spacing.componentComponent": componentSpacing,
      "elk.spacing.nodeNode": nodeSpacing,
    };
  }
  if (mode === "radial") {
    return {
      "elk.algorithm": "radial",
      "elk.radial.centerOnRoot": "true",
      "elk.radial.radius": String(scaled(90, spacingPercent, 12)),
      "elk.radial.sorter": "ID",
      "elk.randomSeed": "1",
      "elk.spacing.componentComponent": componentSpacing,
      "elk.spacing.nodeNode": nodeSpacing,
    };
  }
  return {
    "elk.algorithm": "layered",
    "elk.direction": {
      BT: "UP",
      LR: "RIGHT",
      RL: "LEFT",
      TB: "DOWN",
    }[direction],
    "elk.edgeRouting": "ORTHOGONAL",
    "elk.hierarchyHandling": "INCLUDE_CHILDREN",
    "elk.layered.spacing.nodeNodeBetweenLayers":
      String(scaled(90, spacingPercent, 6)),
    "elk.spacing.nodeNode": nodeSpacing,
  };
}

function stableCompare(left: string, right: string): number {
  return left < right ? -1 : left > right ? 1 : 0;
}

/** Build a deterministic tree because radial layout requires rooted structure. */
export function radialSpanningEdges(
  graph: GraphView,
  preferredRoot: string | null,
): ElkEdge[] {
  const nodeIds = new Set(graph.nodes.map((node) => node.id));
  const adjacency = new Map<string, { edge: string; neighbor: string }[]>(
    graph.nodes.map((node) => [node.id, []]),
  );
  for (const edge of graph.edges) {
    if (edge.source === edge.target
      || !nodeIds.has(edge.source)
      || !nodeIds.has(edge.target)) continue;
    adjacency.get(edge.source)?.push({ edge: edge.id, neighbor: edge.target });
    adjacency.get(edge.target)?.push({ edge: edge.id, neighbor: edge.source });
  }
  for (const entries of adjacency.values()) {
    entries.sort((left, right) =>
      stableCompare(left.neighbor, right.neighbor)
      || stableCompare(left.edge, right.edge));
  }
  const roots = [...nodeIds].sort((left, right) =>
    (adjacency.get(right)?.length || 0) - (adjacency.get(left)?.length || 0)
    || stableCompare(left, right));
  if (preferredRoot && nodeIds.has(preferredRoot)) {
    roots.splice(roots.indexOf(preferredRoot), 1);
    roots.unshift(preferredRoot);
  }
  const visited = new Set<string>();
  const result: ElkEdge[] = [];
  for (const root of roots) {
    if (visited.has(root)) continue;
    visited.add(root);
    const queue = [root];
    for (let index = 0; index < queue.length; index += 1) {
      const source = queue[index];
      for (const candidate of adjacency.get(source) || []) {
        if (visited.has(candidate.neighbor)) continue;
        visited.add(candidate.neighbor);
        queue.push(candidate.neighbor);
        result.push({
          id: `radial:${candidate.edge}`,
          sources: [source],
          targets: [candidate.neighbor],
        });
      }
    }
  }
  return result;
}

export function elkHierarchy(
  nodes: GraphView["nodes"],
  spacingPercent = 100,
  notation: GraphNotation = "compact",
  packedCompounds: ReadonlySet<string> = new Set(),
): ElkNode[] {
  const byId = new Map(nodes.map((node) => [node.id, node]));
  const children = new Map<string, string[]>();
  for (const node of nodes) {
    if (node.parent && byId.has(node.parent)) {
      const values = children.get(node.parent) || [];
      values.push(node.id);
      children.set(node.parent, values);
    }
  }

  function build(id: string): ElkNode {
    const descendants = children.get(id) || [];
    if (!descendants.length) {
      return { ...elkLeafSize(spacingPercent, notation), id };
    }
    return {
      children: descendants.map(build),
      id,
      layoutOptions: {
        ...(packedCompounds.has(id) ? {
          "elk.algorithm": "rectpacking",
          "elk.aspectRatio": "1.6",
          "elk.spacing.nodeNode": String(scaled(44, spacingPercent, 2)),
        } : {}),
        "elk.padding": `[top=${scaled(48, spacingPercent, 18)},`
          + `left=${scaled(24, spacingPercent, 4)},`
          + `bottom=${scaled(24, spacingPercent, 4)},`
          + `right=${scaled(24, spacingPercent, 4)}]`,
      },
    };
  }

  return nodes
    .filter((node) => !node.parent || !byId.has(node.parent))
    .map((node) => build(node.id));
}

/** Pack large groups locally; aggregate boundary routes only for positioning. */
export function elkLayoutGraph(
  graph: GraphView,
  spacingPercent: number,
  notation: GraphNotation,
  mode: GraphLayoutMode,
  preferredRoot: string | null,
): ElkLayoutGraph {
  if (mode === "radial") {
    return {
      children: elkHierarchy(graph.nodes, spacingPercent, notation),
      edges: radialSpanningEdges(graph, preferredRoot),
    };
  }
  if (mode !== "flow") {
    return {
      children: elkHierarchy(graph.nodes, spacingPercent, notation),
      edges: graph.edges.map((edge) => ({
        id: edge.id,
        sources: [edge.source],
        targets: [edge.target],
      })),
    };
  }

  const byId = new Map(graph.nodes.map((node) => [node.id, node]));
  const childCounts = new Map<string, number>();
  for (const node of graph.nodes) {
    if (node.parent && byId.has(node.parent)) {
      childCounts.set(node.parent, (childCounts.get(node.parent) || 0) + 1);
    }
  }
  const packed = new Set([...childCounts]
    .filter(([id, count]) =>
      count >= PACKED_COMPOUND_MIN_CHILDREN
      && byId.get(id)?.attributes.presentation_role === "group")
    .map(([id]) => id));

  function packedBoundary(id: string): string {
    let node = byId.get(id);
    let boundary = id;
    while (node) {
      if (packed.has(node.id)) boundary = node.id;
      node = node.parent ? byId.get(node.parent) : undefined;
    }
    return boundary;
  }

  const edges: ElkEdge[] = [];
  const boundaryPairs = new Set<string>();
  for (const edge of graph.edges) {
    const source = packedBoundary(edge.source);
    const target = packedBoundary(edge.target);
    if (source === target) continue;
    const replaced = source !== edge.source || target !== edge.target;
    if (replaced) {
      const pair = JSON.stringify([source, target]);
      if (boundaryPairs.has(pair)) continue;
      boundaryPairs.add(pair);
    }
    edges.push({
      id: replaced ? `layout:${edges.length}` : edge.id,
      sources: [source],
      targets: [target],
    });
  }
  return {
    children: elkHierarchy(graph.nodes, spacingPercent, notation, packed),
    edges,
  };
}

export function elkPositions(
  nodes: ElkNode[],
  offsetX = 0,
  offsetY = 0,
): Record<string, { x: number; y: number }> {
  const positions: Record<string, { x: number; y: number }> = {};
  for (const node of nodes) {
    const left = offsetX + (node.x ?? 0);
    const top = offsetY + (node.y ?? 0);
    positions[node.id] = {
      x: left + (node.width ?? 0) / 2,
      y: top + (node.height ?? 0) / 2,
    };
    Object.assign(positions, elkPositions(node.children || [], left, top));
  }
  return positions;
}
