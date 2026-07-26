import type { GraphView } from "../artifacts/model";

export interface ElkNode {
  children?: ElkNode[];
  height?: number;
  id: string;
  layoutOptions?: Record<string, string>;
  width?: number;
  x?: number;
  y?: number;
}

const LEAF_HEIGHT = 52;
const LEAF_WIDTH = 170;

function scaled(value: number, spacingPercent: number, minimum: number): number {
  return Math.max(minimum, Math.round(value * spacingPercent / 100));
}

export function elkLeafSize(spacingPercent: number): {
  height: number;
  width: number;
} {
  return {
    height: scaled(LEAF_HEIGHT, spacingPercent, 30),
    width: scaled(LEAF_WIDTH, spacingPercent, 40),
  };
}

export function elkLayoutOptions(
  direction: "BT" | "LR" | "RL" | "TB",
  spacingPercent: number,
): Record<string, string> {
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
    "elk.spacing.nodeNode": String(scaled(44, spacingPercent, 2)),
  };
}

export function elkHierarchy(
  nodes: GraphView["nodes"],
  spacingPercent = 100,
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
      return { ...elkLeafSize(spacingPercent), id };
    }
    return {
      children: descendants.map(build),
      id,
      layoutOptions: {
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
