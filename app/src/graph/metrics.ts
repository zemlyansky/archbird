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
  maximum: number | null;
  measured: number;
  median: number | null;
  metric: MetricId;
  p95: number | null;
  population: number;
  rows: MetricRow[];
  unknown: number;
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

export const DEFAULT_METRIC: MetricId = "source.cyclomatic_complexity";

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
  const values = rows.flatMap((row) => row.value === null ? [] : [row.value])
    .sort((left, right) => left - right);
  return {
    complete: projection.completeness.classification === "complete"
      && projection.completeness.exhaustive
      && !projection.completeness.truncated,
    histogram: histogram(values),
    maximum: values.length ? values[values.length - 1] : null,
    measured: values.length,
    median: median(values),
    metric,
    p95: quantile(values, 0.95),
    population: rows.length,
    rows,
    unknown: rows.length - values.length,
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
