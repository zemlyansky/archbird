"use strict";

const metadata = require("../package.json");

if (metadata.version !== "0.0.1") {
  throw new Error(`refusing release with unexpected version ${metadata.version}`);
}
if (process.env.npm_config_tag && process.env.npm_config_tag !== "latest") {
  throw new Error("the stable release must use npm's latest tag");
}
