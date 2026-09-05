import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import assert from "node:assert/strict";
import test from "node:test";
import Ajv2020 from "ajv/dist/2020.js";
import { VISUALIZATION_PROTOCOL_VERSION } from "../src/adapters/protocol";

const root = fileURLToPath(new URL("../..", import.meta.url));

function validator(name: string) {
  const schema = JSON.parse(readFileSync(`${root}/schema/${name}.schema.json`, "utf8"));
  return new Ajv2020({ allErrors: true, strict: false }).compile(schema);
}

const generation = "1".repeat(64);
const descriptor = {
  blob_sha256: "2".repeat(64),
  bytes: 123,
  generation,
  project: "fixture",
};

test("published visualization schemas describe the public live protocol", () => {
  const request = validator("visualization-request");
  const response = validator("visualization-response");
  const event = validator("visualization-event");

  assert.equal(request({
    display: { view: "source" },
    method: "query",
    params: { symbols: ["src/runtime.c:runtime_start"] },
  }), true, JSON.stringify(request.errors));
  assert.equal(response({
    generation,
    ok: true,
    protocol_version: VISUALIZATION_PROTOCOL_VERSION,
    result: descriptor,
  }), true, JSON.stringify(response.errors));
  assert.equal(event({
    event_id: 4,
    generation,
    payload: {
      artifact: descriptor,
      display: { view: "source" },
      generation,
      method: "query",
      params: { symbols: ["src/runtime.c:runtime_start"] },
      revision: 1,
    },
    protocol_version: VISUALIZATION_PROTOCOL_VERSION,
    type: "display-updated",
  }), true, JSON.stringify(event.errors));
  const observedDisplay = {
    artifact: "map",
    expanded: { files: [], groups: [], omitted: 0, symbols: [] },
    generation,
    graph: {
      direction: "LR",
      edge_classification: "all",
      edge_flow: "flow",
      edge_kind: "all",
      edges: 2,
      group_by: "directory",
      layout: "flow",
      nodes: 3,
      notation: "architecture",
      perspective: "overview",
      view: "components",
    },
    hidden: { ids: [], omitted: 0 },
    history_depth: 0,
    selected: {
      id: "file:src/runtime.c",
      identity: "src/runtime.c",
      kind: "file",
      label: "runtime.c",
      path: "src/runtime.c",
    },
    selected_constraint: null,
    source: null,
    workspace: "live",
  };
  assert.equal(request({
    method: "state",
    params: { observed_display: observedDisplay },
    session: "browser-one",
  }), true, JSON.stringify(request.errors));
  assert.equal(request({
    method: "state",
    params: {
      observed_display: {
        ...observedDisplay,
        selected: { id: "missing-contract" },
      },
    },
    session: "browser-one",
  }), false, "malformed browser display state passed the public schema");

  assert.equal(request({
    id: "legacy",
    method: "query",
    payload: {},
    protocol_version: 1,
    session: "legacy",
  }), false, "legacy worker envelope passed as a public server request");
});
