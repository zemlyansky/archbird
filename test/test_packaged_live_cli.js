#!/usr/bin/env node
"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { spawn } = require("node:child_process");

if (process.argv.length !== 5) {
  throw new Error("usage: test_packaged_live_cli.js CLI FIXTURE_ROOT TEMP_ROOT");
}
const cli = path.resolve(process.argv[2]);
const fixture = path.resolve(process.argv[3]);
const temporary = path.resolve(process.argv[4]);
const repository = path.join(temporary, "repository");

async function waitFor(predicate, label, timeout = 30_000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 75));
  }
  throw new Error(`timed out waiting for ${label}`);
}

function firstLine(stream) {
  return new Promise((resolve, reject) => {
    let buffered = "";
    const data = (chunk) => {
      buffered += chunk.toString("utf8");
      const newline = buffered.indexOf("\n");
      if (newline >= 0) {
        stream.off("data", data);
        resolve(buffered.slice(0, newline));
      }
    };
    stream.on("data", data);
    stream.once("error", reject);
  });
}

async function main() {
  fs.rmSync(temporary, { recursive: true, force: true });
  fs.mkdirSync(temporary, { recursive: true });
  fs.cpSync(fixture, repository, { recursive: true });
  const child = spawn(cli, ["serve", "--root", repository, "--port", "0"], {
    env: { ...process.env, ARCHBIRD_ENGINE: "native" },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stderr = "";
  child.stderr.on("data", (chunk) => { stderr += chunk.toString("utf8"); });
  try {
    const url = await Promise.race([
      firstLine(child.stdout),
      new Promise((_, reject) => child.once("exit", (code) => reject(
        new Error(`packaged live CLI exited ${code}: ${stderr}`),
      ))),
    ]);
    const html = await fetch(url);
    assert.equal(html.status, 200);
    assert.match(await html.text(), /Archbird/);
    const firstBootstrap = await (await fetch(new URL("api/v1/bootstrap", url))).json();
    assert.equal(firstBootstrap.protocol_version, 1);
    assert.match(firstBootstrap.phase, /^(waiting|analyzing|ready)$/);
    await waitFor(async () => {
      const bootstrap = await (await fetch(new URL("api/v1/bootstrap", url))).json();
      return bootstrap.project === "map-base" && bootstrap.phase === "ready";
    }, "packaged live generation");
    console.log("packaged Node live CLI and offline app passed");
  } finally {
    if (child.exitCode === null) {
      const exited = new Promise((resolve) => child.once("exit", resolve));
      child.kill("SIGTERM");
      await exited;
    }
    if (child.exitCode !== 0) throw new Error(stderr);
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
