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

async function waitFor(predicate, label, timeout = 20_000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(`timed out waiting for ${label}`);
}

async function request(server, method, params = {}, {
  display,
  ok = true,
  session,
} = {}) {
  const envelope = {
    method,
    params,
    ...(display === undefined ? {} : { display }),
    ...(session === undefined ? {} : { session }),
  };
  const response = await fetch(new URL("api/v1/request", server.url), {
    body: JSON.stringify(envelope),
    headers: { "Content-Type": "application/json" },
    method: "POST",
  });
  const result = await response.json();
  assert.equal("id" in result, false);
  assert.equal(result.session, session);
  assert.equal(result.ok, ok);
  return result;
}

async function artifact(server, method, payload = {}, options = {}) {
  const envelope = await request(server, method, payload, options);
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
  const activities = [];
  const server = await createLiveServer({
    activity: (event) => activities.push(event),
    app: appRoot,
    port: 0,
    root: repositoryRoot,
  });
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
    assert.equal(bootstrap.protocol_version, 2);
    assert.deepEqual(bootstrap.capabilities, [
      "map", "query", "path", "diff", "verify", "state", "display", "events",
    ]);
    assert.equal(bootstrap.project, null);
    assert.match(bootstrap.phase, /^(waiting|analyzing)$/);
    await waitFor(() => server.repository.current !== null, "initial live generation");
    assert.ok(activities.some((row) => row.event === "scan-started"));
    assert.ok(activities.some((row) =>
      row.event === "snapshot-ready" && row.project === "map-base" && row.files === 3));
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
      server,
      "state",
      { observed_display: observedDisplay },
      { session: "browser-one" },
    )).result;
    assert.deepEqual(observedState.observed_display, {
      ...observedDisplay,
      revision: 1,
      session: "browser-one",
    });
    assert.deepEqual(
      (await request(server, "state")).result.observed_display,
      observedState.observed_display,
      "agent state request did not return the browser-observed display",
    );
    const incompleteObservation = { ...observedDisplay };
    delete incompleteObservation.selected;
    const rejectedObservation = await request(
      server,
      "state",
      { observed_display: incompleteObservation },
      { ok: false, session: "browser-one" },
    );
    assert.match(rejectedObservation.error.message, /select/);
    assert.deepEqual(server.repository.state().observed_display, observedState.observed_display);
    const verification = JSON.parse((await artifact(server, "verify")).bytes);
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

    const cache = new LiveRepository({ root: repositoryRoot });
    cache.snapshotByteLimit = 10;
    cache.blobByteLimit = 10;
    const firstSnapshot = {
      derived: new Map(),
      generation: "1".repeat(64),
      map: Buffer.from("123456"),
      project: "cache",
      verification: null,
    };
    const secondSnapshot = {
      derived: new Map(),
      generation: "2".repeat(64),
      map: Buffer.from("abcdef"),
      project: "cache",
      verification: null,
    };
    cache.current = firstSnapshot;
    cache.retainSnapshot(firstSnapshot);
    cache.current = secondSnapshot;
    cache.retainSnapshot(secondSnapshot);
    assert.deepEqual(
      [...cache.snapshots.keys()],
      [secondSnapshot.generation],
      "Node live snapshots exceeded their byte budget",
    );
    assert.deepEqual(
      cache.derived(secondSnapshot, "probe", {}, () => Buffer.from("12345678")),
      Buffer.from("12345678"),
    );
    assert.equal(secondSnapshot.derived.size, 0, "oversize Node derived result was cached");
    const firstBlob = Buffer.from("123456");
    const secondBlob = Buffer.from("abcdef");
    cache.artifact(secondSnapshot, firstBlob);
    const secondDescriptor = cache.artifact(secondSnapshot, secondBlob);
    assert.deepEqual(
      [...cache.blobs.keys()],
      [secondDescriptor.blob_sha256],
      "Node live blobs exceeded their byte budget",
    );
    assert.strictEqual(
      cache.blobs.get(secondDescriptor.blob_sha256),
      secondBlob,
      "Node live artifact copied an existing Buffer",
    );

    const view = await artifact(server, "view", { max_edge_names: 3, max_nodes: 0, view: "files" });
    assert.equal(JSON.parse(view.bytes).artifact, "archbird-graph-view");
    assert.ok(activities.some((row) =>
      row.event === "operation-complete" && row.label === "view files"));
    const symbols = JSON.parse((await artifact(server, "view", {
      max_edge_names: 3,
      max_nodes: 0,
      query: { depth: 0, paths: ["js/index.js"], testDepth: 0 },
      view: "symbols",
    })).bytes);
    assert.equal(symbols.artifact, "archbird-graph-view");
    assert.equal(symbols.request.view, "symbols");
    assert.ok(symbols.nodes.some((row) => row.kind === "symbol" && row.label === "add"));
    const query = JSON.parse((await artifact(server, "query", {
      depth: 0, symbols: ["add"], test_depth: 0,
    })).bytes);
    assert.equal(query.artifact, "query");
    const connection = JSON.parse((await artifact(server, "path", {
      direction: "downstream",
      relations: ["imports"],
      source: { kind: "file", patterns: ["py/pkg/__init__.py"] },
      target: { kind: "file", patterns: ["py/pkg/api.py"] },
    })).bytes);
    assert.equal(connection.artifact, "path");
    assert.equal(connection.outcome, "found");
    assert.ok(activities.some((row) =>
      row.event === "operation-complete" && row.label === "query symbol add"));

    assert.equal(server.repository.state().display, null);
    const displayed = await request(
      server,
      "query",
      { depth: 0, symbols: ["add"], test_depth: 0 },
      { display: {} },
    );
    const displayState = server.repository.state().display;
    assert.equal(displayState.revision, 1);
    assert.equal(displayState.method, "query");
    assert.deepEqual(displayState.params, {
      depth: 0, symbols: ["add"], test_depth: 0,
    });
    assert.deepEqual(displayState.display, {});
    assert.equal(displayState.artifact.blob_sha256, displayed.result.blob_sha256);
    assert.match(displayState.presentation.blob_sha256, /^[0-9a-f]{64}$/);
    assert.equal("session" in displayState, false);
    assert.ok(events.some((value) => value.includes("event: display-updated")));
    assert.ok(activities.some((row) =>
      row.event === "operation-complete" && row.label === "query symbol add · displayed"));
    const displayEvent = JSON.parse(
      events.find((value) => value.includes("event: display-updated"))
        .match(/data: (.+)\n\n$/s)[1],
    );
    assert.deepEqual(displayEvent.payload, displayState);
    assert.equal("session" in displayEvent, false);

    await request(server, "query", { depth: 0, symbols: ["add"], test_depth: 0 });
    assert.equal(
      server.repository.state().display.revision,
      1,
      "private request replaced the displayed artifact",
    );
    await request(server, "path", {}, { display: {}, ok: false });
    assert.equal(
      server.repository.state().display.revision,
      1,
      "failed display request replaced the last successful display",
    );
    await request(
      server,
      "projection",
      { plan: { id: "private", select: "component_membership" } },
      { display: {}, ok: false },
    );

    const source = await request(server, "source", { path: "js/index.js" });
    assert.equal(source.result.encoding, "utf-8");
    assert.match(source.result.text, /export function add/);
    assert.ok(activities.some((row) =>
      row.event === "operation-complete" && row.label === "source js/index.js"));
    const escape = await request(server, "source", { path: "../outside" }, { ok: false });
    assert.match(escape.error.message, /escapes repository root/);

    const initialGeneration = initial.descriptor.generation;
    const configurationPath = path.join(repositoryRoot, "archbird.json");
    const configuration = fs.readFileSync(configurationPath);
    fs.writeFileSync(configurationPath, '{"schema_version":2}\n');
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
    assert.equal(
      server.repository.state().display,
      null,
      "stale display survived a new source generation",
    );
    assert.equal(
      server.repository.state().observed_display,
      null,
      "stale browser observation survived a new source generation",
    );
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

    const opened = await artifact(
      server,
      "open-snapshot",
      { generation: initialGeneration },
      { session: "snapshot-review" },
    );
    assert.equal(opened.descriptor.generation, initialGeneration);
    const selectedMap = await artifact(server, "map", {}, { session: "snapshot-review" });
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
