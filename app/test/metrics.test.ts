import assert from "node:assert/strict";
import test from "node:test";

import type { GraphView } from "../src/artifacts/model";
import type { ProjectionResult } from "../src/artifacts/projection";
import {
  applyMetricOverlay,
  metricProjectionPlan,
  summarizeMetricScope,
  summarizeMetric,
} from "../src/graph/metrics";

function projection(): ProjectionResult {
  return {
    artifact: "projection-result",
    completeness: {
      classification: "incomplete",
      exhaustive: false,
      truncated: false,
    },
    definition: {
      id: "app-metric-source-cyclomatic-complexity",
      level: "symbol",
      metric: "source.cyclomatic_complexity",
      select: "metrics",
    },
    fact: {
      completeness: {
        classification: "incomplete",
        exhaustive: false,
        truncated: false,
      },
      items: [
        {
          attributes: {
            definition: "source.control-flow.c-v1",
            metric: "source.cyclomatic_complexity",
            path: "src/a.c",
            symbol: "complex",
            unit: "path",
          },
          evidence: [{
            detail: "construct=if_statement; contribution=1; control_depth=1",
            line: 12,
            path: "src/a.c",
          }],
          key: "f:a",
          label: "complex",
          message: "",
          state: "current",
          value: 9,
        },
        {
          attributes: {
            definition: "source.control-flow.c-v1",
            metric: "source.cyclomatic_complexity",
            path: "src/b.c",
            symbol: "simple",
            unit: "path",
          },
          evidence: [],
          key: "f:b",
          label: "simple",
          message: "",
          state: "current",
          value: 3,
        },
        {
          attributes: {
            known_lower_bound: 5,
            metric: "source.cyclomatic_complexity",
            path: "src/c.c",
            symbol: "recovered",
            unit: "path",
          },
          evidence: [],
          key: "f:c",
          label: "recovered",
          message: "source metric evidence is incomplete",
          state: "unknown",
          value: null,
        },
      ],
      message: "source metric evidence contains an unknown frontier",
      name: "app-metric-source-cyclomatic-complexity",
      project: "fixture",
      shape: "values",
      state: "unknown",
    },
    id: "app-metric-source-cyclomatic-complexity",
    schema_version: 1,
  };
}

function graph(): GraphView {
  return {
    artifact: "archbird-graph-view",
    diagnostics: [],
    edges: [],
    nodes: [
      {
        attributes: { path: "src/a.c" }, evidence: [], id: "file:src/a.c",
        identity: "src/a.c", kind: "file", label: "src/a.c", parent: null,
      },
      {
        attributes: { path: "src/b.c" }, evidence: [], id: "file:src/b.c",
        identity: "src/b.c", kind: "file", label: "src/b.c", parent: null,
      },
      {
        attributes: {
          member_files: ["src/a.c", "src/b.c"],
          presentation_role: "group",
        },
        evidence: [], id: "group:src", identity: "directory:src", kind: "directory",
        label: "src/", parent: null,
      },
    ],
    omissions: [],
    project: "fixture",
    request: { max_edge_names: 0, max_nodes: 0, view: "files" },
    schema_version: 1,
    source: {},
    summary: { edges: 0, nodes: 3 },
    tool: {},
  };
}

test("metric plans preserve core metric semantics", () => {
  assert.deepEqual(metricProjectionPlan("source.cyclomatic_complexity"), {
    id: "app-metric-source-cyclomatic-complexity",
    level: "symbol",
    metric: "source.cyclomatic_complexity",
    select: "metrics",
  });
  assert.deepEqual(metricProjectionPlan("graph.out_neighbors"), {
    id: "app-metric-graph-out-neighbors",
    level: "file",
    metric: "graph.out_neighbors",
    relations: ["calls", "imports", "references", "tests"],
    select: "metrics",
  });
});

