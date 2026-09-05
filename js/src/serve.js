"use strict";

const fs = require("node:fs");
const crypto = require("node:crypto");
const http = require("node:http");
const path = require("node:path");
const { Worker } = require("node:worker_threads");
const {
  diffMaps,
  evaluateProjection,
  evaluateProjections,
  exportGraph,
  jsonCanonicalize,
  pathMap,
  queryMap,
} = require("./index");

const PROTOCOL_VERSION = 2;
const STATIC_HOST_MARKER = 'name="archbird-host" content="static"';
const SERVER_HOST_MARKER = 'name="archbird-host" content="server"';
const SNAPSHOT_LIMIT = 4;
const DERIVED_LIMIT = 16;
const BLOB_LIMIT = 32;
const SNAPSHOT_BYTE_LIMIT = 512 * 1024 * 1024;
const BLOB_BYTE_LIMIT = 256 * 1024 * 1024;
const BODY_LIMIT = 2 * 1024 * 1024;
const CAPABILITIES = ["map", "query", "path", "diff", "verify", "state", "display", "events"];
const DISPLAY_METHODS = new Set(["diff", "map", "path", "query", "verify"]);
const DEFAULT_SKIPPED_DIRECTORIES = new Set([
  ".git", ".hg", ".svn", ".venv", "__pycache__", "build", "dist", "node_modules",
]);
const MIME = new Map([
  [".css", "text/css; charset=utf-8"],
  [".html", "text/html; charset=utf-8"],
  [".js", "text/javascript; charset=utf-8"],
  [".json", "application/json; charset=utf-8"],
  [".svg", "image/svg+xml"],
  [".wasm", "application/wasm"],
]);

function utf8Compare(left, right) {
  return Buffer.compare(Buffer.from(left), Buffer.from(right));
}

function safeRelative(root, value) {
  if (typeof value !== "string" || !value || value.includes("\0")) {
    throw new Error("source path must be a non-empty repository-relative string");
  }
  const candidate = path.resolve(root, ...value.replaceAll("\\", "/").split("/"));
  const relative = path.relative(root, candidate).split(path.sep).join("/");
  if (!relative || relative === ".." || relative.startsWith("../") || path.isAbsolute(relative)) {
    throw new Error(`source path escapes repository root: ${value}`);
  }
  return { candidate, relative };
}

function appRoot(explicit) {
  const candidates = [
    explicit && path.resolve(explicit),
    path.resolve(__dirname, "../app"),
    path.resolve(__dirname, "../../app/dist"),
  ].filter(Boolean);
  for (const candidate of candidates) {
    try {
      if (fs.lstatSync(path.join(candidate, "index.html")).isFile()) return candidate;
    } catch (error) {
      if (!error || error.code !== "ENOENT") throw error;
    }
  }
  throw new Error("visualization application is unavailable; build app/dist or package app assets");
}

function liveAppBytes(candidate) {
  const bytes = fs.readFileSync(candidate);
  if (!candidate.endsWith("index.html")) return bytes;
  const document = bytes.toString("utf8");
  const first = document.indexOf(STATIC_HOST_MARKER);
  if (first < 0 || document.indexOf(STATIC_HOST_MARKER, first + STATIC_HOST_MARKER.length) >= 0) {
    throw new Error("visualization index must contain exactly one static Archbird host marker");
  }
  return Buffer.from(document.replace(STATIC_HOST_MARKER, SERVER_HOST_MARKER));
}

class LiveRepository {
  constructor({
    root,
    activity = null,
    config = null,
    configJson = null,
    noConfig = false,
    projectOptions = {},
    typescript = true,
    debounceMs = 180,
  }) {
    if (activity !== null && typeof activity !== "function") {
      throw new TypeError("activity must be a function or null");
    }
    this.root = path.resolve(root);
    this.activity = activity;
    this.config = config ? path.resolve(config) : null;
    this.configJson = configJson ? Buffer.from(configJson) : null;
    this.noConfig = noConfig;
    this.projectOptions = { ...projectOptions };
    this.typescript = typescript;
    this.debounceMs = debounceMs;
    this.current = null;
    this.snapshots = new Map();
    this.blobs = new Map();
    this.snapshotByteLimit = SNAPSHOT_BYTE_LIMIT;
    this.blobByteLimit = BLOB_BYTE_LIMIT;
    this.watchers = new Map();
    this.clients = new Set();
    this.eventId = 0;
    this.display = null;
    this.displayRevision = 0;
    this.observedDisplay = null;
    this.observedDisplayRevision = 0;
    this.pendingDisplayDigest = null;
    this.timer = null;
    this.building = null;
    this.dirty = false;
    this.dirtyForce = false;
    this.pendingForce = false;
    this.watchState = null;
    this.phase = "waiting";
    this.lastError = null;
    this.closed = false;
  }

  state() {
    return {
      generation: this.current?.generation || null,
      last_error: this.lastError,
      phase: this.phase,
      project: this.current?.project || null,
      schema_version: this.current?.schemaVersion || null,
      source_available: Boolean(this.current),
      display: this.display,
      observed_display: this.observedDisplay,
    };
  }

