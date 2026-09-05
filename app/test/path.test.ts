import assert from "node:assert/strict";
import test from "node:test";
import {
  pathGraphFromArtifact,
  pathStatusFromArtifact,
} from "../src/artifacts/path";

function candidate(identity: string, label: string) {
  return {
    attributes: { entity_kind: "symbol", id: identity, path: "src/control.py", symbol: label },
    evidence: [{ line: 7, path: "src/control.py" }],
    key: `node:${identity}`,
    label,
    state: "current",
  };
}

test("Path witnesses become directed graph relations", () => {
  const source = 'symbol:["src/control.py","Control.invoke"]';
  const target = 'symbol:["src/context.py","Context"]';
  const graph = pathGraphFromArtifact({
    artifact: "path",
    graph: { classification: "complete" },
    outcome: "found",
    paths: [{
      nodes: [source, target],
      state: "current",
      steps: [{
        relation: {
          attributes: {
            family: "calls",
            names: ["Context"],
            relation_kind: "call",
            resolution: "unique",
            source,
            target,
          },
          evidence: [{ line: 9, path: "src/control.py" }],
          key: "relation:control-context",
          state: "current",
        },
        traversal: "forward",
      }],
    }],
    project: "fixture",
    reason: "witnesses",
    search: { truncated: false },
    source: { candidates: [candidate(source, "Control.invoke")], state: "resolved" },
    target: { candidates: [candidate(target, "Context")], state: "resolved" },
  });

  assert.equal(graph.summary.nodes, 2);
  assert.equal(graph.summary.edges, 1);
  assert.equal(graph.edges[0].source, source);
  assert.equal(graph.edges[0].target, target);
  assert.equal(graph.edges[0].classification, "direct");
  assert.equal(graph.edges[0].evidence.length, 1);
  assert.equal(graph.request.query?.outcome, "found");
});

test("unknown Path shows disconnected endpoint candidates", () => {
  const source = 'symbol:["server.py","ControlService.invoke"]';
  const target = 'symbol:["programs/paraview/context.py","ParaViewInteractionContext"]';
  const graph = pathGraphFromArtifact({
    artifact: "path",
    graph: { classification: "incomplete" },
    outcome: "unknown",
    paths: [],
    project: "software-control",
    reason: "graph-incomplete",
    request: {},
    search: { shortest_length: null, truncated: false },
    source: {
      candidates: [{
        ...candidate(source, "ControlService.invoke"),
        label: `node:["symbol","${source}"]`,
      }],
      state: "resolved",
    },
    target: {
      candidates: [candidate(target, "ParaViewInteractionContext")],
      state: "resolved",
    },
  });

  assert.equal(graph.summary.nodes, 2);
  assert.equal(graph.summary.edges, 0);
  assert.deepEqual(graph.nodes.map((node) => node.label).sort(), [
    "ControlService.invoke",
    "ParaViewInteractionContext",
  ]);
  assert.equal(graph.request.query?.outcome, "unknown");
  assert.equal(graph.request.query?.graph_classification, "incomplete");
});

test("Path status states the result once", () => {
  assert.deepEqual(pathStatusFromArtifact({
    artifact: "path",
    graph: { classification: "incomplete" },
    outcome: "unknown",
    paths: [],
    reason: "graph-incomplete",
  }), {
    detail: "No connection witness · graph incomplete",
    outcome: "unknown",
  });
  assert.deepEqual(pathStatusFromArtifact({
    artifact: "path",
    graph: { classification: "complete" },
    outcome: "found",
    paths: [{}, {}],
    reason: "witnesses",
  }), {
    detail: "2 witnesses · graph complete",
    outcome: "found",
  });
});


test("upstream and mixed Path traversal preserve canonical relation direction", () => {
  for (const traversals of [["reverse"], ["reverse", "forward"]]) {
    const identities = ["file:a.js", "file:b.js", "file:c.js"].slice(0, traversals.length + 1);
    const graph = pathGraphFromArtifact({
      artifact: "path", outcome: "unknown", graph: { classification: "incomplete" },
      paths: [{
        nodes: identities, state: "unknown",
        steps: traversals.map((traversal, index) => ({
          traversal,
          relation: {
            key: `relation:${index}`, state: "unknown",
            attributes: {
              source: identities[traversal === "reverse" ? index + 1 : index],
              target: identities[traversal === "reverse" ? index : index + 1],
              relation_kind: "import", resolution: "candidate",
            },
          },
        })),
      }],
    });
    assert.deepEqual(graph.edges.map((edge) => [edge.source, edge.target]),
      traversals.map((traversal, index) => traversal === "reverse"
        ? [identities[index + 1], identities[index]] : [identities[index], identities[index + 1]]));
    assert.deepEqual(graph.edges.map((edge) => edge.attributes?.path_traversal), traversals);
    assert.ok(graph.edges.every((edge) => edge.classification === "unresolved"));
  }
});