test("metric summaries retain completeness, evidence, and ranked values", () => {
  const summary = summarizeMetric(projection());
  assert.equal(summary.metric, "source.cyclomatic_complexity");
  assert.equal(summary.population, 3);
  assert.equal(summary.measured, 2);
  assert.equal(summary.unknown, 1);
  assert.equal(summary.lowerBounds, 1);
  assert.equal(summary.knownMaximumLowerBound, 9);
  assert.equal(summary.median, null);
  assert.equal(summary.q1, null);
  assert.equal(summary.q3, null);
  assert.equal(summary.p95, null);
  assert.equal(summary.maximum, 9);
  assert.equal(summary.minimum, 3);
  assert.equal(summary.complete, false);
  assert.deepEqual(summary.rows.map((row) => [row.label, row.value]), [
    ["complex", 9],
    ["simple", 3],
    ["recovered", null],
  ]);
  assert.equal(summary.rows[0].evidence[0].line, 12);
  assert.equal(
    summary.histogram.reduce((total, bucket) => total + bucket.count, 0),
    2,
  );
});

test("metric summaries expose only exact values above p99", () => {
  const input = projection();
  input.completeness = {
    classification: "complete",
    exhaustive: true,
    truncated: false,
  };
  input.fact.completeness = {
    classification: "complete",
    exhaustive: true,
    truncated: false,
  };
  input.fact.state = "current";
  input.fact.message = "";
  input.fact.items = [
    ...Array.from({ length: 90 }, () => 1),
    2, 3, 4, 5, 6, 7, 8, 20, 40, 80,
  ].map((value, index) => ({
    attributes: {
      definition: "source.control-flow.c-v1",
      metric: "source.cyclomatic_complexity",
      path: `src/f${index}.c`,
      symbol: `f${index}`,
      unit: "path",
    },
    evidence: [{ detail: `complexity=${value}`, line: index + 1, path: `src/f${index}.c` }],
    key: `f:${index}`,
    label: `f${index}`,
    message: "",
    state: "current",
    value,
  }));

  const summary = summarizeMetric(input);
  assert.equal(summary.q1, 1);
  assert.equal(summary.median, 1);
  assert.equal(summary.q3, 1);
  assert.equal(summary.p90, 1);
  assert.equal(summary.p95, 6);
  assert.equal(summary.p99, 40);
  assert.equal(summary.lowerWhisker, 1);
  assert.equal(summary.upperWhisker, 6);
  assert.deepEqual(
    summary.tailPoints.map((point) => point.value),
    [80],
  );
});

test("metric summaries follow stable selected graph regions", () => {
  const selected = summarizeMetricScope(
    projection(),
    graph(),
    "group:src",
    "selection",
  );
  assert.equal(selected.scope.label, "src/");
  assert.equal(selected.summary.population, 2);
  assert.equal(selected.summary.minimum, 3);
  assert.equal(selected.summary.maximum, 9);
  assert.deepEqual(selected.summary.rows.map((row) => row.path), [
    "src/a.c",
    "src/b.c",
  ]);

  const project = summarizeMetricScope(
    projection(),
    graph(),
    "group:src",
    "project",
  );
  assert.equal(project.scope.label, "Project");
  assert.equal(project.summary.population, 3);
});

test("metric overlays heat files and groups without changing graph identity", () => {
  const overlaid = applyMetricOverlay(graph(), projection());
  assert.equal(overlaid.nodes[0].attributes.metric_value, 9);
  assert.equal(overlaid.nodes[0].attributes.metric_ratio, 1);
  assert.equal(overlaid.nodes[1].attributes.metric_value, 3);
  assert.equal(overlaid.nodes[1].attributes.metric_ratio, 1 / 3);
  assert.equal(overlaid.nodes[2].attributes.metric_value, 9);
  assert.equal(overlaid.nodes[2].attributes.metric_population, 2);
  assert.equal(overlaid.nodes[0].id, graph().nodes[0].id);
});

test("graph metric identities aggregate onto collapsed file groups", () => {
  const input = projection();
  input.definition = {
    id: "app-metric-graph-out-neighbors",
    level: "file",
    metric: "graph.out_neighbors",
    relations: ["imports"],
    select: "metrics",
  };
  input.fact.items = [{
    attributes: {
      definition: "graph.directed-neighbors-v1",
      entity_kind: "file",
      metric: "graph.out_neighbors",
      node: "file:src/a.c",
      unit: "neighbor",
    },
    evidence: [],
    key: "file:src/a.c",
    label: "src/a.c",
    message: "",
    state: "current",
    value: 4,
  }];
  const overlaid = applyMetricOverlay(graph(), input);
  assert.equal(overlaid.nodes[0].attributes.metric_value, 4);
  assert.equal(overlaid.nodes[2].attributes.metric_value, 4);
});