  inputSignature() {
    if (!this.current) return null;
    const paths = this.current.files.map((row) => path.join(this.root, ...row.path.split("/")));
    if (this.config) paths.push(this.config);
    const rows = [...new Set(paths)].sort(utf8Compare).map((candidate) => {
      try {
        const stat = fs.lstatSync(candidate, { bigint: true });
        return [
          candidate,
          stat.mode.toString(),
          stat.ino.toString(),
          stat.size.toString(),
          stat.mtimeNs.toString(),
        ];
      } catch (error) {
        if (error && error.code === "ENOENT") return [candidate, null];
        throw error;
      }
    });
    return crypto.createHash("sha256").update(JSON.stringify(rows)).digest("hex");
  }

  event(type, payload, generation = null, session = "server") {
    const event = {
      event_id: this.eventId++,
      ...(generation ? { generation } : {}),
      payload,
      protocol_version: PROTOCOL_VERSION,
      ...(session === null ? {} : { session }),
      type,
    };
    const encoded = `id: ${event.event_id}\nevent: ${type}\ndata: ${JSON.stringify(event)}\n\n`;
    for (const response of this.clients) response.write(encoded);
    this.activityEvent(type, { ...payload, ...(generation ? { generation } : {}) });
  }

  activityEvent(event, payload = {}) {
    if (!this.activity) return;
    try {
      this.activity({ event, ...payload });
    } catch {
      // Activity reporting must not change repository analysis or host responses.
    }
  }

  candidate() {
    return new Promise((resolve, reject) => {
      const worker = new Worker(path.join(__dirname, "serve-worker.js"), {
        workerData: {
          config: this.config,
          configJson: this.configJson,
          noConfig: this.noConfig,
          options: this.projectOptions,
          root: this.root,
          typescript: this.typescript,
        },
      });
      let settled = false;
      worker.once("message", (result) => {
        settled = true;
        if (result.error) reject(new Error(result.error));
        else resolve({
          derived: new Map(),
          files: result.files,
          generation: result.generation,
          map: Buffer.from(result.map),
          project: result.project,
          schemaVersion: result.schemaVersion,
          stored_at: Date.now(),
          verification: result.verification ? Buffer.from(result.verification) : null,
        });
      });
      worker.once("error", (error) => {
        settled = true;
        reject(error);
      });
      worker.once("exit", (code) => {
        if (!settled) reject(new Error(`Map worker exited without a result (status ${code})`));
      });
    });
  }

  async rebuild() {
    if (this.closed) return;
    if (this.building) {
      this.dirty = true;
      return this.building;
    }
    this.phase = "analyzing";
    this.lastError = null;
    this.event("scan-started", { phase: "analyzing", root: "." }, this.current?.generation);
    this.building = this.candidate().then((snapshot) => {
      if (this.closed) return;
      const changed = this.current?.generation !== snapshot.generation;
      this.current = snapshot;
      if (changed) {
        this.display = null;
        if (this.observedDisplay?.workspace === "live") this.observedDisplay = null;
      }
      this.retainSnapshot(snapshot);
      this.syncWatchers(snapshot.files);
      this.watchState = this.inputSignature();
      this.phase = "ready";
      this.lastError = null;
      this.event(
        "snapshot-ready",
        { files: snapshot.files.length, phase: "ready", project: snapshot.project },
        snapshot.generation,
      );
      if (snapshot.verification) {
        this.event(
          "verification-ready",
          { phase: "ready", project: snapshot.project },
          snapshot.generation,
        );
      }
    }).catch((error) => {
      this.phase = "failed";
      this.lastError = String(error.message || error);
      this.event(
        "candidate-failed",
        { message: this.lastError, phase: "failed" },
        this.current?.generation,
      );
      if (!this.current) throw error;
    }).finally(() => {
      this.building = null;
      if (this.dirty && !this.closed) {
        const force = this.dirtyForce;
        this.dirty = false;
        this.dirtyForce = false;
        this.schedule(null, force);
      }
    });
    return this.building;
  }

  schedule(candidate = null, force = false) {
    if (this.closed) return;
    const relative = candidate
      ? path.relative(this.root, path.resolve(candidate)).split(path.sep).join("/")
      : null;
    const known = relative
      ? this.current?.files.some((row) => row.path === relative)
      : false;
    const requiresFullCheck = force || !this.current || !relative || !known;
    if (this.building) {
      this.dirty = true;
      this.dirtyForce ||= requiresFullCheck;
      return;
    }
    this.phase = "stale";
    this.event("progress", { phase: "stale", root: "." }, this.current?.generation);
    this.pendingForce ||= requiresFullCheck;
    clearTimeout(this.timer);
    this.timer = setTimeout(() => {
      const pendingForce = this.pendingForce;
      this.pendingForce = false;
      if (!pendingForce && this.inputSignature() === this.watchState) {
        this.phase = "ready";
        return;
      }
      void this.rebuild().catch(() => {});
    }, this.debounceMs);
  }

