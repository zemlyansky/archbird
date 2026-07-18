"use strict";

const { createWasmFacade } = require("./wasm-facade");

async function createArchbirdCore(options = {}) {
  const createModule = require("../wasm/archbird.js");
  const module = await createModule(options);
  return createWasmFacade(module, { mode: "async" });
}

module.exports = { createArchbirdCore };
