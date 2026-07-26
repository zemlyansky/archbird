import assert from "node:assert/strict";
import test from "node:test";

import type { GraphView } from "../src/artifacts/model";
import { exportPresentedGraph } from "../src/graph/export";

const graph: GraphView = {
  artifact: "archbird-graph-view",
  diagnostics: [],
  edges: [{
    classification: "direct",
    evidence: [],
    id: 'edge-"',
    kind: "import",
    names: ['api"<&'],
    omitted_names: 0,
    source: "a&",
    target: "b<",
  }],
  nodes: [{
    attributes: {},
    evidence: [],
    id: "a&",
    identity: "src/a.c",
    kind: "file",
    label: 'a "<&',
    parent: null,
  }, {
    attributes: {},
    evidence: [],
    id: "b<",
    identity: "src/b.c",
    kind: "file",
    label: "b",
    parent: null,
  }],
  omissions: [],
  project: "demo",
  request: { max_edge_names: 0, max_nodes: 0, view: "files" },
  schema_version: 1,
  source: {},
  summary: { edges: 1, nodes: 2 },
  tool: {},
};

test("current presentation exports deterministic GraphML with escaped identities", () => {
  const output = new TextDecoder().decode(exportPresentedGraph(graph, "graphml", "LR"));
  assert.match(output, /^<\?xml version="1.0"/);
  assert.match(output, /id="a&amp;"/);
  assert.match(output, /a &quot;&lt;&amp;/);
  assert.match(output, /api&quot;&lt;&amp;/);
});

test("current presentation exports deterministic Mermaid with opaque node IDs", () => {
  const output = new TextDecoder().decode(exportPresentedGraph(graph, "mermaid", "TB"));
  assert.match(output, /^%% Archbird projected architecture view\nflowchart TB/);
  assert.match(output, /n0\["a \\"<&"\]/);
  assert.match(output, /n0 -->\|"import · api\\"<&"\| n1/);
});
