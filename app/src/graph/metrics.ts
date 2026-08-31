import type { GraphNode, GraphView } from "../artifacts/model";
import type {
  ProjectionEvidence,
  ProjectionResult,
} from "../artifacts/projection";

export type MetricId =
  | "graph.in_neighbors"
  | "graph.out_neighbors"
  | "graph.scc_size"
  | "source.cyclomatic_complexity"
  | "source.decision_count"
  | "source.extent_lines"
  | "source.max_control_nesting";

export interface MetricDefinition {
  description: string;
  id: MetricId;
  label: string;
  level: "file" | "symbol";
  unit: string;
}

export interface MetricRow {
  attributes: Record<string, unknown>;
  evidence: ProjectionEvidence[];
  key: string;
  label: string;
  lowerBound: number | null;
  message: string;
  path: string | null;
  state: "current" | "stale" | "unknown";
  symbol: string | null;
  value: number | null;
}

export interface MetricBucket {
  count: number;
  end: number;
  label: string;
  start: number;
}

export interface MetricSummary {
  complete: boolean;
  histogram: MetricBucket[];
  knownMaximumLowerBound: number | null;
  lowerBounds: number;
  lowerWhisker: number | null;
  maximum: number | null;
  measured: number;
  median: number | null;
  metric: MetricId;
  minimum: number | null;
  p90: number | null;
  p95: number | null;
  p99: number | null;
  population: number;
  q1: number | null;
  q3: number | null;
  rows: MetricRow[];
  unknown: number;
  tailPoints: MetricRow[];
  upperWhisker: number | null;
}

export type MetricScopeMode = "project" | "selection";

export interface ScopedMetricSummary {
  scope: {
    label: string;
    mode: MetricScopeMode;
    paths: number;
  };
  summary: MetricSummary;
}

const GRAPH_RELATIONS = ["calls", "imports", "references", "tests"] as const;

export const METRIC_DEFINITIONS: readonly MetricDefinition[] = [
  {
    description: "Independent control-flow paths per function or method.",
    id: "source.cyclomatic_complexity",
    label: "Complexity",
    level: "symbol",
    unit: "path",
  },
  {
    description: "Versioned control-flow decisions per function or method.",
    id: "source.decision_count",
    label: "Decisions",
    level: "symbol",
    unit: "decision",
  },
  {
    description: "Deepest contributing control-flow construct.",
    id: "source.max_control_nesting",
    label: "Nesting",
    level: "symbol",
    unit: "level",
  },
  {
    description: "Physical lines covered by an exact declaration extent.",
    id: "source.extent_lines",
    label: "Declaration size",
    level: "symbol",
    unit: "line",
  },
  {
    description: "Distinct files reached through selected outgoing relations.",
    id: "graph.out_neighbors",
    label: "Outgoing reach",
    level: "file",
    unit: "neighbor",
  },
  {
    description: "Distinct files reaching this file through selected relations.",
    id: "graph.in_neighbors",
    label: "Incoming reach",
    level: "file",
    unit: "neighbor",
  },
  {
    description: "Size of the strongly connected component containing each file.",
    id: "graph.scc_size",
    label: "Cycle component",
    level: "file",
    unit: "node",
  },
] as const;

export function metricDefinition(metric: MetricId): MetricDefinition {
  const definition = METRIC_DEFINITIONS.find((row) => row.id === metric);
  if (!definition) throw new Error(`unsupported application metric ${metric}`);
  return definition;
}

function planId(metric: MetricId): string {
  return `app-metric-${metric.replaceAll(".", "-").replaceAll("_", "-")}`;
}

export function metricProjectionPlan(metric: MetricId): Record<string, unknown> {
  const definition = metricDefinition(metric);
  return {
    id: planId(metric),
    level: definition.level,
    metric,
    ...(metric.startsWith("graph.") ? { relations: [...GRAPH_RELATIONS] } : {}),
    select: "metrics",
  };
}

function number(value: unknown): number | null {
  return typeof value === "number" && Number.isSafeInteger(value) && value >= 0
    ? value
    : null;
}

function text(value: unknown): string | null {
  return typeof value === "string" && value.length ? value : null;
}

function utf8Compare(left: string, right: string): number {
  const encoder = new TextEncoder();
  const a = encoder.encode(left);
  const b = encoder.encode(right);
  const common = Math.min(a.length, b.length);
  for (let index = 0; index < common; index += 1) {
    if (a[index] !== b[index]) return a[index] - b[index];
  }
  return a.length - b.length;
}

function quantile(sorted: readonly number[], fraction: number): number | null {
  if (!sorted.length) return null;
  const index = Math.max(0, Math.ceil(fraction * sorted.length) - 1);
  return sorted[index];
}

