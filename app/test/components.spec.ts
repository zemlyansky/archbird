// @vitest-environment happy-dom

import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createApp, h, nextTick, shallowReactive, type App as VueApp } from "vue";
import type { GraphView, ParsedArtifact } from "../src/artifacts/model";
import type { MetricId } from "../src/graph/metrics";
import ArtifactDrop from "../src/components/ArtifactDrop.vue";
import ConstraintPanel from "../src/components/ConstraintPanel.vue";
import DocumentPanel from "../src/components/DocumentPanel.vue";
import InspectorPanel from "../src/components/InspectorPanel.vue";
import MetricsPanel from "../src/components/MetricsPanel.vue";
import VerificationEditor from "../src/components/VerificationEditor.vue";
import { sourceSegments } from "../src/graph/source";

const encoder = new TextEncoder();
let mounted: VueApp<Element> | null = null;

function artifact(document: Record<string, unknown>, name: string): ParsedArtifact {
  return {
    artifact: String(document.artifact || "project-configuration"),
    bytes: encoder.encode(JSON.stringify(document)),
    document,
    name,
  };
}

function mount(component: Parameters<typeof createApp>[0], props: Record<string, unknown>) {
  const root = document.createElement("div");
  document.body.append(root);
  mounted = createApp(component, props);
  mounted.mount(root);
  return root;
}

beforeEach(() => {
  document.body.replaceChildren();
  vi.stubGlobal("URL", {
    ...URL,
    createObjectURL: vi.fn(() => "blob:archbird-test"),
    revokeObjectURL: vi.fn(),
  });
  vi.spyOn(HTMLAnchorElement.prototype, "click").mockImplementation(() => undefined);
});

