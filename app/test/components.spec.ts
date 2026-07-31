// @vitest-environment happy-dom

import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createApp, nextTick, type App as VueApp } from "vue";
import type { ParsedArtifact } from "../src/artifacts/model";
import ConstraintPanel from "../src/components/ConstraintPanel.vue";
import DocumentPanel from "../src/components/DocumentPanel.vue";
import InspectorPanel from "../src/components/InspectorPanel.vue";
import VerificationEditor from "../src/components/VerificationEditor.vue";

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
        schema_version: 3,
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
});