  watchDirectory(directory, force = false) {
    const resolved = path.resolve(directory);
    const configuredDirectory = this.config ? path.dirname(this.config) : null;
    if (
      this.watchers.has(resolved)
      || (
        resolved !== this.root
        && resolved !== configuredDirectory
        && !resolved.startsWith(`${this.root}${path.sep}`)
      )
    ) return;
    if (!force && DEFAULT_SKIPPED_DIRECTORIES.has(path.basename(resolved))) return;
    let watcher;
    try {
      watcher = fs.watch(resolved, { persistent: false }, (_event, filename) => {
        if (filename) {
          const candidate = path.join(resolved, filename.toString());
          try {
            if (fs.lstatSync(candidate).isDirectory()) this.watchDirectory(candidate);
          } catch (error) {
            if (!error || error.code !== "ENOENT") this.event("candidate-failed", { message: error.message });
          }
        }
        this.schedule(filename ? path.join(resolved, filename.toString()) : null);
      });
    } catch (error) {
      if (!error || error.code !== "ENOENT") throw error;
      return;
    }
    watcher.on("error", (error) => this.event("candidate-failed", { message: error.message }));
    this.watchers.set(resolved, watcher);
  }

  syncWatchers(files) {
    const required = new Set([this.root]);
    if (this.config) required.add(path.dirname(this.config));
    for (const row of files) {
      let directory = path.dirname(path.join(this.root, ...row.path.split("/")));
      while (directory === this.root || directory.startsWith(`${this.root}${path.sep}`)) {
        required.add(directory);
        if (directory === this.root) break;
        directory = path.dirname(directory);
      }
    }
    for (const directory of required) this.watchDirectory(directory, true);
    for (const [directory, watcher] of this.watchers) {
      if (!required.has(directory)) {
        watcher.close();
        this.watchers.delete(directory);
      }
    }
  }

  snapshot(generation = null) {
    const selected = generation ? this.snapshots.get(generation) : this.current;
    if (!selected) throw new Error(`snapshot is unavailable: ${generation || "current"}`);
    return selected;
  }

  snapshotBytes(snapshot) {
    return snapshot.map.length
      + (snapshot.verification?.length || 0)
      + [...snapshot.derived.values()].reduce((total, value) => total + value.length, 0);
  }

  pruneSnapshots() {
    while (this.snapshots.size) {
      const retainedBytes = [...this.snapshots.values()]
        .reduce((total, snapshot) => total + this.snapshotBytes(snapshot), 0);
      const overCount = this.snapshots.size > SNAPSHOT_LIMIT;
      const overBytes = retainedBytes > this.snapshotByteLimit;
      if (!overCount && !overBytes) return;
      const removable = [...this.snapshots.keys()]
        .find((generation) => generation !== this.current?.generation);
      if (removable) {
        this.snapshots.delete(removable);
        continue;
      }
      if (overBytes && this.current?.derived.size) {
        this.current.derived.delete(this.current.derived.keys().next().value);
        continue;
      }
      return;
    }
  }

  retainSnapshot(snapshot) {
    // Keep recent generations without multiplying one large Map in memory.
    this.snapshots.delete(snapshot.generation);
    this.snapshots.set(snapshot.generation, snapshot);
    this.pruneSnapshots();
  }

