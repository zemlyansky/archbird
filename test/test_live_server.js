#!/usr/bin/env node
"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { LiveRepository, createLiveServer } = require("../js/src/serve");

if (process.argv.length !== 5) {
  throw new Error("usage: test_live_server.js FIXTURE_ROOT APP_ROOT TEMP_ROOT");
}

const fixtureRoot = path.resolve(process.argv[2]);
const appRoot = path.resolve(process.argv[3]);
const temporaryRoot = path.resolve(process.argv[4]);
const repositoryRoot = path.join(temporaryRoot, "repository");
let nextId = 0;

async function waitFor(predicate, label, timeout = 20_000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(`timed out waiting for ${label}`);
}

async function request(server, method, payload = {}, { ok = true } = {}) {
  const envelope = {
    id: `test-${++nextId}`,
    method,
    payload,
    protocol_version: 1,
    session: "test-session",
  };
  const response = await fetch(new URL("api/v1/request", server.url), {
    body: JSON.stringify(envelope),
    headers: { "Content-Type": "application/json" },
    method: "POST",
  });
  const result = await response.json();
  assert.equal(result.id, envelope.id);
  assert.equal(result.session, envelope.session);
  assert.equal(result.ok, ok);
  return result;
}

async function artifact(server, method, payload = {}) {
  const envelope = await request(server, method, payload);
  const descriptor = envelope.result;
  assert.match(descriptor.blob_sha256, /^[0-9a-f]{64}$/);
  const response = await fetch(new URL(`api/v1/blobs/${descriptor.blob_sha256}`, server.url));
  assert.equal(response.status, 200);
  const bytes = Buffer.from(await response.arrayBuffer());
  assert.equal(bytes.length, descriptor.bytes);
  assert.equal(crypto.createHash("sha256").update(bytes).digest("hex"), descriptor.blob_sha256);
  return { bytes, descriptor };
}

