#!/usr/bin/env node
"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

if (process.argv.length !== 5) {
  throw new Error("usage: test_node_input_budget.js REPOSITORY ADDON WASM_BUILD");
}

const repository = path.resolve(process.argv[2]);
process.env.ARCHBIRD_ENGINE = "native";
process.env.ARCHBIRD_NATIVE_ADDON = path.resolve(process.argv[3]);
const wasmBuild = path.resolve(process.argv[4]);
const archbird = require(path.join(repository, "js/src/index"));

const cached = Buffer.from(`[${"0,".repeat(4_000_000)}0]`);
assert.throws(
  () => archbird.jsonCanonicalize(cached, true),
  /status=5|exceeds 4000000/,
);
const project = Object.create(archbird.Project.prototype);
project._cachedMap = cached;
const nativePretty = project.mapJson({ pretty: true });
assert(nativePretty.subarray(0, 2).equals(Buffer.from("[\n")));
assert(
  archbird.jsonCanonicalize(nativePretty, false, false, true).equals(cached),
);

(async () => {
  const createModule = require(path.join(wasmBuild, "archbird.js"));
  const { createWasmFacade } = require(path.join(repository, "js/src/wasm-facade"));
  const module = await createModule({
    wasmBinary: fs.readFileSync(path.join(wasmBuild, "archbird.wasm")),
  });
  const core = createWasmFacade(module, { mode: "async" });
  assert.throws(
    () => core.jsonCanonicalize(cached, true),
    /status=5|exceeds 4000000/,
  );
  const wasmPretty = core.jsonCanonicalize(cached, true, false, true);
  assert(wasmPretty.subarray(0, 2).equals(Buffer.from("[\n")));
  assert(core.jsonCanonicalize(wasmPretty, false, false, true).equals(cached));
  console.log("Node native/Wasm saved-artifact input budgets passed");
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
