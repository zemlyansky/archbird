import assert from "node:assert/strict";
import test from "node:test";

import type { GraphNode, GraphView } from "../src/artifacts/model";
import {
  composeArchitectureGraph,
  directoryPresentationId,
  edgePresentation,
  filePresentationId,
  graphActivation,
} from "../src/graph/explore";
import { applyVerificationOverlay } from "../src/graph/verification";

function node(
  id: string,
  identity: string,
  kind: string,
  parent: string | null = null,
  attributes: Record<string, unknown> = {},
): GraphNode {
  return {
    attributes,
    evidence: [],
    id,
    identity,
    kind,
    label: identity.replace(/^component:/, ""),
    parent,
  };
}

function view(kind: "components" | "files" | "symbols", nodes: GraphNode[], edges: GraphView["edges"]): GraphView {
  return {
    artifact: "archbird-graph-view",
    diagnostics: [],
    edges,
    nodes,
    omissions: [],
    project: "demo",
    request: { max_edge_names: 0, max_nodes: 0, view: kind },
    schema_version: 1,
    source: {},
    summary: { edges: edges.length, nodes: nodes.length },
    tool: {},
  };
}

const componentGraph = view("components", [
  node("component-a", "component:a", "component", null, {
    description: "A",
    member_files: ["src/a.c"],
    presentation_role: "group",
    symbol_count: 2,
  }),
  node("component-b", "component:b", "component", null, {
    description: "B",
    member_files: ["src/b.c"],
    presentation_role: "group",
    symbol_count: 1,
  }),
], [{
  classification: "direct",
  evidence: [{ path: "src/a.c" }],
  id: "component-edge",
  kind: "call",
  names: ["b"],
  omitted_names: 0,
  source: "component-a",
  target: "component-b",
}]);
const fileGraph = view("files", [
  node("file-a", "src/a.c", "file"),
  node("file-b", "src/b.c", "file"),
], [{
  classification: "direct",
  evidence: [{ path: "src/a.c" }],
  id: "file-edge",
  kind: "call",
  names: ["b"],
  omitted_names: 0,
  source: "file-a",
  target: "file-b",
}]);
const symbolGraph = view("symbols", [
  node("query-file-a", "src/a.c", "file"),
  { ...node("symbol-a1", "src/a.c:first", "symbol", "query-file-a"), label: "first" },
  { ...node("symbol-a2", "src/a.c:second", "symbol", "query-file-a"), label: "second" },
  node("query-file-b", "src/b.c", "file"),
  { ...node("symbol-b1", "src/b.c:external", "symbol", "query-file-b"), label: "external" },
], [{
  classification: "direct",
  evidence: [{ path: "src/a.c" }],
  id: "symbol-edge",
  kind: "call",
  names: ["second"],
  omitted_names: 0,
  source: "symbol-a1",
  target: "symbol-a2",
}, {
  classification: "direct",
  evidence: [{ path: "src/a.c" }],
  id: "symbol-boundary-edge",
  kind: "call",
  names: ["external"],
  omitted_names: 0,
  source: "symbol-a2",
  target: "symbol-b1",
}]);
test("collapsed architecture explorer preserves the canonical component projection", () => {
  const graph = composeArchitectureGraph(componentGraph, null, new Map(), {
    expandedGroups: new Set(),
    expandedFiles: new Set(),
    hidden: new Set(),
    revealedSymbols: new Set(),
  });
  assert.deepEqual(graph.edges.map((edge) => edge.id), ["component-edge"]);
  assert.deepEqual(graph.nodes.map((row) => row.id), ["component-a", "component-b"]);
});

test("component relations explain their architectural and file-level meaning", () => {
  assert.deepEqual(edgePresentation(componentGraph, componentGraph.edges[0]), {
    eyebrow: "component dependency",
    subtitle: "call",
    summary: "a depends on b through call relations between their member files.",
    title: "a → b",
  });
});

test("only containers, files, and symbols have graph activation behavior", () => {
  assert.equal(graphActivation(node("package", "package:runtime", "package")), null);
  assert.equal(graphActivation(node("external", "external:runtime", "external")), null);
  assert.equal(graphActivation(node("file", "src/a.c", "file")), "file");
  assert.equal(graphActivation(node("symbol", "src/a.c:first", "symbol")), "symbol");
  assert.equal(graphActivation({
    ...node("group", "directory:src", "directory"),
    attributes: { presentation_role: "group" },
  }), "group");
});