  artifact(snapshot, bytes) {
    const encoded = Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes);
    const sha256 = crypto.createHash("sha256").update(encoded).digest("hex");
    this.blobs.delete(sha256);
    this.blobs.set(sha256, encoded);
    const blobBytes = () => [...this.blobs.values()]
      .reduce((total, value) => total + value.length, 0);
    while (
      this.blobs.size > 1
      && (this.blobs.size > BLOB_LIMIT || blobBytes() > this.blobByteLimit)
    ) {
      const protectedDigests = new Set([sha256, this.pendingDisplayDigest]);
      for (const name of ["artifact", "presentation"]) {
        protectedDigests.add(this.display?.[name]?.blob_sha256);
      }
      const removable = [...this.blobs.keys()]
        .find((digest) => !protectedDigests.has(digest));
      if (!removable) break;
      this.blobs.delete(removable);
    }
    return {
      blob_sha256: sha256,
      bytes: encoded.length,
      generation: snapshot.generation,
      project: snapshot.project,
    };
  }

  publishDisplay(request, result) {
    if (!result || typeof result !== "object" || Array.isArray(result)
        || typeof result.blob_sha256 !== "string") {
      throw new Error("display requires an artifact-producing operation");
    }
    if (!this.blobs.has(result.blob_sha256)) {
      throw new Error("display artifact is unavailable");
    }
    let presentation = null;
    this.pendingDisplayDigest = result.blob_sha256;
    try {
      if (request.method === "query") {
        const bytes = this.blobs.get(result.blob_sha256);
        if (!bytes) throw new Error("display artifact is unavailable");
        const snapshot = this.snapshot(result.generation);
        presentation = this.artifact(snapshot, exportGraph(bytes, {
          format: "json",
          maxEdgeNames: 3,
          maxNodes: 0,
          view: "symbols",
        }));
      }
    } finally {
      this.pendingDisplayDigest = null;
    }
    this.display = {
      artifact: { ...result },
      display: { ...request.display },
      generation: result.generation,
      method: request.method,
      params: { ...request.params },
      ...(presentation ? { presentation } : {}),
      revision: ++this.displayRevision,
      ...(request.session === null ? {} : { session: request.session }),
    };
    this.event("display-updated", this.display, result.generation, request.session);
    return this.display;
  }

  observeDisplay(value, session) {
    if (!session) throw new Error("observed display requires a session");
    const observed = validateObservedDisplay(value);
    this.snapshot(observed.generation);
    this.observedDisplay = {
      ...observed,
      revision: ++this.observedDisplayRevision,
      session,
    };
    return this.observedDisplay;
  }

  derived(snapshot, operation, request, evaluate) {
    const canonical = jsonCanonicalize(Buffer.from(JSON.stringify(request)));
    const digest = crypto.createHash("sha256").update(canonical).digest("hex");
    const key = `${operation}:${digest}`;
    const cached = snapshot.derived.get(key);
    if (cached) {
      snapshot.derived.delete(key);
      snapshot.derived.set(key, cached);
      return cached;
    }
    const bytes = Buffer.from(evaluate());
    const baseBytes = snapshot.map.length + (snapshot.verification?.length || 0);
    if (bytes.length > Math.max(0, this.snapshotByteLimit - baseBytes)) return bytes;
    snapshot.derived.set(key, bytes);
    while (snapshot.derived.size > DERIVED_LIMIT) {
      snapshot.derived.delete(snapshot.derived.keys().next().value);
    }
    this.pruneSnapshots();
    return bytes;
  }

  source(snapshot, sourcePath) {
    const allowed = new Map(snapshot.files.map((row) => [row.path, row]));
    const { candidate, relative } = safeRelative(this.root, sourcePath);
    const evidence = allowed.get(relative);
    if (!evidence) throw new Error(`source is not mapped in this generation: ${relative}`);
    const metadata = fs.lstatSync(candidate);
    if (!metadata.isFile() || metadata.isSymbolicLink()) {
      throw new Error(`source is not a regular current file: ${relative}`);
    }
    const bytes = fs.readFileSync(candidate);
    const digest = crypto.createHash("sha256").update(bytes).digest("hex");
    if (digest !== evidence.sha256) {
      throw new Error(`source changed after selected generation: ${relative}`);
    }
    let utf8 = true;
    try {
      new TextDecoder("utf-8", { fatal: true }).decode(bytes);
    } catch {
      utf8 = false;
    }
    let visible = bytes.subarray(0, 256 * 1024);
    if (utf8 && visible.length < bytes.length) {
      while (visible.length) {
        try {
          new TextDecoder("utf-8", { fatal: true }).decode(visible);
          break;
        } catch {
          visible = visible.subarray(0, visible.length - 1);
        }
      }
    }
    return {
      bytes: bytes.length,
      encoding: utf8 ? "utf-8" : "hex",
      path: relative,
      sha256: digest,
      text: utf8 ? visible.toString("utf8") : visible.subarray(0, 64 * 1024).toString("hex"),
      truncated: bytes.length > (utf8 ? visible.length : Math.min(visible.length, 64 * 1024)),
    };
  }

  async close() {
    this.closed = true;
    clearTimeout(this.timer);
    for (const watcher of this.watchers.values()) watcher.close();
    this.watchers.clear();
    for (const response of this.clients) response.end();
    this.clients.clear();
    await this.building;
  }
}

function securityHeaders(response) {
  response.setHeader("Content-Security-Policy", "default-src 'self'; connect-src 'self'; img-src 'self' data:; object-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; worker-src 'self' blob:; base-uri 'none'; frame-ancestors 'none'");
  response.setHeader("Cross-Origin-Opener-Policy", "same-origin");
  response.setHeader("Referrer-Policy", "no-referrer");
  response.setHeader("X-Content-Type-Options", "nosniff");
  response.setHeader("X-Frame-Options", "DENY");
}

function json(response, status, value) {
  const bytes = Buffer.from(JSON.stringify(value));
  response.writeHead(status, {
    "Cache-Control": "no-store",
    "Content-Length": bytes.length,
    "Content-Type": "application/json; charset=utf-8",
  });
  response.end(bytes);
}

function readJson(request) {
  return new Promise((resolve, reject) => {
    const parts = [];
    let length = 0;
    request.on("data", (part) => {
      length += part.length;
      if (length > BODY_LIMIT) {
        reject(new Error(`request body exceeds ${BODY_LIMIT} bytes`));
        request.destroy();
      } else parts.push(part);
    });
    request.on("error", reject);
    request.on("end", () => {
      try {
        resolve(JSON.parse(Buffer.concat(parts).toString("utf8")));
      } catch (error) {
        reject(new Error(`request is not valid JSON: ${error.message}`));
      }
    });
  });
}

