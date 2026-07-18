import assert from "node:assert/strict";
import test from "node:test";
import { parseArtifact, parseGraphView, supportedViews } from "../src/artifacts/model";

const decoder = new TextEncoder();

test("saved artifact routing is explicit", () => {
  assert.deepEqual(supportedViews("map"), ["components", "files"]);
  assert.deepEqual(supportedViews("query"), ["symbols"]);
  assert.deepEqual(supportedViews("verification"), []);
});

test("artifact parsing rejects invalid UTF-8 JSON and missing identities", () => {
  assert.throws(() => parseArtifact(decoder.encode("[]"), "array.json"), /JSON object/);
  assert.throws(() => parseArtifact(decoder.encode("{}"), "empty.json"), /artifact/);
});

test("graph projection validates endpoint identities", () => {
  const base = {
    artifact: "archbird-graph-view",
    schema_version: 1,
    project: "fixture",
    source: {},
    tool: {},
    request: { view: "components", max_nodes: 0, max_edge_names: 3 },
    nodes: [
      {
        id: "n_a",
        identity: "component:a",
        kind: "component",
        label: "a",
        parent: null,
        attributes: {},
        evidence: [],
      },
    ],
    edges: [],
    summary: { nodes: 1, edges: 0 },
    omissions: [],
    diagnostics: [],
  };
  assert.equal(parseGraphView(base).project, "fixture");
  assert.throws(
    () => parseGraphView({
      ...base,
      edges: [{ id: "e_x", source: "n_a", target: "n_missing" }],
    }),
    /unknown node/,
  );
});
