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

async function waitFor(predicate, label, timeout = 30_000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 75));
  }
  throw new Error(`timed out waiting for ${label}`);
}

async function request(url, method, params = {}, expected = 200, options = {}) {
  const envelope = {
    method,
    params,
    ...(options.display === undefined ? {} : { display: options.display }),
    ...(options.session === undefined ? {} : { session: options.session }),
  };
  const response = await fetch(new URL("api/v1/request", url), {
    body: JSON.stringify(envelope),
    headers: { "Content-Type": "application/json" },
    method: "POST",
  });
  assert.equal(response.status, expected);
  const value = await response.json();
  assert.equal("id" in value, false);
  assert.equal(value.session, options.session);
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
  const documentedSource = path.join(repositoryRoot, "js/index.js");
  fs.writeFileSync(
    documentedSource,
    `/** @fileoverview Fixture arithmetic module. */\n`
      + `/** Add two values. */\n${fs.readFileSync(documentedSource, "utf8")}`,
  );
  const childArguments = ["-m", "archbird", "serve", "--root", repositoryRoot, "--port", "0"];
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
    const appResponse = await fetch(url);
    assert.equal(appResponse.status, 200);
    const appDocument = await appResponse.text();
    assert.match(appDocument, /<meta name="archbird-host" content="server"/);
    assert.doesNotMatch(appDocument, /<meta name="archbird-host" content="static"/);

    const firstBootstrap = await (await fetch(new URL("api/v1/bootstrap", url))).json();
    assert.equal(firstBootstrap.protocol_version, 2);
    assert.deepEqual(firstBootstrap.capabilities, [
      "map", "query", "path", "diff", "verify", "state", "display", "events",
    ]);
    assert.match(firstBootstrap.phase, /^(waiting|analyzing|ready)$/);
    await waitFor(async () => {
      const bootstrap = await (await fetch(new URL("api/v1/bootstrap", url))).json();
      return bootstrap.project === "map-base"
        && Number.isSafeInteger(bootstrap.schema_version)
        && bootstrap.phase === "ready";
    }, "initial Python live generation");

    const initial = await artifact(url, "map");
    const initialMap = JSON.parse(initial.bytes);
    assert.equal(initialMap.project, "map-base");
    assert.equal(
      (await (await fetch(new URL("api/v1/bootstrap", url))).json()).schema_version,
      initialMap.schema_version,
    );
    assert.equal(initial.descriptor.generation, initialMap.evidence.input_sha256);
    const observedDisplay = {
      artifact: "map",
      expanded: { files: [], groups: [], omitted: 0, symbols: [] },
      generation: initial.descriptor.generation,
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
        id: "file:js/index.js",
        identity: "js/index.js",
        kind: "file",
        label: "index.js",
        path: "js/index.js",
      },
      selected_constraint: null,
      source: null,
      workspace: "live",
    };
    const observedState = (await request(
      url,
      "state",
      { observed_display: observedDisplay },
      200,
      { session: "python-browser" },
    )).result;
    assert.deepEqual(observedState.observed_display, {
      ...observedDisplay,
      revision: 1,
      session: "python-browser",
    });
    assert.deepEqual(
      (await request(url, "state")).result.observed_display,
      observedState.observed_display,
    );
    const incompleteObservation = { ...observedDisplay };
    delete incompleteObservation.selected;
    assert.match((await request(
      url,
      "state",
      { observed_display: incompleteObservation },
      400,
      { session: "python-browser" },
    )).error.message, /selected/);
    assert.deepEqual(
      (await request(url, "state")).result.observed_display,
      observedState.observed_display,
    );
    const verification = JSON.parse((await artifact(url, "verify")).bytes);
    assert.equal(verification.artifact, "verification");
    assert.equal(verification.policy.project, "map-base");
    const membership = JSON.parse((await artifact(url, "projection", {
      plan: { id: "app-membership", select: "component_membership" },
    })).bytes);
    assert.equal(membership.artifact, "projection-result");
    assert.equal(membership.completeness.classification, "complete");
    assert.equal(membership.fact.items.length, initialMap.files.length);
    assert.equal(JSON.parse((await artifact(url, "view", { view: "files", max_nodes: 0 })).bytes).artifact, "archbird-graph-view");
    const symbols = JSON.parse((await artifact(url, "view", {
      max_nodes: 0,
      query: { depth: 0, paths: ["js/index.js"], testDepth: 0 },
      view: "symbols",
    })).bytes);
    assert.equal(symbols.artifact, "archbird-graph-view");
    assert.equal(symbols.request.view, "symbols");
    const add = symbols.nodes.find((row) => row.kind === "symbol" && row.label === "add");
    assert.equal(add.attributes.documentation.text, "Add two values.");
    const documentedFile = symbols.nodes.find(
      (row) => row.kind === "file" && row.label === "js/index.js",
    );
    assert.equal(
      documentedFile.attributes.documentation.text,
      "Fixture arithmetic module.",
    );
    const query = JSON.parse((await artifact(url, "query", {
      depth: 0, symbols: ["add"], test_depth: 0,
    })).bytes);
    assert.equal(query.artifact, "query");
    const connection = JSON.parse((await artifact(url, "path", {
      direction: "downstream",
      relations: ["imports"],
      source: { kind: "file", patterns: ["py/pkg/__init__.py"] },
      target: { kind: "file", patterns: ["py/pkg/api.py"] },
    })).bytes);
    assert.equal(connection.artifact, "path");
    assert.equal(connection.outcome, "found");
    const displayed = await request(
      url,
      "query",
      { depth: 0, symbols: ["add"], test_depth: 0 },
      200,
      { display: {}, session: "python-agent" },
    );
    const displayState = (await request(url, "state")).result.display;
    assert.equal(displayState.revision, 1);
    assert.equal(displayState.session, "python-agent");
    assert.equal(displayState.artifact.blob_sha256, displayed.result.blob_sha256);
    assert.match(displayState.presentation.blob_sha256, /^[0-9a-f]{64}$/);
    await request(url, "query", { depth: 0, symbols: ["add"], test_depth: 0 });
    assert.equal((await request(url, "state")).result.display.revision, 1);
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
    assert.equal(
      (await request(url, "state")).result.display,
      null,
      "stale display survived a new source generation",
    );
    assert.equal(
      (await request(url, "state")).result.observed_display,
      null,
      "stale browser observation survived a new source generation",
    );
    const snapshots = await request(url, "snapshots");
    assert.equal(snapshots.result.length, 2);
    assert.ok(snapshots.result.every(
      (row) => row.schema_version === initialMap.schema_version,
    ));
    assert.match((await request(url, "source", {
      generation: initialGeneration,
      path: "js/index.js",
    }, 400)).error.message, /changed after selected generation/);
    const diff = JSON.parse((await artifact(url, "diff", {
      before: initialGeneration,
      after: currentGeneration,
    })).bytes);
    assert.equal(diff.artifact, "diff");
    assert.match(stderr, /archbird serve\nProject: .*repository/);
    assert.doesNotMatch(stderr, /Open: http:/);
    assert.doesNotMatch(stderr, /archbird serve \[[0-9.]+s\]/);
    assert.match(stderr, /archbird serve: analysis started/);
    assert.match(stderr, /map ready: map-base · 3 files · [0-9a-f]{12} \([0-9]+ ms\)/);
    assert.match(stderr, /projection component_membership · app-membership/);
    assert.match(stderr, /view files/);
    assert.match(stderr, /query symbol add/);
    assert.match(stderr, /source js\/index\.js/);
    assert.match(stderr, /diff [0-9a-f]{12} → [0-9a-f]{12}/);
    console.log("Python live server Map/view/Verification/source/watch/last-good passed");
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