function validateRequest(value) {
  if (!value || Array.isArray(value) || typeof value !== "object") throw new Error("request must be an object");
  const unknown = Object.keys(value).filter((key) =>
    !["display", "method", "params", "session"].includes(key)).sort();
  if (unknown.length) throw new Error(`request contains unsupported field: ${unknown[0]}`);
  if (typeof value.method !== "string" || !value.method) {
    throw new Error("request.method is required");
  }
  const params = value.params ?? {};
  if (!params || Array.isArray(params) || typeof params !== "object") {
    throw new Error("request.params must be an object");
  }
  if (value.session !== undefined
      && (typeof value.session !== "string" || !value.session)) {
    throw new Error("request.session must be a non-empty string when supplied");
  }
  if (value.display !== undefined && value.display !== null
      && (Array.isArray(value.display) || typeof value.display !== "object")) {
    throw new Error("request.display must be an object or null");
  }
  if (value.display !== undefined && value.display !== null
      && !DISPLAY_METHODS.has(value.method)) {
    throw new Error(`request.display is unsupported for method ${value.method}`);
  }
  return {
    display: value.display ?? null,
    method: value.method,
    params: { ...params },
    session: value.session ?? null,
  };
}

function displayString(value, label) {
  if (typeof value !== "string" || !value || value.length > 4096) {
    throw new Error(`${label} must be a non-empty string of at most 4096 characters`);
  }
  return value;
}

function displayObject(value, label, keys) {
  if (!value || Array.isArray(value) || typeof value !== "object") {
    throw new Error(`${label} must be an object`);
  }
  const unknown = Object.keys(value).filter((key) => !keys.includes(key)).sort();
  if (unknown.length) throw new Error(`${label} contains unsupported field: ${unknown[0]}`);
  return value;
}

function displayCount(value, label) {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new Error(`${label} must be a nonnegative safe integer`);
  }
  return value;
}

function displayIds(value, label) {
  if (!Array.isArray(value) || value.length > 128) {
    throw new Error(`${label} must contain at most 128 identifiers`);
  }
  return value.map((item, index) => displayString(item, `${label}[${index}]`));
}

function validateObservedDisplay(value) {
  const row = displayObject(value, "observed display", [
    "artifact", "expanded", "generation", "graph", "hidden", "history_depth",
    "selected", "selected_constraint", "source", "workspace",
  ]);
  const generation = displayString(row.generation, "observed display generation");
  if (!/^[0-9a-f]{64}$/.test(generation)) {
    throw new Error("observed display generation must be a SHA-256 digest");
  }
  const workspace = displayString(row.workspace, "observed display workspace");
  if (!["live", "snapshot"].includes(workspace)) {
    throw new Error("observed display workspace is invalid");
  }
  const expanded = displayObject(row.expanded, "observed display expanded state", [
    "files", "groups", "omitted", "symbols",
  ]);
  const hidden = displayObject(row.hidden, "observed display hidden state", ["ids", "omitted"]);
  let graph = null;
  if (row.graph !== null) {
    const input = displayObject(row.graph, "observed display graph", [
      "direction", "edge_classification", "edge_flow", "edge_kind", "edges",
      "group_by", "layout", "nodes", "notation", "perspective", "view",
    ]);
    graph = {
      direction: displayString(input.direction, "observed display graph direction"),
      edge_classification: displayString(input.edge_classification, "observed display edge classification"),
      edge_flow: displayString(input.edge_flow, "observed display edge flow"),
      edge_kind: displayString(input.edge_kind, "observed display edge kind"),
      edges: displayCount(input.edges, "observed display graph edges"),
      group_by: displayString(input.group_by, "observed display graph grouping"),
      layout: displayString(input.layout, "observed display graph layout"),
      nodes: displayCount(input.nodes, "observed display graph nodes"),
      notation: displayString(input.notation, "observed display graph notation"),
      perspective: displayString(input.perspective, "observed display graph perspective"),
      view: displayString(input.view, "observed display graph view"),
    };
  }
  let selected = null;
  if (row.selected !== null) {
    const input = displayObject(row.selected, "observed display selection", [
      "id", "identity", "kind", "label", "path",
    ]);
    selected = {
      id: displayString(input.id, "observed display selection id"),
      identity: displayString(input.identity, "observed display selection identity"),
      kind: displayString(input.kind, "observed display selection kind"),
      label: displayString(input.label, "observed display selection label"),
      ...(input.path === undefined ? {} : {
        path: displayString(input.path, "observed display selection path"),
      }),
    };
  }
  let source = null;
  if (row.source !== null) {
    const input = displayObject(row.source, "observed display source", ["end", "path", "start"]);
    source = {
      path: displayString(input.path, "observed display source path"),
      ...(input.start === undefined ? {} : {
        start: displayCount(input.start, "observed display source start"),
      }),
      ...(input.end === undefined ? {} : {
        end: displayCount(input.end, "observed display source end"),
      }),
    };
    if ((source.start === undefined) !== (source.end === undefined)
        || (source.start !== undefined && source.end <= source.start)) {
      throw new Error("observed display source range is invalid");
    }
  }
  return {
    artifact: displayString(row.artifact, "observed display artifact"),
    expanded: {
      files: displayIds(expanded.files, "observed display expanded files"),
      groups: displayIds(expanded.groups, "observed display expanded groups"),
      omitted: displayCount(expanded.omitted, "observed display omitted expansions"),
      symbols: displayIds(expanded.symbols, "observed display revealed symbols"),
    },
    generation,
    graph,
    hidden: {
      ids: displayIds(hidden.ids, "observed display hidden identifiers"),
      omitted: displayCount(hidden.omitted, "observed display omitted hidden identifiers"),
    },
    history_depth: displayCount(row.history_depth, "observed display history depth"),
    selected,
    selected_constraint: row.selected_constraint === null
      ? null
      : displayString(row.selected_constraint, "observed display selected constraint"),
    source,
    workspace,
  };
}