function median(sorted: readonly number[]): number | null {
  if (!sorted.length) return null;
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2
    ? sorted[middle]
    : (sorted[middle - 1] + sorted[middle]) / 2;
}

function histogram(values: readonly number[], count = 8): MetricBucket[] {
  if (!values.length) return [];
  const minimum = values[0];
  const maximum = values[values.length - 1];
  if (minimum === maximum) {
    return [{ count: values.length, end: maximum, label: String(maximum), start: minimum }];
  }
  const width = Math.max(1, Math.ceil((maximum - minimum + 1) / count));
  const buckets: MetricBucket[] = [];
  for (let start = minimum; start <= maximum; start += width) {
    const end = Math.min(maximum, start + width - 1);
    buckets.push({ count: 0, end, label: start === end ? String(start) : `${start}–${end}`, start });
  }
  for (const value of values) {
    const index = Math.min(buckets.length - 1, Math.floor((value - minimum) / width));
    buckets[index].count += 1;
  }
  return buckets;
}

export function summarizeMetric(projection: ProjectionResult): MetricSummary {
  if (projection.definition.select !== "metrics") {
    throw new Error("metric summary requires a Metrics ProjectionResult");
  }
  const metric = String(projection.definition.metric) as MetricId;
  metricDefinition(metric);
  const rows: MetricRow[] = projection.fact.items.map((item) => ({
    attributes: item.attributes,
    evidence: item.evidence,
    key: item.key,
    label: item.label,
    lowerBound: number(item.attributes.known_lower_bound),
    message: item.message,
    path: text(item.attributes.path),
    state: item.state,
    symbol: text(item.attributes.symbol),
    value: item.state === "current" ? number(item.value) : null,
  })).sort((left, right) =>
    Number(right.value !== null) - Number(left.value !== null)
    || (right.value ?? right.lowerBound ?? -1) - (left.value ?? left.lowerBound ?? -1)
    || utf8Compare(left.label, right.label)
    || utf8Compare(left.key, right.key));
  return summarizeRows(metric, rows, projection.completeness.classification === "complete"
    && projection.completeness.exhaustive
    && !projection.completeness.truncated);
}

function summarizeRows(
  metric: MetricId,
  rows: MetricRow[],
  complete: boolean,
): MetricSummary {
  const values = rows.flatMap((row) => row.value === null ? [] : [row.value])
    .sort((left, right) => left - right);
  const lowerBounds = rows.filter((row) => row.value === null && row.lowerBound !== null);
  const knownValues = rows.flatMap((row) => {
    const value = row.value ?? row.lowerBound;
    return value === null ? [] : [value];
  }).sort((left, right) => left - right);
  const exactDistribution = complete && values.length === rows.length;
  const q1 = exactDistribution ? quantile(values, 0.25) : null;
  const q3 = exactDistribution ? quantile(values, 0.75) : null;
  const p95 = exactDistribution ? quantile(values, 0.95) : null;
  const p99 = exactDistribution ? quantile(values, 0.99) : null;
  const minimum = values.length ? values[0] : null;
  const maximum = values.length ? values[values.length - 1] : null;
  // Percentile cutoffs stay interpretable for discrete, long-tailed code
  // metrics where a Tukey fence can classify much more than 5% as outliers.
  const tailPoints = exactDistribution && p99 !== null
    ? rows.filter((row) => row.value !== null && row.value > p99)
    : [];
  return {
    complete,
    histogram: histogram(values),
    knownMaximumLowerBound: knownValues.length ? knownValues[knownValues.length - 1] : null,
    lowerBounds: lowerBounds.length,
    lowerWhisker: exactDistribution ? minimum : null,
    maximum,
    measured: values.length,
    median: exactDistribution ? median(values) : null,
    metric,
    minimum,
    p90: exactDistribution ? quantile(values, 0.90) : null,
    p95,
    p99,
    population: rows.length,
    q1,
    q3,
    rows,
    unknown: rows.length - values.length,
    tailPoints,
    upperWhisker: p95,
  };
}

function selectedPaths(
  graph: GraphView,
  selectedId: string,
): { label: string; paths: Set<string>; symbol: string | null } | null {
  const nodes = new Map(graph.nodes.map((node) => [node.id, node]));
  const selectedNode = nodes.get(selectedId);
  const selectedEdge = graph.edges.find((edge) => edge.id === selectedId);
  const paths = new Set<string>();
  let label = "Selection";
  let symbol: string | null = null;

  function addNode(node: GraphNode | undefined): void {
    if (!node) return;
    label = selectedNode === node ? node.label : label;
    const members = Array.isArray(node.attributes.member_files)
      ? node.attributes.member_files.filter((value): value is string => typeof value === "string")
      : [];
    for (const path of members) paths.add(path);
    const path = nodePath(node);
    if (path) paths.add(path);
    if (node.kind === "symbol") {
      symbol = node.label;
      const parent = node.parent ? nodes.get(node.parent) : undefined;
      const parentPath = parent ? nodePath(parent) : null;
      if (parentPath) paths.add(parentPath);
    }
  }

  if (selectedNode) addNode(selectedNode);
  else if (selectedEdge) {
    label = `${nodes.get(selectedEdge.source)?.label || selectedEdge.source} → `
      + `${nodes.get(selectedEdge.target)?.label || selectedEdge.target}`;
    addNode(nodes.get(selectedEdge.source));
    addNode(nodes.get(selectedEdge.target));
  } else return null;
  return { label, paths, symbol };
}

