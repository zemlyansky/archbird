"use strict";

const metadata = require("../package.json");

if (!/^[0-9]+\.[0-9]+\.[0-9]+$/.test(metadata.version)) {
  throw new Error(`refusing release with unexpected version ${metadata.version}`);
}
if (process.env.npm_config_tag && process.env.npm_config_tag !== "latest") {
  throw new Error("the stable release must use npm's latest tag");
}