function internalRequest(request) {
  const method = request.method === "verify" ? "verification" : request.method;
  const params = { ...request.params };
  let payload = params;
  if (method === "query") {
    const generation = params.generation;
    delete params.generation;
    payload = { query: params, ...(generation === undefined ? {} : { generation }) };
  }
  return { method, payload, session: request.session };
}

function activityText(value, limit = 120) {
  const text = String(value).replace(/\s+/g, " ").trim();
  return text.length <= limit ? text : `${text.slice(0, limit - 1)}…`;
}

function operationLabel(request) {
  const payload = request.payload || {};
  if (request.method === "projection") {
    const plan = payload.plan || {};
    const select = String(plan.select || "projection");
    if (select === "metrics") return `metric ${activityText(plan.metric || plan.id || "unknown")}`;
    if (select === "graph") {
      const grouping = String(plan.group_by || "structure");
      const id = String(plan.id || "");
      const suffix = `-${grouping}`;
      const view = id.startsWith("app-") && id.endsWith(suffix)
        ? id.slice(4, -suffix.length)
        : id.replace(/^app-/, "") || "architecture";
      return `view ${view} · ${grouping}`;
    }
    const identity = activityText(plan.id || "");
    return `projection ${select}${identity ? ` · ${identity}` : ""}`;
  }
  if (request.method === "projections") {
    return `projections ${Array.isArray(payload.plans) ? payload.plans.length : 0}`;
  }
  if (request.method === "view") return `view ${String(payload.view || "components")}`;
  if (request.method === "query") {
    const query = payload.query || {};
    for (const [key, label] of [
      ["symbols", "symbol"], ["paths", "path"], ["components", "component"],
      ["packages", "package"], ["artifacts", "artifact"], ["search", "search"],
    ]) {
      const value = query[key];
      const selected = Array.isArray(value) ? value[0] : value;
      if (selected !== undefined && selected !== null && String(selected)) {
        return `query ${label} ${activityText(selected)}`;
      }
    }
    return "query";
  }
  if (request.method === "map") return "map";
  if (request.method === "path") {
    const source = payload.source && typeof payload.source === "object" ? payload.source : {};
    const target = payload.target && typeof payload.target === "object" ? payload.target : {};
    const sourceName = source.identity || source.name || "unknown";
    const targetName = target.identity || target.name || "unknown";
    return `path ${activityText(sourceName)} → ${activityText(targetName)}`;
  }
  if (request.method === "source") return `source ${activityText(payload.path || "unknown")}`;
  if (request.method === "verification") return "verification";
  if (request.method === "diff") {
    return `diff ${String(payload.before || "unknown").slice(0, 12)} → ${String(payload.after || "unknown").slice(0, 12)}`;
  }
  if (request.method === "export") {
    return `export ${String(payload.view || "components")} · ${String(payload.format || "graphml")}`;
  }
  if (request.method === "open-snapshot") {
    return `snapshot ${String(payload.generation || "unknown").slice(0, 12)}`;
  }
  return null;
}

function requestSnapshot(repository, selections, request) {
  const generation = request.payload.generation
    || (request.session ? selections.get(request.session) : null) || null;
  return repository.snapshot(generation);
}