export function summarizeMetricScope(
  projection: ProjectionResult,
  graph: GraphView,
  selectedId: string | null,
  mode: MetricScopeMode,
): ScopedMetricSummary {
  const base = summarizeMetric(projection);
  const selection = mode === "selection" && selectedId
    ? selectedPaths(graph, selectedId)
    : null;
  if (!selection) {
    return {
      scope: { label: "Project", mode: "project", paths: 0 },
      summary: base,
    };
  }
  const definition = metricDefinition(base.metric);
  const fileNodeIds = new Set(graph.nodes.flatMap((node) => {
    const path = nodePath(node);
    if (!path || !selection.paths.has(path)) return [];
    return [
      node.id,
      node.identity,
      ...(text(node.attributes.canonical_id) ? [String(node.attributes.canonical_id)] : []),
    ];
  }));
  const rows = base.rows.filter((row) => {
    const node = text(row.attributes.node);
    const inSelection = Boolean(
      (row.path && selection.paths.has(row.path))
      || (node && fileNodeIds.has(node)),
    );
    if (!inSelection) return false;
    return !selection.symbol || definition.level !== "symbol"
      || row.symbol === selection.symbol;
  });
  return {
    scope: {
      label: selection.label,
      mode: "selection",
      paths: selection.paths.size,
    },
    summary: summarizeRows(base.metric, rows, base.complete),
  };
}

function nodePath(node: GraphNode): string | null {
  return text(node.attributes.canonical_path)
    || text(node.attributes.path)
    || (node.kind === "file" ? node.identity : null);
}

export function applyMetricOverlay(
  graph: GraphView,
  projection: ProjectionResult,
): GraphView {
  const summary = summarizeMetric(projection);
  const fileValues = new Map<string, number[]>();
  const nodeValues = new Map<string, number>();
  const symbolValues = new Map<string, number>();
  for (const row of summary.rows) {
    if (row.value === null) continue;
    if (row.path) {
      const values = fileValues.get(row.path) || [];
      values.push(row.value);
      fileValues.set(row.path, values);
      if (row.symbol) symbolValues.set(`${row.path}\0${row.symbol}`, row.value);
    }
    const node = text(row.attributes.node);
    if (node) nodeValues.set(node, row.value);
  }
  const graphFileValues = new Map<string, number>();
  for (const node of graph.nodes) {
    const path = nodePath(node);
    const canonical = text(node.attributes.canonical_id) || node.id;
    const value = nodeValues.get(canonical) ?? nodeValues.get(node.id);
    if (path && value !== undefined) graphFileValues.set(path, value);
  }
  const maximum = summary.maximum || 0;
  const nodes = graph.nodes.map((node) => {
    const path = nodePath(node);
    const canonical = text(node.attributes.canonical_id) || node.id;
    const memberFiles = Array.isArray(node.attributes.member_files)
      ? node.attributes.member_files.filter((value): value is string => typeof value === "string")
      : [];
    let values: number[] = [];
    if (node.kind === "symbol" && path) {
      const value = symbolValues.get(`${path}\0${node.label}`);
      if (value !== undefined) values = [value];
    }
    if (!values.length && path) values = fileValues.get(path) || [];
    if (!values.length) {
      const value = nodeValues.get(canonical) ?? nodeValues.get(node.id);
      if (value !== undefined) values = [value];
    }
    if (!values.length && memberFiles.length) {
      values = memberFiles.flatMap((member) => {
        const sourceValues = fileValues.get(member) || [];
        const graphValue = graphFileValues.get(member);
        return graphValue === undefined ? sourceValues : [...sourceValues, graphValue];
      });
    }
    if (!values.length) return node;
    const value = Math.max(...values);
    return {
      ...node,
      attributes: {
        ...node.attributes,
        metric_name: summary.metric,
        metric_population: values.length,
        metric_ratio: maximum ? value / maximum : 0,
        metric_state: summary.complete ? "complete" : "partial",
        metric_value: value,
      },
    };
  });
  return { ...graph, nodes };
}
