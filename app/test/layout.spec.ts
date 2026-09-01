import { describe, expect, it } from "vitest";
import ELK from "elkjs/lib/elk.bundled.js";
import type { GraphView } from "../src/artifacts/model";
import {
  elkHierarchy,
  elkLeafSize,
  elkLayoutGraph,
  elkLayoutOptions,
  elkPositions,
  radialSpanningEdges,
} from "../src/graph/layout";
import {
  architectureNodeCard,
  architectureNodeDisplayLabel,
  architectureNodeLabel,
} from "../src/graph/notation";

describe("compound graph layout", () => {
  it("represents file containment in ELK and restores absolute child positions", () => {
    const nodes: GraphView["nodes"] = [
      { attributes: {}, evidence: [], id: "file", identity: "src/a.c", kind: "file", label: "a.c", parent: null },
      { attributes: {}, evidence: [], id: "symbol", identity: "src/a.c:f", kind: "symbol", label: "f", parent: "file" },
      { attributes: {}, evidence: [], id: "target", identity: "builtin:x", kind: "builtin", label: "x", parent: null },
    ];
    expect(elkHierarchy(nodes)).toEqual([
      {
        children: [{ height: 52, id: "symbol", width: 170 }],
        id: "file",
        layoutOptions: { "elk.padding": "[top=48,left=24,bottom=24,right=24]" },
      },
      { height: 52, id: "target", width: 170 },
    ]);

    expect(elkPositions([
      {
        children: [{ height: 20, id: "symbol", width: 40, x: 10, y: 15 }],
        height: 70,
        id: "file",
        width: 80,
        x: 100,
        y: 200,
      },
    ])).toEqual({
      file: { x: 140, y: 235 },
      symbol: { x: 130, y: 225 },
    });
  });

  it("scales graph and compound-node spacing without changing graph content", () => {
    expect(elkLeafSize(100)).toEqual({ height: 52, width: 170 });
    expect(elkLeafSize(60)).toEqual({ height: 31, width: 102 });
    expect(elkLeafSize(5)).toEqual({ height: 30, width: 40 });
    expect(elkLeafSize(60, "architecture")).toEqual({ height: 28, width: 112 });
    expect(elkLeafSize(5, "architecture")).toEqual({ height: 28, width: 112 });
    expect(elkLayoutOptions("LR", 60)).toMatchObject({
      "elk.direction": "RIGHT",
      "elk.layered.spacing.nodeNodeBetweenLayers": "54",
      "elk.spacing.nodeNode": "26",
    });
    expect(elkLayoutOptions("TB", 35)).toMatchObject({
      "elk.direction": "DOWN",
      "elk.layered.spacing.nodeNodeBetweenLayers": "32",
      "elk.spacing.nodeNode": "15",
    });
    expect(elkLayoutOptions("LR", 5)).toMatchObject({
      "elk.direction": "RIGHT",
      "elk.layered.spacing.nodeNodeBetweenLayers": "6",
      "elk.spacing.nodeNode": "2",
    });
    expect(elkLayoutOptions("LR", 60, "force")).toEqual({
      "elk.algorithm": "force",
      "elk.force.iterations": "300",
      "elk.randomSeed": "1",
      "elk.spacing.componentComponent": "48",
      "elk.spacing.nodeNode": "26",
    });
    expect(elkLayoutOptions("TB", 60, "radial")).toEqual({
      "elk.algorithm": "radial",
      "elk.radial.centerOnRoot": "true",
      "elk.radial.radius": "54",
      "elk.radial.sorter": "ID",
      "elk.randomSeed": "1",
      "elk.spacing.componentComponent": "48",
      "elk.spacing.nodeNode": "26",
    });
    expect(elkHierarchy([
      {
        attributes: {},
        evidence: [],
        id: "file",
        identity: "a",
        kind: "file",
        label: "a",
        parent: null,
      },
      {
        attributes: {},
        evidence: [],
        id: "symbol",
        identity: "a:f",
        kind: "symbol",
        label: "f",
        parent: "file",
      },
    ], 60)[0].layoutOptions).toEqual({
      "elk.padding": "[top=29,left=14,bottom=14,right=14]",
    });
    expect(elkHierarchy([
      {
        attributes: {},
        evidence: [],
        id: "node",
        identity: "node",
        kind: "file",
        label: "node",
        parent: null,
      },
    ], 5)).toEqual([{ height: 30, id: "node", width: 40 }]);
    expect(elkHierarchy([
      {
        attributes: {},
        evidence: [],
        id: "file",
        identity: "a",
        kind: "file",
        label: "a",
        parent: null,
      },
      {
        attributes: {},
        evidence: [],
        id: "symbol",
        identity: "a:f",
        kind: "symbol",
        label: "f",
        parent: "file",
      },
    ], 5)[0].layoutOptions).toEqual({
      "elk.padding": "[top=18,left=4,bottom=4,right=4]",
    });
  });

  it("packs high-cardinality compounds without laying out every boundary edge", async () => {
    const files = Array.from({ length: 48 }, (_, index) => ({
      attributes: {},
      evidence: [],
      id: `file-${index}`,
      identity: `tests/test-${index}.py`,
      kind: "file",
      label: `test-${index}.py`,
      parent: "tests",
    }));
    const graph: GraphView = {
      artifact: "archbird-graph-view",
      diagnostics: [],
      edges: files.map((file, index) => ({
        attributes: {},
        classification: "direct",
        evidence: [],
        id: `edge-${index}`,
        kind: "import",
        names: [],
        omitted_names: 0,
        source: file.id,
        target: index % 2 ? "runtime" : "testing",
      })),
      nodes: [
        {
          attributes: { presentation_role: "group" },
          evidence: [],
          id: "tests",
          identity: "tests/",
          kind: "directory",
          label: "tests/",
          parent: null,
        },
        ...files,
        ...["runtime", "testing"].map((id) => ({
          attributes: {}, evidence: [], id, identity: `package:${id}`,
          kind: "package", label: id, parent: null,
        })),
      ],
      omissions: [],
      project: "fixture",
      request: { max_edge_names: 0, max_nodes: 0, view: "files" },
      schema_version: 1,
      source: {},
      summary: { edges: 48, nodes: 51 },
      tool: {},
    };
    const input = elkLayoutGraph(graph, 60, "architecture", "flow", null);
    expect(input.edges).toHaveLength(2);
    expect(graph.edges).toHaveLength(48);
    const expandedFile = {
      ...graph,
      nodes: graph.nodes.map((node) => node.id === "tests"
        ? { ...node, attributes: {}, kind: "file" }
        : node),
    };
    expect(elkLayoutGraph(
      expandedFile,
      60,
      "architecture",
      "flow",
      null,
    ).edges).toHaveLength(48);

    const result = await new ELK().layout({
      id: "root",
      layoutOptions: elkLayoutOptions("LR", 60),
      ...input,
    });
    const group = result.children?.find((node) => node.id === "tests");
    expect(new Set(group?.children?.map((node) => node.x))).not.toHaveLength(1);
    expect(group?.width).toBeGreaterThan(group?.height || 0);

    const compact = await new ELK().layout({
      id: "root",
      layoutOptions: elkLayoutOptions("LR", 5),
      ...elkLayoutGraph(graph, 5, "architecture", "flow", null),
    });
    const compactGroup = compact.children?.find((node) => node.id === "tests");
    expect((compactGroup?.width || 0) * (compactGroup?.height || 0)).toBeLessThan(
      (group?.width || 0) * (group?.height || 0) * 0.8,
    );
  });

  it("orients a deterministic radial spanning forest from the selected node", () => {
    const graph: GraphView = {
      artifact: "archbird-graph-view",
      diagnostics: [],
      edges: [
        { attributes: {}, classification: "direct", evidence: [], id: "ab", kind: "call", names: [], omitted_names: 0, source: "a", target: "b" },
        { attributes: {}, classification: "direct", evidence: [], id: "bc", kind: "call", names: [], omitted_names: 0, source: "b", target: "c" },
        { attributes: {}, classification: "direct", evidence: [], id: "cd", kind: "call", names: [], omitted_names: 0, source: "c", target: "d" },
      ],
      nodes: ["a", "b", "c", "d"].map((id) => ({
        attributes: {}, evidence: [], id, identity: id, kind: "symbol", label: id, parent: null,
      })),
      omissions: [],
      project: "fixture",
      request: { max_edge_names: 0, max_nodes: 0, view: "symbols" },
      schema_version: 1,
      source: {},
      summary: { edges: 3, nodes: 4 },
      tool: {},
    };
    expect(radialSpanningEdges(graph, "c")).toEqual([
      { id: "radial:bc", sources: ["c"], targets: ["b"] },
      { id: "radial:cd", sources: ["c"], targets: ["d"] },
      { id: "radial:ab", sources: ["b"], targets: ["a"] },
    ]);
  });

  it("derives architecture notation from typed graph entities", () => {
    expect(architectureNodeDisplayLabel({
      attributes: { canonical_path: "src/features/graph/GraphCanvas.vue" },
      evidence: [], id: "file", identity: "src/features/graph/GraphCanvas.vue",
      kind: "file", label: "src/features/graph/GraphCanvas.vue", parent: null,
    })).toBe("…/graph/GraphCanvas.vue");
    expect(architectureNodeDisplayLabel({
      attributes: {}, evidence: [], id: "file",
      identity: "src/extremely_long_component_name.py",
      kind: "file", label: "src/extremely_long_component_name.py", parent: null,
    })).toBe("extremely_long_compon….py");
    expect(architectureNodeDisplayLabel({
      attributes: {}, evidence: [], id: "component", identity: "component:runtime",
      kind: "component", label: "runtime-platform-with-a-long-name", parent: null,
    })).toBe("runtime-platform-with-a-…");
    expect(architectureNodeLabel({
      attributes: { presentation_role: "group" }, evidence: [], id: "src",
      identity: "src/", kind: "directory", label: "src", parent: null,
    })).toBe("«directory»\nsrc");
    expect(architectureNodeLabel({
      attributes: {}, evidence: [], id: "runtime", identity: "component:runtime",
      kind: "component", label: "Runtime", parent: null,
    })).toBe("«component»\nRuntime");
    expect(architectureNodeLabel({
      attributes: { kind: "function" }, evidence: [], id: "source", identity: "src/run.c:run",
      kind: "symbol", label: "run", parent: "file",
    })).toBe("«function»\nrun");
    expect(architectureNodeLabel({
      attributes: { kind: "function»\nspoof" }, evidence: [], id: "source", identity: "src/run.c:run",
      kind: "symbol", label: "run", parent: "file",
    })).toBe("«symbol»\nrun");

    const card = architectureNodeCard({
      attributes: {}, evidence: [], id: "source", identity: "src/a&b.c",
      kind: "file", label: "a&b.c", parent: null,
    }, { accent: "#55b8d6", text: "#e6edf5" }, { height: 28, width: 112 });
    const svg = decodeURIComponent(card.slice(card.indexOf(",") + 1));
    expect(card.startsWith("data:image/svg+xml;charset=utf-8,")).toBe(true);
    expect(svg).toContain(">FILE</text>");
    expect(svg).toContain(">src/a&amp;b.c</text>");
    expect(svg).toContain('font-size="5"');
    expect(svg).toContain('font-weight="400"');
    expect(svg).toContain('<text x="5" y="6"');
    expect(svg).toContain('font-size="9"');
    expect(svg).toContain('viewBox="0 0 112 28"');
    expect(svg).toContain('preserveAspectRatio="none"');
    expect(svg.match(/dominant-baseline="middle"/g)).toHaveLength(2);
    expect(svg).not.toContain('<rect width="6"');
    expect(svg).not.toContain("«file»");

    const verifiedCard = architectureNodeCard({
      attributes: { verification_status: "pass" }, evidence: [], id: "src",
      identity: "directory:src", kind: "directory", label: "src", parent: null,
    }, {
      accent: "#fdba74",
      danger: "#e46f6f",
      success: "#49b979",
      text: "#e6edf5",
      warning: "#e2ae52",
    }, { height: 28, width: 112 });
    const verifiedSvg = decodeURIComponent(verifiedCard.slice(verifiedCard.indexOf(",") + 1));
    expect(verifiedSvg).toContain('fill="#49b979"');
    expect(verifiedSvg).not.toContain('fill="#fdba74"');
  });
});