test("presentation never manufactures components for unowned files", () => {
  const components = view("components", [
    ...componentGraph.nodes,
    node("package-runtime", "package:runtime", "package"),
  ], componentGraph.edges);
  const files = view("files", [
    ...fileGraph.nodes,
    node("file-unowned", "notes.txt", "file"),
  ], [
    ...fileGraph.edges,
    {
      classification: "direct",
      evidence: [{ path: "src/a.c" }],
      id: "unowned-edge",
      kind: "reference",
      names: ["notes"],
      omitted_names: 0,
      source: "file-a",
      target: "file-unowned",
    },
  ]);
  const collapsed = composeArchitectureGraph(components, files, new Map(), {
    expandedGroups: new Set(),
    expandedFiles: new Set(),
    hidden: new Set(),
    revealedSymbols: new Set(),
  });
  assert.deepEqual(
    collapsed.nodes.map((row) => row.id),
    components.nodes.map((row) => row.id),
  );
  assert.equal(collapsed.nodes.some((row) => row.label === "unassigned"), false);

  const expanded = composeArchitectureGraph(components, files, new Map(), {
      expandedGroups: new Set(["component-a"]),
      expandedFiles: new Set(),
    hidden: new Set(),
    revealedSymbols: new Set(),
  });
  assert.equal(expanded.nodes.find((row) => row.id === "file-unowned")?.kind, "file");
  assert.equal(expanded.nodes.some((row) => row.label === "unassigned"), false);
});

test("selected components and files expand without flattening unrelated components", () => {
  const fileId = filePresentationId("a", "src/a.c");
  const directoryId = directoryPresentationId("component-a", "src");
  const graph = composeArchitectureGraph(
    componentGraph,
    fileGraph,
    new Map([["src/a.c", symbolGraph]]),
    {
      expandedGroups: new Set(["component-a", directoryId]),
      expandedFiles: new Set([fileId]),
      hidden: new Set(),
      revealedSymbols: new Set(),
    },
  );
  const expandedFile = graph.nodes.find((row) => row.id === fileId);
  assert.equal(expandedFile?.parent, directoryId);
  assert.equal(graph.nodes.find((row) => row.id === "component-b")?.parent, null);
  assert.equal(graph.nodes.filter((row) => row.parent === fileId).length, 2);
  assert.ok(graph.edges.some((edge) => edge.source === fileId && edge.target === "component-b"));
  assert.ok(graph.edges.some((edge) =>
    graph.nodes.find((row) => row.id === edge.source)?.parent === fileId
    && graph.nodes.find((row) => row.id === edge.target)?.parent === fileId));
  assert.equal(graph.edges.some((edge) =>
    graph.nodes.find((row) => row.id === edge.source)?.parent === fileId
    && edge.target === "component-b"), false);

  const externalSymbol = graph.nodes.find((row) =>
    row.parent === fileId && row.label === "second");
  assert.ok(externalSymbol);
  const revealed = composeArchitectureGraph(
    componentGraph,
    fileGraph,
    new Map([["src/a.c", symbolGraph]]),
    {
      expandedGroups: new Set(["component-a", directoryId]),
      expandedFiles: new Set([fileId]),
      hidden: new Set(),
      revealedSymbols: new Set([externalSymbol.id]),
    },
  );
  assert.ok(revealed.edges.some((edge) =>
    edge.source === externalSymbol.id && edge.target === "component-b"));
});

test("hidden presentation nodes remove their incident relations without changing source artifacts", () => {
  const graph = composeArchitectureGraph(componentGraph, fileGraph, new Map(), {
    expandedGroups: new Set(["component-a"]),
    expandedFiles: new Set(),
    hidden: new Set(["component-b"]),
    revealedSymbols: new Set(),
  });
  assert.equal(graph.nodes.some((row) => row.id === "component-b"), false);
  assert.equal(graph.edges.length, 0);
  assert.equal(componentGraph.edges.length, 1);
});

test("verification overlays map relations and findings onto the visible frontier", () => {
  const directoryId = directoryPresentationId("component-a", "src");
  const graph = composeArchitectureGraph(componentGraph, fileGraph, new Map(), {
    expandedGroups: new Set(["component-a", directoryId]),
    expandedFiles: new Set(),
    hidden: new Set(),
    revealedSymbols: new Set(),
  });
  const overlay = applyVerificationOverlay(graph, {
    artifact: "verification",
    constraints: [{
      assert: "allowed_edges",
      coverage: ['{"kind":"call","source":"a","target":"b"}'],
      findings: [{
        evidence: [{ path: "src/a.c" }],
        fingerprint: "f",
        key: "first",
        message: "unexpected symbol",
      }],
      id: "ARCH-1",
      owner: "architecture",
      rationale: "Calls remain within the reviewed component boundary.",
      requirements: ["ARCH-CALL-001"],
      severity: "warning",
      status: "fail",
      tags: ["dependencies"],
      witnesses: [{ detail: "component edge call a -> b" }],
    }],
  });
  const file = overlay.graph.nodes.find((row) => row.attributes.canonical_path === "src/a.c");
  assert.equal(file?.attributes.verification_status, "fail");
  assert.equal(file?.attributes.verification_findings, 1);
  assert.equal(overlay.graph.edges[0].attributes?.verification_status, "fail");
  assert.deepEqual(overlay.unmappedFindings, []);
  assert.equal(overlay.constraints[0].owner, "architecture");
  assert.equal(
    overlay.constraints[0].rationale,
    "Calls remain within the reviewed component boundary.",
  );
  assert.deepEqual(overlay.constraints[0].requirements, ["ARCH-CALL-001"]);
  assert.equal(overlay.constraints[0].severity, "warning");
  assert.deepEqual(overlay.constraints[0].tags, ["dependencies"]);
  assert.equal(overlay.constraints[0].witnesses.length, 1);
});
