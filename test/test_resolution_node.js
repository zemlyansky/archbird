"use strict";

const path = require("node:path");

if (process.argv.length !== 5 && process.argv.length !== 6) {
  throw new Error(
    "usage: test_resolution_node.js ADDON REPOSITORY FIXTURE [default]",
  );
}
process.env.ARCHBIRD_NATIVE_ADDON = path.resolve(process.argv[2]);
const { resolveDiscovery } = require(path.join(path.resolve(process.argv[3]), "js/src/index.js"));
const fixture = path.resolve(process.argv[4]);
const outputs = process.argv[5] === "default"
  ? [resolveDiscovery(fixture)]
  : [
      resolveDiscovery(fixture),
      resolveDiscovery(fixture, {
        project: "cli",
        ignoreFiles: [".customignore"],
        maxFileBytes: 100,
        maxIndexBytes: 1000,
      }),
      resolveDiscovery(fixture, {
        ignore: false,
        ignoreFiles: [".customignore"],
      }),
    ];
process.stdout.write(`${outputs.map((value) => value.toString("hex")).join("\n")}\n`);