async function main() {
  fs.rmSync(temporaryRoot, { force: true, recursive: true });
  fs.mkdirSync(temporaryRoot, { recursive: true });
  fs.cpSync(fixtureRoot, repositoryRoot, { recursive: true });
  const candidate = LiveRepository.prototype.candidate;
  let candidateCalls = 0;
  LiveRepository.prototype.candidate = async function delayedCandidate() {
    candidateCalls += 1;
    await new Promise((resolve) => setTimeout(resolve, 350));
    return candidate.call(this);
  };
  const started = Date.now();
  const server = await createLiveServer({ app: appRoot, port: 0, root: repositoryRoot });
  assert.ok(Date.now() - started < 250, "live server waited for initial Map analysis");
  assert.equal(server.repository.current, null);
  const events = [];
  server.repository.clients.add({
    end() {},
    write(value) { events.push(value); },
  });
  try {
    const appResponse = await fetch(server.url);
    assert.equal(appResponse.status, 200);
    const appDocument = await appResponse.text();
    assert.match(appDocument, /<meta name="archbird-host" content="server"/);
    assert.doesNotMatch(appDocument, /<meta name="archbird-host" content="static"/);

    const bootstrapResponse = await fetch(new URL("api/v1/bootstrap", server.url));
    assert.equal(bootstrapResponse.status, 200);
    const bootstrap = await bootstrapResponse.json();
    assert.equal(bootstrap.protocol_version, 1);
    assert.deepEqual(bootstrap.capabilities, [
      "map", "projection", "view", "export", "query", "diff", "source",
      "verification", "snapshots", "events",
    ]);
    assert.equal(bootstrap.project, null);
    assert.match(bootstrap.phase, /^(waiting|analyzing)$/);
    await waitFor(() => server.repository.current !== null, "initial live generation");
    const readyState = server.repository.state();
    assert.ok(Number.isSafeInteger(readyState.schema_version));
    const transportedState = (await request(server, "state")).result;
    assert.deepEqual(
      transportedState,
      readyState,
      "state request diverged from the authoritative repository state",
    );
    assert.equal(candidateCalls, 1);
    server.repository.schedule(path.join(repositoryRoot, "js/index.js"));
    await new Promise((resolve) => setTimeout(resolve, 300));
    assert.equal(candidateCalls, 1, "unchanged mapped file scheduled another analysis");
    assert.equal(server.repository.phase, "ready");
    LiveRepository.prototype.candidate = candidate;

    const initial = await artifact(server, "map");
    const initialMap = JSON.parse(initial.bytes);
    assert.equal(initialMap.project, "map-base");
    assert.equal(readyState.schema_version, initialMap.schema_version);
    assert.equal(initial.descriptor.generation, initialMap.evidence.input_sha256);
    const verification = JSON.parse((await artifact(server, "verification")).bytes);
    assert.equal(verification.artifact, "verification");
    assert.equal(verification.policy.project, "map-base");
    const membershipArtifact = await artifact(server, "projection", {
      plan: { id: "app-membership", select: "component_membership" },
    });
    const membership = JSON.parse(membershipArtifact.bytes);
    assert.equal(membership.artifact, "projection-result");
    assert.equal(membership.completeness.classification, "complete");
    assert.equal(membership.fact.items.length, initialMap.files.length);
    assert.equal(server.repository.current.derived.size, 1);
    const repeatedMembership = await artifact(server, "projection", {
      plan: { select: "component_membership", id: "app-membership" },
    });
    assert.equal(
      repeatedMembership.descriptor.blob_sha256,
      membershipArtifact.descriptor.blob_sha256,
    );
    assert.equal(
      server.repository.current.derived.size,
      1,
      "equivalent projection plans were evaluated more than once",
    );

    const view = await artifact(server, "view", { max_edge_names: 3, max_nodes: 0, view: "files" });
    assert.equal(JSON.parse(view.bytes).artifact, "archbird-graph-view");
    const symbols = JSON.parse((await artifact(server, "view", {
      max_edge_names: 3,
      max_nodes: 0,
      query: { depth: 0, paths: ["js/index.js"], testDepth: 0 },
      view: "symbols",
    })).bytes);
    assert.equal(symbols.artifact, "archbird-graph-view");
    assert.equal(symbols.request.view, "symbols");
    assert.ok(symbols.nodes.some((row) => row.kind === "symbol" && row.label === "add"));

    const source = await request(server, "source", { path: "js/index.js" });
    assert.equal(source.result.encoding, "utf-8");
    assert.match(source.result.text, /export function add/);
    const escape = await request(server, "source", { path: "../outside" }, { ok: false });
    assert.match(escape.error.message, /escapes repository root/);

    const initialGeneration = initial.descriptor.generation;
    const configurationPath = path.join(repositoryRoot, "archbird.json");
    const configuration = fs.readFileSync(configurationPath);
    fs.writeFileSync(configurationPath, "{}\n");
    await waitFor(
      () => events.some((value) => value.includes("candidate-failed")),
      "failed candidate event",
    );
    assert.equal(server.repository.current.generation, initialGeneration);

    const recoveryEvents = events.length;
    fs.writeFileSync(configurationPath, configuration);
    await waitFor(
      () => server.repository.phase === "ready" && server.repository.lastError === null,
      "unchanged live generation recovery",
    );
    assert.equal(server.repository.current.generation, initialGeneration);
    assert.ok(
      events.slice(recoveryEvents).some((value) => value.includes("snapshot-ready")),
      "successful unchanged rebuild did not publish readiness",
    );

    fs.appendFileSync(path.join(repositoryRoot, "js/index.js"), "\nexport const three = 3;\n");
    await waitFor(
      () => server.repository.current.generation !== initialGeneration,
      "new live generation",
    );
    const currentGeneration = server.repository.current.generation;
    assert.match(currentGeneration, /^[0-9a-f]{64}$/);
    const snapshots = await request(server, "snapshots");
    assert.equal(snapshots.result.length, 2);
    assert.ok(snapshots.result.every(
      (row) => row.schema_version === initialMap.schema_version,
    ));

    const staleSource = await request(
      server,
      "source",
      { generation: initialGeneration, path: "js/index.js" },
      { ok: false },
    );
    assert.match(staleSource.error.message, /changed after selected generation/);

    const opened = await artifact(server, "open-snapshot", { generation: initialGeneration });
    assert.equal(opened.descriptor.generation, initialGeneration);
    const selectedMap = await artifact(server, "map");
    assert.equal(selectedMap.descriptor.generation, initialGeneration);
    const latestMap = await artifact(server, "map", { generation: currentGeneration });
    assert.equal(latestMap.descriptor.generation, currentGeneration);

    const diff = await artifact(server, "diff", {
      after: currentGeneration,
      before: initialGeneration,
    });
    assert.equal(JSON.parse(diff.bytes).artifact, "diff");
    console.log("local live server Map/view/Verification/source/watch/last-good passed");
  } finally {
    LiveRepository.prototype.candidate = candidate;
    await server.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
