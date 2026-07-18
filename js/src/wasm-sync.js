"use strict";

const { createWasmFacade } = require("./wasm-facade");

function loadArchbirdCoreSync() {
  const module = require("../wasm/archbird.sync.js");
  return createWasmFacade(module, { mode: "node-sync" });
}

module.exports = { loadArchbirdCoreSync };