async function hostRequest(repository, selections, request) {
  if (request.method === "bootstrap") {
    return {
      capabilities: CAPABILITIES,
      engine: "native",
    };
  }
  if (request.method === "state") {
    const keys = Object.keys(request.payload);
    if (keys.some((key) => key !== "observed_display")) {
      throw new Error(`state request contains unsupported parameter: ${keys.sort()[0]}`);
    }
    if (Object.prototype.hasOwnProperty.call(request.payload, "observed_display")) {
      repository.observeDisplay(request.payload.observed_display, request.session);
    }
    return repository.state();
  }
  if (request.method === "snapshots") {
    return [...repository.snapshots.values()]
      .sort((left, right) => right.stored_at - left.stored_at || utf8Compare(left.generation, right.generation))
      .map((row) => ({
        files: row.files.length,
        generation: row.generation,
        project: row.project,
        schema_version: row.schemaVersion,
        stored_at: row.stored_at,
      }));
  }
  if (request.method === "open-snapshot") {
    const snapshot = repository.snapshot(request.payload.generation);
    if (request.session) selections.set(request.session, snapshot.generation);
    return repository.artifact(snapshot, snapshot.map);
  }
  const snapshot = requestSnapshot(repository, selections, request);
  if (request.method === "map") return repository.artifact(snapshot, snapshot.map);
  if (request.method === "projection") {
    const plan = request.payload.plan;
    if (!plan || typeof plan !== "object" || Array.isArray(plan)) {
      throw new Error("projection request requires a plan object");
    }
    return repository.artifact(
      snapshot,
      repository.derived(snapshot, "projection", plan, () =>
        evaluateProjection(snapshot.map, Buffer.from(JSON.stringify(plan)))),
    );
  }
  if (request.method === "projections") {
    const plans = request.payload.plans;
    if (!Array.isArray(plans) || plans.some((plan) =>
      !plan || typeof plan !== "object" || Array.isArray(plan))) {
      throw new Error("projections request requires plan objects");
    }
    return repository.artifact(
      snapshot,
      repository.derived(snapshot, "projections", plans, () =>
        evaluateProjections(snapshot.map, Buffer.from(JSON.stringify(plans)))),
    );
  }
  if (request.method === "view") {
    const view = request.payload.view || "components";
    const input = view === "symbols"
      ? queryMap(snapshot.map, request.payload.query || {})
      : snapshot.map;
    return repository.artifact(snapshot, exportGraph(input, {
      format: "json",
      maxEdgeNames: Number(request.payload.max_edge_names ?? 3),
      maxNodes: Number(request.payload.max_nodes ?? 0),
      view,
    }));
  }
  if (request.method === "export") {
    const view = request.payload.view || "components";
    const input = view === "symbols"
      ? queryMap(snapshot.map, request.payload.query || {})
      : snapshot.map;
    return repository.artifact(snapshot, exportGraph(input, {
      direction: request.payload.direction || "LR",
      format: request.payload.format || "graphml",
      maxEdgeNames: Number(request.payload.max_edge_names ?? 0),
      maxNodes: Number(request.payload.max_nodes ?? 0),
      view,
    }));
  }
  if (request.method === "query") {
    return repository.artifact(snapshot, queryMap(snapshot.map, request.payload.query || {}));
  }
  if (request.method === "path") {
    const { source, target } = request.payload;
    if (!source || Array.isArray(source) || typeof source !== "object"
        || !target || Array.isArray(target) || typeof target !== "object") {
      throw new Error("path request requires source and target objects");
    }
    return repository.artifact(snapshot, pathMap(snapshot.map, {
      direction: request.payload.direction || "downstream",
      level: request.payload.level || "file",
      maxDepth: Number(request.payload.max_depth ?? 8),
      maxPaths: Number(request.payload.max_paths ?? 8),
      producerPolicy: request.payload.producer_policy || "compatible",
      relations: request.payload.relations ?? null,
      source,
      target,
    }));
  }
  if (request.method === "diff") {
    const before = repository.snapshot(request.payload.before);
    const after = repository.snapshot(request.payload.after);
    return repository.artifact(after, diffMaps(before.map, after.map));
  }
  if (request.method === "source") return repository.source(snapshot, request.payload.path);
  if (request.method === "verification") {
    return snapshot.verification
      ? repository.artifact(snapshot, snapshot.verification)
      : null;
  }
  if (request.method === "dispose") {
    if (request.session) selections.delete(request.session);
    return { disposed: true };
  }
  throw new Error(`unsupported visualization method: ${request.method}`);
}