afterEach(() => {
  mounted?.unmount();
  mounted = null;
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

describe("artifact workspaces", () => {
  it("requests only expanded metrics, deduplicates pending work, and retries explicitly", async () => {
    const state = shallowReactive({
      errors: new Map<MetricId, string>(),
      graph: {
        artifact: "archbird-graph-view", schema_version: 1, project: "metrics",
        source: {}, tool: {}, request: { view: "files", max_nodes: 0, max_edge_names: 0 },
        nodes: [], edges: [], summary: { nodes: 0, edges: 0 }, omissions: [], diagnostics: [],
      } satisfies GraphView,
      loading: new Set<MetricId>(),
      overlay: null,
      projections: new Map(),
      scope: "project" as const,
      selectedId: null,
    });
    const load = vi.fn((metric: MetricId) => {
      state.loading = new Set(state.loading).add(metric);
    });
    const root = mount({ render: () => h(MetricsPanel, { ...state, onLoad: load }) }, {});
    await nextTick();
    expect(load).not.toHaveBeenCalled();
    const metric = root.querySelector('[data-metric="source.cyclomatic_complexity"]')!;
    const toggle = metric.querySelector(".metric-summary") as HTMLButtonElement;
    toggle.click();
    await nextTick();
    expect(load.mock.calls).toEqual([["source.cyclomatic_complexity"]]);
    toggle.click();
    await nextTick();
    toggle.click();
    await nextTick();
    expect(load).toHaveBeenCalledTimes(1);
    state.errors = new Map([["source.cyclomatic_complexity", "metric probe failed"]]);
    state.loading = new Set();
    await nextTick();
    expect(load).toHaveBeenCalledTimes(1);
    const retry = [...metric.querySelectorAll("button")].find((button) => button.textContent === "Retry")!;
    retry.click();
    await nextTick();
    expect(load).toHaveBeenCalledTimes(2);
    // A new Map clears results/errors; an open metric follows the new generation.
    state.errors = new Map();
    state.loading = new Set();
    state.projections = new Map();
    await nextTick();
    expect(load).toHaveBeenCalledTimes(3);
  });

  it("selects source spans by UTF-8 byte offsets", () => {
    expect(sourceSegments(
      { encoding: "utf-8", path: "src/api.py", text: "aλdocz" },
      { end: 6, path: "src/api.py", start: 3 },
    )).toEqual({ after: "z", before: "aλ", selected: "doc" });
  });

  it("makes local source choices and provenance explicit", () => {
    const root = mount(ArtifactDrop, {
      prominent: true,
      status: "Ready for local analysis",
    });
    expect(root.textContent).toContain("Choose what to explore");
    expect(root.textContent).toContain("Project folder");
    expect(root.textContent).toContain("Source ZIP");
    expect(root.textContent).toContain("Saved artifact");
    expect(root.textContent).toContain("Nothing is sent to a remote service");
  });

  it("presents metric distributions and evidence-backed hotspots", async () => {
    const metricProjection = {
      artifact: "projection-result",
      completeness: { classification: "complete", exhaustive: true, truncated: false },
      definition: {
        id: "app-metric-source-cyclomatic-complexity",
        level: "symbol",
        metric: "source.cyclomatic_complexity",
        select: "metrics",
      },
      fact: {
        completeness: { classification: "complete", exhaustive: true, truncated: false },
        items: [
          {
            attributes: {
              definition: "source.control-flow.c-v1",
              metric: "source.cyclomatic_complexity",
              path: "src/core.c",
              symbol: "monster",
              unit: "path",
            },
            evidence: [{ detail: "construct=switch_case", line: 90, path: "src/core.c" }],
            key: "f:monster",
            label: "monster",
            message: "",
            state: "current",
            value: 80,
          },
          {
            attributes: {
              definition: "source.control-flow.c-v1",
              metric: "source.cyclomatic_complexity",
              path: "src/core.c",
              symbol: "dispatch",
              unit: "path",
            },
            evidence: [{ detail: "construct=if_statement", line: 42, path: "src/core.c" }],
            key: "f:dispatch",
            label: "dispatch",
            message: "",
            state: "current",
            value: 8,
          },
          {
            attributes: {
              metric: "source.cyclomatic_complexity",
              path: "src/core.c",
              symbol: "parse",
              unit: "path",
            },
            evidence: [],
            key: "f:parse",
            label: "parse",
            message: "",
            state: "current",
            value: 5,
          },
          {
            attributes: {
              metric: "source.cyclomatic_complexity",
              path: "src/core.c",
              symbol: "open",
              unit: "path",
            },
            evidence: [],
            key: "f:open",
            label: "open",
            message: "",
            state: "current",
            value: 2,
          },
          ...[
            ...Array.from({ length: 90 }, () => 1),
            2, 3, 4, 5, 6, 7, 20, 40,
          ].map((value, index) => ({
            attributes: {
              metric: "source.cyclomatic_complexity",
              path: "src/core.c",
              symbol: `helper${index}`,
              unit: "path",
            },
            evidence: [],
            key: `f:helper${index}`,
            label: `helper${index}`,
            message: "",
            state: "current",
            value,
          })),
        ],
        message: "",
        name: "app-metric-source-cyclomatic-complexity",
        project: "fixture",
        shape: "values",
        state: "current",
      },
      id: "app-metric-source-cyclomatic-complexity",
      schema_version: 1,
    };
    const metricGraph = {
      artifact: "archbird-graph-view",
      diagnostics: [],
      edges: [],
      nodes: [{
        attributes: { member_files: ["src/core.c"], presentation_role: "group" },
        evidence: [],
        id: "group:src",
        identity: "directory:src",
        kind: "directory",
        label: "src/",
        parent: null,
      }],
      omissions: [],
      project: "fixture",
      request: { max_edge_names: 0, max_nodes: 0, view: "files" },
      schema_version: 1,
      source: {},
      summary: { edges: 0, nodes: 1 },
      tool: {},
    };
    const query = vi.fn();
    const root = mount(MetricsPanel, {
      errors: new Map(),
      graph: metricGraph,
      loading: new Set(),
      onQuery: query,
      overlay: null,
      projections: new Map([["source.cyclomatic_complexity", metricProjection]]),
      scope: "selection",
      selectedId: "group:src",
    });
    expect(root.textContent).toContain("Metrics");
    expect(root.textContent).toContain("Selection");
    expect(root.textContent).toContain("Complexity");
    expect(root.querySelectorAll(".metric-accordion")).toHaveLength(7);
    const complexity = root.querySelector(
      '[data-metric="source.cyclomatic_complexity"]',
    ) as HTMLElement;
    expect(complexity.querySelector(".metric-summary-copy small")).toBeNull();
    expect(complexity.querySelector(".metric-summary-value strong")?.textContent).toBe("1");
    expect(complexity.querySelector(".metric-summary-value")?.textContent).toContain("1 median / 80 max");
    expect(complexity.querySelector(".metric-bar-fill")).toBeNull();
    expect(complexity.querySelectorAll(".metric-box-plot")).toHaveLength(1);
    expect(complexity.querySelectorAll("button.metric-plot-point")).toHaveLength(1);
    (complexity.querySelector(".metric-summary") as HTMLButtonElement).click();
    await nextTick();
    expect(complexity.querySelector(".metric-facts")?.textContent).toContain("Distribution");
    expect(complexity.querySelector(".metric-facts")?.textContent).toContain("Coverage");
    expect(complexity.querySelector(".metric-facts")?.textContent).not.toContain("Scope");
    expect(complexity.querySelector(".metric-distribution")?.textContent).toContain("Q1");
    expect(complexity.querySelector(".metric-distribution")?.textContent).toContain("p99 40");
    expect(complexity.querySelectorAll(".metric-box-plot")).toHaveLength(1);
    const extreme = complexity.querySelector("button.metric-plot-point.extreme") as HTMLButtonElement;
    expect(extreme.title).toBe("src/core.c:monster: 80; above p99");
    extreme.click();
    await nextTick();
    expect(query).toHaveBeenLastCalledWith("src/core.c", "monster");
    (complexity.querySelector(".metric-hotspot") as HTMLButtonElement).click();
    await nextTick();
    expect(query).toHaveBeenLastCalledWith("src/core.c", "monster");
    expect(root.querySelector(".metric-evidence")).toBeNull();
    expect(root.textContent).toContain("Measurements are not quality scores");
  });

  it("presents incomplete graph metrics as known lower bounds", async () => {
    const projection = {
      artifact: "projection-result",
      completeness: { classification: "incomplete", exhaustive: false, truncated: false },
      definition: {
        id: "app-metric-graph-out-neighbors",
        level: "file",
        metric: "graph.out_neighbors",
        relations: ["calls", "imports", "references", "tests"],
        select: "metrics",
      },
      fact: {
        completeness: { classification: "incomplete", exhaustive: false, truncated: false },
        items: [{
          attributes: {
            known_lower_bound: 4,
            metric: "graph.out_neighbors",
            node: "file:src/core.c",
            unit: "neighbor",
          },
          evidence: [],
          key: "file:src/core.c",
          label: "src/core.c",
          message: "graph evidence is incomplete; the exact metric value is unknown",
          state: "unknown",
          value: null,
        }],
        message: "graph evidence contains an unknown frontier",
        name: "app-metric-graph-out-neighbors",
        project: "fixture",
        shape: "values",
        state: "unknown",
      },
      id: "app-metric-graph-out-neighbors",
      schema_version: 1,
    };
    const graph = {
      artifact: "archbird-graph-view",
      diagnostics: [],
      edges: [],
      nodes: [{
        attributes: { path: "src/core.c" },
        evidence: [],
        id: "file:src/core.c",
        identity: "src/core.c",
        kind: "file",
        label: "src/core.c",
        parent: null,
      }],
      omissions: [],
      project: "fixture",
      request: { max_edge_names: 0, max_nodes: 0, view: "files" },
      schema_version: 1,
      source: {},
      summary: { edges: 0, nodes: 1 },
      tool: {},
    };
    const root = mount(MetricsPanel, {
      errors: new Map(),
      graph,
      loading: new Set(),
      overlay: null,
      projections: new Map([["graph.out_neighbors", projection]]),
      scope: "project",
      selectedId: null,
    });
    const metric = root.querySelector('[data-metric="graph.out_neighbors"]') as HTMLElement;
    expect(metric.querySelector(".metric-summary-value")?.textContent).toBe("≥4");
    (metric.querySelector(".metric-summary") as HTMLButtonElement).click();
    await nextTick();
    expect(metric.querySelector(".metric-facts")?.textContent).toContain("Known maximum");
    expect(metric.querySelector(".metric-facts")?.textContent).not.toContain("—–—");
    expect(metric.querySelector(".metric-hotspots h3")?.textContent).toBe("Highest known lower bounds");
  });

  it("explains configured component dependencies with concrete file relations", () => {
    const graph = {
      artifact: "archbird-graph-view",
      diagnostics: [],
      edges: [{
        attributes: {
          verification_constraints: ["ARCH-EDGE", "ARCH-CYCLE"],
          verification_findings: 0,
          verification_status: "pass",
        },
        classification: "direct",
        evidence: [],
        id: "edge",
        kind: "import",
        names: ["parser.h"],
        omitted_names: 0,
        source: "base",
        target: "parser",
      }],
      nodes: [{
        attributes: {
          description: "Base utilities",
          member_files: ["src/a.c", "src/b.c"],
          presentation_role: "group",
          symbol_count: 2,
        },
        evidence: [],
        id: "base",
        identity: "component:base",
        kind: "component",
        label: "base",
        parent: null,
      }, {
        attributes: {
          description: "Parser",
          member_files: ["vendor/parser.h"],
          presentation_role: "group",
          symbol_count: 1,
        },
        evidence: [],
        id: "parser",
        identity: "component:parser",
        kind: "component",
        label: "parser",
        parent: null,
      }],
      omissions: [],
      project: "demo",
      request: { max_edge_names: 10, max_nodes: 100, view: "components" },
      schema_version: 1,
      source: {},
      summary: { edges: 1, nodes: 2 },
      tool: {},
    };
    const root = mount(InspectorPanel, {
      constraint: null,
      fileGraph: {
        ...graph,
        edges: [{
          ...graph.edges[0],
          id: "file-edge-a",
          source: "file-a",
          target: "file-parser",
        }, {
          ...graph.edges[0],
          id: "file-edge-b",
          source: "file-b",
          target: "file-parser",
        }],
        nodes: [{
          ...graph.nodes[0],
          id: "file-a",
          identity: "src/a.c",
          kind: "file",
          label: "src/a.c",
        }, {
          ...graph.nodes[0],
          id: "file-b",
          identity: "src/b.c",
          kind: "file",
          label: "src/b.c",
        }, {
          ...graph.nodes[1],
          id: "file-parser",
          identity: "vendor/parser.h",
          kind: "file",
          label: "vendor/parser.h",
        }],
        request: { ...graph.request, view: "files" },
        summary: { edges: 2, nodes: 3 },
      },
      graph,
      selectedId: "edge",
      source: null,
      sourcePath: null,
    });

    expect(root.textContent).toContain("base → parser");
    expect(root.textContent).toContain(
      "base depends on parser through import relations between their member files.",
    );
    expect(root.textContent).toContain("Referenced names");
    expect(root.textContent).toContain("Underlying file relations · 2");
    expect(root.textContent).toContain("src/a.c");
    expect(root.textContent).toContain("src/b.c");
    expect(root.textContent).toContain("All applicable evaluated constraints pass");
  });

  it("reports the selected Map axes and unmapped constraint findings", () => {
    const graph = {
      artifact: "archbird-graph-view",
      diagnostics: [],
      edges: [],
      nodes: [],
      omissions: [],
      project: "demo",
      request: {
        max_edge_names: 0,
        max_nodes: 0,
        query: {
          grouping: "layer",
          projection: { group_by: "layer", level: "file", select: "graph" },
          view: "evidence",
        },
        view: "components",
      },
      schema_version: 1,
      source: {},
      summary: { edges: 0, nodes: 0 },
      tool: {},
    };
    const constraint = {
      assert: "required_subset",
      coverage: ["src/missing.c"],
      findings: [{
        evidence: [{ path: "src/missing.c" }],
        fingerprint: "a".repeat(64),
        key: "src/missing.c",
        message: "required path is not mapped",
      }],
      id: "REQUIRED-PATH",
      owner: "architecture",
      rationale: "Public paths remain available.",
      requirements: ["ARCH-001"],
      severity: "error",
      status: "fail",
      tags: ["public-api"],
      witnesses: [{ path: "archbird.json" }],
    };
    const root = mount(InspectorPanel, {
      constraint,
      fileGraph: null,
      graph,
      selectedId: null,
      source: null,
      sourcePath: null,
    });

    expect(root.textContent).toContain("Architecture constraint");
    expect(root.textContent).toContain("REQUIRED-PATH");
    expect(root.textContent).toContain("required path is not mapped");
    expect(root.textContent).toContain("Public paths remain available.");
    expect(root.textContent).toContain("architecture");
    expect(root.textContent).toContain("ARCH-001");
    expect(root.textContent).toContain("public-api");
    expect(root.textContent).not.toContain("Projectioncomponents");
    mounted?.unmount();

    const summary = mount(InspectorPanel, {
      constraint: null,
      fileGraph: null,
      graph,
      selectedId: null,
      source: null,
      sourcePath: null,
    });
    expect(summary.textContent).toContain("Evidence view");
    expect(summary.textContent).toContain("Viewevidence");
    expect(summary.textContent).toContain("Groupinglayer");
    expect(summary.textContent).toContain("Levelfile");
  });

  it("pluralizes constraint finding counts", () => {
    const root = mount(ConstraintPanel, {
      constraints: [{
        assert: "required_subset",
        coverage: [],
        findings: [{ evidence: [], fingerprint: "f", key: "x", message: "missing" }],
        id: "ONE",
        owner: "architecture",
        rationale: "One finding remains visible.",
        requirements: [],
        severity: "error",
        status: "fail",
        tags: [],
        witnesses: [],
      }],
      selectedId: null,
      unmappedFindings: 1,
    });
    expect(root.textContent).toContain("1 finding");
    expect(root.textContent).toContain("1 finding has no visible graph location");
  });

  it("mounts a Verification result and creates a reviewed waiver", async () => {
    const root = mount(VerificationEditor, {
      artifact: artifact({
        artifact: "verification",
        schema_version: 2,
        policy: { project: "demo" },
        constraints: [{
          id: "ARCH-1",
          findings: [{
            comparison: "extra",
            fingerprint: "a".repeat(64),
            key: "ui->storage",
            message: "forbidden edge",
          }],
        }],
      }, "verification.json"),
    });

    expect(root.querySelector(".verification-editor")).not.toBeNull();
    expect(root.querySelectorAll("select option")).toHaveLength(1);
    const inputs = root.querySelectorAll("input");
    (inputs[0] as HTMLInputElement).value = "architecture";
    inputs[0].dispatchEvent(new Event("input"));
    (root.querySelector("textarea") as HTMLTextAreaElement).value = "Temporary migration.";
    root.querySelector("textarea")?.dispatchEvent(new Event("input"));
    (inputs[1] as HTMLInputElement).value = "2026-08-31";
    inputs[1].dispatchEvent(new Event("input"));
    (root.querySelector("button") as HTMLButtonElement).click();
    await nextTick();
    expect(root.textContent).toContain("Saved a waiver entry");
  });

  it("mounts keyed constraints and exports a reviewed configuration", async () => {
    const root = mount(VerificationEditor, {
      artifact: artifact({
        project: "demo",
        layers: [{ name: "core", language: "c", globs: ["src/**"] }],
        constraints: {
          "NO-CYCLES": {
            actual: { projection: { select: "component_edges" } },
            assert: "acyclic",
            owner: "architecture",
            rationale: "Keep component dependencies acyclic.",
          },
        },
      }, "archbird.json"),
    });

    expect(root.querySelectorAll(".check-editor")).toHaveLength(1);
    expect(root.textContent).toContain("NO-CYCLES");
    (
      root.querySelector(
        ".verification-editor > button",
      ) as HTMLButtonElement
    ).click();
    await nextTick();
    expect(root.textContent).toContain("Saved the project configuration");
  });

  it("renders Diff, Plan, and Act artifacts as task-oriented summaries", () => {
    const diff = mount(DocumentPanel, {
      artifact: artifact({
        artifact: "diff",
        schema_version: 7,
        sections: {
          symbols: {
            added: [{ name: "demo_open" }],
            changed: [],
            removed: [{ name: "demo_close" }],
          },
        },
      }, "diff.json"),
    });
    expect(diff.textContent).toContain("Structural comparison");
    expect(diff.textContent).toContain("1 added");
    mounted?.unmount();

    const plan = mount(DocumentPanel, {
      artifact: artifact({
        artifact: "plan",
        schema_version: 9,
        objective: "Remove the forbidden declaration.",
        items: [{
          id: "remove-legacy",
          statement: "Remove legacy from src/api.c.",
          executable: true,
          operation: { action: "replace_range", path: "src/api.c" },
          non_executable_reasons: [],
        }],
        preserved_constraints: ["NO-CYCLES"],
        unknowns: [],
      }, "plan.json"),
    });
    expect(plan.textContent).toContain("Structural change plan");
    expect(plan.textContent).toContain("Remove legacy from src/api.c");
    mounted?.unmount();

    const act = mount(DocumentPanel, {
      artifact: artifact({
        artifact: "act",
        schema_version: 2,
        state: "accepted",
        plan_sha256: "a".repeat(64),
        transitions: [{
          item_ids: ["remove-legacy"],
          kind: "modify",
          path: "src/api.c",
          source_path: null,
        }],
        acceptance: { status: "satisfied" },
      }, "act.json"),
    });
    expect(act.textContent).toContain("Architecture act");
    expect(act.textContent).toContain("satisfied");
    expect(act.textContent).toContain("src/api.c");
  });

  it("opens and highlights exact declaration documentation source", async () => {
    const onSource = vi.fn();
    const graph = {
      artifact: "archbird-graph-view",
      diagnostics: [],
      edges: [],
      nodes: [{
        attributes: {},
        evidence: [],
        id: "file:api",
        identity: "src/api.py",
        kind: "file",
        label: "api.py",
        parent: null,
      }, {
        attributes: {
          documentation: {
            kind: "docstring",
            span: { end: 70, start: 16 },
            text: "Open the configured project.\n\nThe caller owns disposal.",
          },
        },
        evidence: [],
        id: "symbol:open",
        identity: "src/api.py:open_project",
        kind: "symbol",
        label: "open_project",
        parent: "file:api",
      }],
      omissions: [],
      project: "demo",
      request: { max_edge_names: 10, max_nodes: 100, view: "symbols" },
      schema_version: 1,
      source: {},
      summary: { edges: 0, nodes: 1 },
      tool: {},
    };
    const root = mount(InspectorPanel, {
      constraint: null,
      fileGraph: null,
      graph,
      onSource,
      selectedId: "symbol:open",
      source: {
        encoding: "utf-8",
        path: "src/api.py",
        text: "0123456789abcdefOpen the configured project. The caller owns disposal.trailing",
        truncated: false,
      },
      sourcePath: "src/api.py",
      sourceSelection: { end: 70, path: "src/api.py", start: 16 },
    });

    expect(root.textContent).toContain("Documentation · docstring");
    expect(root.textContent).toContain("Open the configured project.");
    expect(root.textContent).toContain("The caller owns disposal.");
    expect(root.textContent).not.toContain("[object Object]");
    expect(root.querySelector("mark.source-selection")?.textContent)
      .toBe("Open the configured project. The caller owns disposal.");

    (root.querySelector("button.documentation-source") as HTMLButtonElement).click();
    await nextTick();
    expect(onSource).toHaveBeenCalledWith({
      end: 70,
      path: "src/api.py",
      start: 16,
    });
  });
});
