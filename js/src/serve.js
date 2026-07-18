"use strict";

const fs = require("node:fs");
const crypto = require("node:crypto");
const http = require("node:http");
const path = require("node:path");
const { Worker } = require("node:worker_threads");
const { diffMaps, exportGraph, queryMap } = require("./index");

const PROTOCOL_VERSION = 1;
const SNAPSHOT_LIMIT = 4;
const BODY_LIMIT = 2 * 1024 * 1024;
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

class LiveRepository {
  constructor({
    root,
    config = null,
    configJson = null,
    noConfig = false,
    projectOptions = {},
    typescript = true,
    debounceMs = 180,
  }) {
    this.root = path.resolve(root);
    this.config = config ? path.resolve(config) : null;
    this.configJson = configJson ? Buffer.from(configJson) : null;
    this.noConfig = noConfig;
    this.projectOptions = { ...projectOptions };
    this.typescript = typescript;
    this.debounceMs = debounceMs;
    this.current = null;
    this.snapshots = new Map();
    this.blobs = new Map();
    this.watchers = new Map();
    this.clients = new Set();
    this.eventId = 0;
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
      source_available: Boolean(this.current),
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

  event(type, payload, generation = null) {
    const event = {
      event_id: this.eventId++,
      ...(generation ? { generation } : {}),
      payload,
      protocol_version: PROTOCOL_VERSION,
      session: "server",
      type,
    };
    const encoded = `id: ${event.event_id}\nevent: ${type}\ndata: ${JSON.stringify(event)}\n\n`;
    for (const response of this.clients) response.write(encoded);
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
          files: result.files,
          generation: result.generation,
          map: Buffer.from(result.map),
          project: result.project,
          stored_at: Date.now(),
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
      const changed = !this.current || snapshot.generation !== this.current.generation;
      this.current = snapshot;
      this.snapshots.set(snapshot.generation, snapshot);
      const ordered = [...this.snapshots.values()]
        .sort((left, right) => right.stored_at - left.stored_at || utf8Compare(left.generation, right.generation));
      for (const expired of ordered.slice(SNAPSHOT_LIMIT)) this.snapshots.delete(expired.generation);
      this.syncWatchers(snapshot.files);
      this.watchState = this.inputSignature();
      this.phase = "ready";
      this.lastError = null;
      if (changed) {
        this.event(
          "snapshot-ready",
          { files: snapshot.files.length, phase: "ready", project: snapshot.project },
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

  artifact(snapshot, bytes) {
    const encoded = Buffer.from(bytes);
    const sha256 = crypto.createHash("sha256").update(encoded).digest("hex");
    this.blobs.delete(sha256);
    this.blobs.set(sha256, encoded);
    while (this.blobs.size > 32) this.blobs.delete(this.blobs.keys().next().value);
    return {
      blob_sha256: sha256,
      bytes: encoded.length,
      generation: snapshot.generation,
      project: snapshot.project,
    };
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
  if (value.protocol_version !== PROTOCOL_VERSION) throw new Error("unsupported visualization protocol");
  for (const name of ["id", "method", "session"]) {
    if (typeof value[name] !== "string" || !value[name]) throw new Error(`request.${name} is required`);
  }
  if (!value.payload || Array.isArray(value.payload) || typeof value.payload !== "object") {
    throw new Error("request.payload must be an object");
  }
  return value;
}

function requestSnapshot(repository, selections, request) {
  const generation = request.payload.generation || selections.get(request.session) || null;
  return repository.snapshot(generation);
}

async function hostRequest(repository, selections, request) {
  if (request.method === "bootstrap") {
    return { capabilities: ["map", "view", "query", "diff", "source", "snapshots", "events"], engine: "native" };
  }
  if (request.method === "state") {
    const snapshot = repository.current;
    return snapshot ? { generation: snapshot.generation, project: snapshot.project, source_available: true } : { generation: null, project: null, source_available: false };
  }
  if (request.method === "snapshots") {
    return [...repository.snapshots.values()]
      .sort((left, right) => right.stored_at - left.stored_at || utf8Compare(left.generation, right.generation))
      .map(({ map: _map, ...row }) => ({ files: row.files.length, generation: row.generation, project: row.project, stored_at: row.stored_at }));
  }
  if (request.method === "open-snapshot") {
    const snapshot = repository.snapshot(request.payload.generation);
    selections.set(request.session, snapshot.generation);
    return repository.artifact(snapshot, snapshot.map);
  }
  const snapshot = requestSnapshot(repository, selections, request);
  if (request.method === "map") return repository.artifact(snapshot, snapshot.map);
  if (request.method === "view") {
    return repository.artifact(snapshot, exportGraph(snapshot.map, {
      format: "json",
      maxEdgeNames: Number(request.payload.max_edge_names ?? 3),
      maxNodes: Number(request.payload.max_nodes ?? 0),
      view: request.payload.view || "components",
    }));
  }
  if (request.method === "query") {
    return repository.artifact(snapshot, queryMap(snapshot.map, request.payload.query || {}));
  }
  if (request.method === "diff") {
    const before = repository.snapshot(request.payload.before);
    const after = repository.snapshot(request.payload.after);
    return repository.artifact(after, diffMaps(before.map, after.map));
  }
  if (request.method === "source") return repository.source(snapshot, request.payload.path);
  if (["verification", "act-proposal", "act-contract", "act-result"].includes(request.method)) return null;
  if (request.method === "dispose") {
    selections.delete(request.session);
    return { disposed: true };
  }
  throw new Error(`unsupported visualization method: ${request.method}`);
}

async function createLiveServer({
  root = ".",
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
    config,
    configJson,
    noConfig,
    projectOptions,
    typescript,
  });
  const staticRoot = appRoot(app);
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
        capabilities: ["map", "view", "query", "diff", "source", "snapshots", "events"],
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
      try {
        raw = await readJson(request);
        hostEnvelope = validateRequest(raw);
        const result = await hostRequest(repository, selections, hostEnvelope);
        envelope = {
          generation: repository.current?.generation,
          id: hostEnvelope.id,
          ok: true,
          protocol_version: PROTOCOL_VERSION,
          result,
          session: hostEnvelope.session,
        };
      } catch (error) {
        const identity = hostEnvelope || raw || {};
        envelope = {
          error: { code: "host-error", message: String(error.message || error) },
          id: typeof identity.id === "string" ? identity.id : "invalid",
          ok: false,
          protocol_version: PROTOCOL_VERSION,
          session: typeof identity.session === "string" ? identity.session : "invalid",
        };
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
    const bytes = fs.readFileSync(candidate);
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
