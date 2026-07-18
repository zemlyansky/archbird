#!/usr/bin/env node
"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { spawn } = require("node:child_process");

if (process.argv.length !== 6) {
  throw new Error("usage: test_python_live_server.js PYTHON FIXTURE_ROOT APP_ROOT TEMP_ROOT");
}

const pythonArgument = process.argv[2];
const python = path.isAbsolute(pythonArgument) || pythonArgument.includes(path.sep)
  ? path.resolve(pythonArgument)
  : pythonArgument;
const fixtureRoot = path.resolve(process.argv[3]);
const appRoot = path.resolve(process.argv[4]);
const packagedApp = process.argv[4] === "-";
const temporaryRoot = path.resolve(process.argv[5]);
const repositoryRoot = path.join(temporaryRoot, "repository");
let nextId = 0;

async function waitFor(predicate, label, timeout = 30_000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 75));
  }
  throw new Error(`timed out waiting for ${label}`);
}

async function request(url, method, payload = {}, expected = 200) {
  const envelope = {
    id: `python-${++nextId}`,
    method,
    payload,
    protocol_version: 1,
    session: "python-test",
  };
  const response = await fetch(new URL("api/v1/request", url), {
    body: JSON.stringify(envelope),
    headers: { "Content-Type": "application/json" },
    method: "POST",
  });
  assert.equal(response.status, expected);
  const value = await response.json();
  assert.equal(value.id, envelope.id);
  assert.equal(value.session, envelope.session);
  assert.equal(value.ok, expected === 200);
  return value;
}

async function artifact(url, method, payload = {}) {
  const envelope = await request(url, method, payload);
  const descriptor = envelope.result;
  assert.match(descriptor.blob_sha256, /^[0-9a-f]{64}$/);
  const response = await fetch(new URL(`api/v1/blobs/${descriptor.blob_sha256}`, url));
  assert.equal(response.status, 200);
  const bytes = Buffer.from(await response.arrayBuffer());
  assert.equal(bytes.length, descriptor.bytes);
  assert.equal(crypto.createHash("sha256").update(bytes).digest("hex"), descriptor.blob_sha256);
  assert.equal(response.headers.get("x-content-sha256"), descriptor.blob_sha256);
  return { bytes, descriptor };
}

function firstLine(stream) {
  return new Promise((resolve, reject) => {
    let buffered = "";
    const onData = (chunk) => {
      buffered += chunk.toString("utf8");
      const newline = buffered.indexOf("\n");
      if (newline >= 0) {
        stream.off("data", onData);
        resolve(buffered.slice(0, newline));
      }
    };
    stream.on("data", onData);
    stream.once("error", reject);
  });
}

async function main() {
  fs.rmSync(temporaryRoot, { recursive: true, force: true });
  fs.mkdirSync(temporaryRoot, { recursive: true });
  fs.cpSync(fixtureRoot, repositoryRoot, { recursive: true });
  const childArguments = ["-m", "archbird", "serve", repositoryRoot, "--port", "0"];
  if (!packagedApp) childArguments.push("--app", appRoot);
  const child = spawn(python, childArguments, { stdio: ["ignore", "pipe", "pipe"] });
  let stderr = "";
  child.stderr.on("data", (chunk) => { stderr += chunk.toString("utf8"); });
  let url;
  try {
    url = await Promise.race([
      firstLine(child.stdout),
      new Promise((_, reject) => child.once("exit", (code) => reject(
        new Error(`Python live server exited ${code}: ${stderr}`),
      ))),
    ]);
    assert.match(url, /^http:\/\/127\.0\.0\.1:\d+\/$/);
    const firstBootstrap = await (await fetch(new URL("api/v1/bootstrap", url))).json();
    assert.equal(firstBootstrap.protocol_version, 1);
    assert.match(firstBootstrap.phase, /^(waiting|analyzing|ready)$/);
    await waitFor(async () => {
      const bootstrap = await (await fetch(new URL("api/v1/bootstrap", url))).json();
      return bootstrap.project === "map-base" && bootstrap.phase === "ready";
    }, "initial Python live generation");

    const initial = await artifact(url, "map");
    const initialMap = JSON.parse(initial.bytes);
    assert.equal(initialMap.project, "map-base");
    assert.equal(initial.descriptor.generation, initialMap.evidence.input_sha256);
    assert.equal(JSON.parse((await artifact(url, "view", { view: "files", max_nodes: 0 })).bytes).artifact, "archbird-graph-view");
    const source = await request(url, "source", { path: "js/index.js" });
    assert.match(source.result.text, /export function add/);
    assert.match((await request(url, "source", { path: "../outside" }, 400)).error.message, /escapes repository root/);
    const forbidden = await fetch(new URL("api/v1/bootstrap", url), {
      headers: { Origin: "https://example.com" },
    });
    assert.equal(forbidden.status, 403);

    const initialGeneration = initial.descriptor.generation;
    const configPath = path.join(repositoryRoot, "archbird.json");
    const config = fs.readFileSync(configPath);
    fs.writeFileSync(configPath, "{}\n");
    await new Promise((resolve) => setTimeout(resolve, 1200));
    assert.equal((await request(url, "map")).result.generation, initialGeneration);

    fs.writeFileSync(configPath, config);
    fs.appendFileSync(path.join(repositoryRoot, "js/index.js"), "\nexport const pythonLive = true;\n");
    let currentGeneration;
    await waitFor(async () => {
      const current = await request(url, "map");
      currentGeneration = current.result.generation;
      return currentGeneration !== initialGeneration;
    }, "Python live generation");
    const snapshots = await request(url, "snapshots");
    assert.equal(snapshots.result.length, 2);
    assert.match((await request(url, "source", {
      generation: initialGeneration,
      path: "js/index.js",
    }, 400)).error.message, /changed after selected generation/);
    const diff = JSON.parse((await artifact(url, "diff", {
      before: initialGeneration,
      after: currentGeneration,
    })).bytes);
    assert.equal(diff.artifact, "diff");
    console.log("Python live server Map/view/source/watch/last-good passed");
  } finally {
    if (child.exitCode === null) {
      const exited = new Promise((resolve) => child.once("exit", resolve));
      child.kill("SIGTERM");
      await exited;
    }
    if (child.exitCode !== 0) throw new Error(`Python live server failed: ${stderr}`);
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