async function createLiveServer({
  root = ".",
  activity = null,
  config = null,
  configJson = null,
  noConfig = false,
  projectOptions = {},
  app = null,
  host = "127.0.0.1",
  port = 4177,
  typescript = true,
} = {}) {
  if (!["127.0.0.1", "::1"].includes(host)) {
    throw new Error("live server binds only to 127.0.0.1 or ::1");
  }
  const repository = new LiveRepository({
    root,
    activity,
    config,
    configJson,
    noConfig,
    projectOptions,
    typescript,
  });
  const staticRoot = appRoot(app);
  liveAppBytes(path.join(staticRoot, "index.html"));
  const selections = new Map();
  const server = http.createServer(async (request, response) => {
    securityHeaders(response);
    const hostHeader = request.headers.host || "";
    const allowedHosts = new Set([`127.0.0.1:${server.address().port}`, `localhost:${server.address().port}`]);
    if (host === "::1") allowedHosts.add(`[::1]:${server.address().port}`);
    if (!allowedHosts.has(hostHeader)) {
      response.writeHead(403);
      response.end("forbidden host");
      return;
    }
    const origin = request.headers.origin;
    if (origin && origin !== `http://${hostHeader}`) {
      response.writeHead(403);
      response.end("forbidden origin");
      return;
    }
    let pathname;
    try {
      pathname = decodeURIComponent(new URL(request.url || "/", `http://${hostHeader}`).pathname);
    } catch {
      response.writeHead(400);
      response.end("invalid request path");
      return;
    }
    if (pathname === "/api/v1/bootstrap" && request.method === "GET") {
      json(response, 200, {
        capabilities: CAPABILITIES,
        protocol_version: PROTOCOL_VERSION,
        ...repository.state(),
      });
      return;
    }
    if (pathname === "/api/v1/events" && request.method === "GET") {
      response.writeHead(200, {
        "Cache-Control": "no-store",
        "Connection": "keep-alive",
        "Content-Type": "text/event-stream; charset=utf-8",
      });
      repository.clients.add(response);
      response.write(": archbird live events\n\n");
      request.on("close", () => repository.clients.delete(response));
      return;
    }
    if (pathname.startsWith("/api/v1/blobs/") && request.method === "GET") {
      const digest = pathname.slice("/api/v1/blobs/".length);
      const bytes = /^[0-9a-f]{64}$/.test(digest) ? repository.blobs.get(digest) : null;
      if (!bytes) {
        response.writeHead(404);
        response.end("unknown artifact blob");
        return;
      }
      response.writeHead(200, {
        "Cache-Control": "private, max-age=31536000, immutable",
        "Content-Length": bytes.length,
        "Content-Type": "application/json; charset=utf-8",
        "X-Content-SHA256": digest,
      });
      response.end(bytes);
      return;
    }
    if (pathname === "/api/v1/request" && request.method === "POST") {
      let envelope;
      let raw = null;
      let hostEnvelope = null;
      let label = null;
      const started = process.hrtime.bigint();
      try {
        raw = await readJson(request);
        const requestEnvelope = validateRequest(raw);
        hostEnvelope = internalRequest(requestEnvelope);
        label = operationLabel(hostEnvelope);
        if (label && requestEnvelope.display !== null) label += " · displayed";
        const result = await hostRequest(repository, selections, hostEnvelope);
        if (requestEnvelope.display !== null) {
          repository.publishDisplay(requestEnvelope, result);
        }
        envelope = {
          generation: repository.current?.generation,
          ok: true,
          protocol_version: PROTOCOL_VERSION,
          result,
          ...(requestEnvelope.session === null ? {} : { session: requestEnvelope.session }),
        };
        if (label) {
          repository.activityEvent("operation-complete", {
            duration_ms: Number(process.hrtime.bigint() - started) / 1e6,
            label,
            method: requestEnvelope.method,
          });
        }
      } catch (error) {
        const identity = hostEnvelope || raw || {};
        envelope = {
          error: { code: "host-error", message: String(error.message || error) },
          ok: false,
          protocol_version: PROTOCOL_VERSION,
          ...(typeof identity.session === "string" ? { session: identity.session } : {}),
        };
        if (label) {
          repository.activityEvent("operation-failed", {
            duration_ms: Number(process.hrtime.bigint() - started) / 1e6,
            label,
            message: String(error.message || error),
            method: hostEnvelope.method,
          });
        }
      }
      json(response, envelope.ok ? 200 : 400, envelope);
      return;
    }
    if (request.method !== "GET" && request.method !== "HEAD") {
      response.writeHead(405, { Allow: "GET, HEAD, POST" });
      response.end();
      return;
    }
    const relative = pathname === "/" ? "index.html" : pathname.replace(/^\/+/, "");
    let candidate = path.resolve(staticRoot, relative);
    if (candidate !== staticRoot && !candidate.startsWith(`${staticRoot}${path.sep}`)) {
      response.writeHead(403);
      response.end("forbidden");
      return;
    }
    try {
      if (!fs.lstatSync(candidate).isFile() || fs.lstatSync(candidate).isSymbolicLink()) throw new Error("not-file");
    } catch {
      if (path.extname(relative)) {
        response.writeHead(404);
        response.end("not found");
        return;
      }
      candidate = path.join(staticRoot, "index.html");
    }
    const bytes = liveAppBytes(candidate);
    response.writeHead(200, {
      "Cache-Control": candidate.endsWith("index.html") ? "no-cache" : "public, max-age=31536000, immutable",
      "Content-Length": bytes.length,
      "Content-Type": MIME.get(path.extname(candidate)) || "application/octet-stream",
    });
    if (request.method === "HEAD") response.end();
    else response.end(bytes);
  });
  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(port, host, resolve);
  });
  repository.watchDirectory(repository.root, true);
  if (repository.config) repository.watchDirectory(path.dirname(repository.config), true);
  void repository.rebuild().catch(() => {});
  const address = server.address();
  const url = `http://${host === "::1" ? "[::1]" : host}:${address.port}/`;
  return {
    close: async () => {
      await repository.close();
      await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
    },
    repository,
    server,
    url,
  };
}

module.exports = { LiveRepository, createLiveServer };
