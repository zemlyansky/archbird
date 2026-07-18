/// <reference lib="webworker" />

import browser from "archbird/browser";
import {
  VISUALIZATION_PROTOCOL_VERSION,
  type HostEvent,
  type HostRequest,
  type HostResponse,
  type SnapshotSummary,
} from "../adapters/protocol";
import { readDirectoryFiles, readZipFile, type RepositoryInputFile } from "../sources/input";

interface BrowserRuntime {
  Project: {
    fromFiles(sources: unknown[], options?: Record<string, unknown>): BrowserProject;
  };
  Source: new (path: string, data: Uint8Array) => unknown;
  VERSION: string;
  core: {
    mapDiff(before: Uint8Array, after: Uint8Array, pretty?: boolean): Uint8Array;
    mapExportGraph(
      artifact: Uint8Array,
      format: "json",
      view: "components" | "files" | "symbols",
      direction: "LR",
      maxNodes: number,
      maxEdgeNames: number,
    ): Uint8Array;
    sha256(bytes: Uint8Array): string;
  };
}

interface BrowserProject {
  dispose(): void;
  graphViewJson(options: Record<string, unknown>): Uint8Array;
  mapJson(options?: { pretty?: boolean }): Uint8Array;
  queryJson(options?: Record<string, unknown>): Uint8Array;
}

interface SnapshotRecord extends SnapshotSummary {
  bytes: ArrayBuffer;
}

interface CurrentGeneration {
  files: Map<string, Uint8Array>;
  generation: string;
  map: Uint8Array;
  project: BrowserProject | null;
  projectName: string;
}

const context = self as unknown as DedicatedWorkerGlobalScope;
const SNAPSHOT_DATABASE = "archbird-visualization";
const SNAPSHOT_STORE = "snapshots";
const SNAPSHOT_LIMIT = 4;
let runtimePromise: Promise<BrowserRuntime> | null = null;
let current: CurrentGeneration | null = null;
let activeSession = "";
let eventId = 0;
const memorySnapshots = new Map<string, SnapshotRecord>();

function cloneBytes(bytes: Uint8Array): ArrayBuffer {
  return bytes.slice().buffer;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function emit(type: HostEvent["type"], payload: Record<string, unknown>, generation?: string): void {
  if (!activeSession) return;
  const event: HostEvent = {
    event_id: eventId++,
    ...(generation ? { generation } : {}),
    payload,
    protocol_version: VISUALIZATION_PROTOCOL_VERSION,
    session: activeSession,
    type,
  };
  context.postMessage(event);
}

function artifact(bytes: Uint8Array, generation: CurrentGeneration): Record<string, unknown> {
  return {
    bytes: cloneBytes(bytes),
    generation: generation.generation,
    project: generation.projectName,
  };
}

function requireCurrent(): CurrentGeneration {
  if (!current) throw new Error("no repository generation is loaded");
  return current;
}

function mapIdentity(bytes: Uint8Array): { generation: string; project: string; files: number } {
  let document: Record<string, unknown>;
  try {
    document = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  } catch (error) {
    throw new Error(`native Map is not valid UTF-8 JSON: ${errorMessage(error)}`);
  }
  const evidence = document.evidence as Record<string, unknown> | undefined;
  const generation = evidence?.input_sha256;
  const project = document.project;
  const files = document.files;
  if (typeof generation !== "string" || !/^[0-9a-f]{64}$/.test(generation)) {
    throw new Error("native Map has no valid evidence.input_sha256");
  }
  if (typeof project !== "string" || !project || !Array.isArray(files)) {
    throw new Error("native Map identity is incomplete");
  }
  return { files: files.length, generation, project };
}

function openDatabase(): Promise<IDBDatabase | null> {
  if (typeof indexedDB === "undefined") return Promise.resolve(null);
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(SNAPSHOT_DATABASE, 1);
    request.onupgradeneeded = () => {
      if (!request.result.objectStoreNames.contains(SNAPSHOT_STORE)) {
        request.result.createObjectStore(SNAPSHOT_STORE, { keyPath: "generation" });
      }
    };
    request.onerror = () => reject(request.error || new Error("cannot open snapshot database"));
    request.onsuccess = () => resolve(request.result);
  });
}

function transactionResult<T>(request: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    request.onerror = () => reject(request.error || new Error("snapshot transaction failed"));
    request.onsuccess = () => resolve(request.result);
  });
}

async function snapshotRows(): Promise<SnapshotRecord[]> {
  const database = await openDatabase();
  if (!database) return [...memorySnapshots.values()];
  try {
    const rows = await transactionResult(
      database.transaction(SNAPSHOT_STORE, "readonly").objectStore(SNAPSHOT_STORE).getAll(),
    ) as SnapshotRecord[];
    return rows;
  } finally {
    database.close();
  }
}

function snapshotSort(left: SnapshotRecord, right: SnapshotRecord): number {
  if (left.stored_at !== right.stored_at) return right.stored_at - left.stored_at;
  return left.generation < right.generation ? -1 : left.generation > right.generation ? 1 : 0;
}

async function persistSnapshot(row: SnapshotRecord): Promise<void> {
  memorySnapshots.set(row.generation, row);
  const database = await openDatabase();
  if (database) {
    try {
      await transactionResult(
        database.transaction(SNAPSHOT_STORE, "readwrite").objectStore(SNAPSHOT_STORE).put(row),
      );
    } finally {
      database.close();
    }
  }
  const rows = (await snapshotRows()).sort(snapshotSort);
  const expired = rows.slice(SNAPSHOT_LIMIT);
  for (const item of expired) memorySnapshots.delete(item.generation);
  if (!expired.length) return;
  const cleanup = await openDatabase();
  if (!cleanup) return;
  try {
    const store = cleanup.transaction(SNAPSHOT_STORE, "readwrite").objectStore(SNAPSHOT_STORE);
    await Promise.all(expired.map((item) => transactionResult(store.delete(item.generation))));
  } finally {
    cleanup.close();
  }
}

async function findSnapshot(generation: string): Promise<SnapshotRecord> {
  const memory = memorySnapshots.get(generation);
  if (memory) return memory;
  const database = await openDatabase();
  if (!database) throw new Error(`snapshot is unavailable: ${generation}`);
  try {
    const row = await transactionResult(
      database.transaction(SNAPSHOT_STORE, "readonly").objectStore(SNAPSHOT_STORE).get(generation),
    ) as SnapshotRecord | undefined;
    if (!row) throw new Error(`snapshot is unavailable: ${generation}`);
    memorySnapshots.set(generation, row);
    return row;
  } finally {
    database.close();
  }
}

function rootConfig(files: RepositoryInputFile[]): Uint8Array | undefined {
  const matches = files.filter((file) => file.path === "archbird.json" || file.path === ".archbird.json");
  if (matches.length > 1) throw new Error("repository contains both archbird.json and .archbird.json");
  return matches[0]?.data;
}

async function loadRepository(request: HostRequest): Promise<Record<string, unknown>> {
  const runtime = await requireRuntime();
  emit("scan-started", { kind: request.payload.kind || "unknown" });
  let files: RepositoryInputFile[];
  if (request.payload.kind === "directory" && Array.isArray(request.payload.files)) {
    files = await readDirectoryFiles(request.payload.files as File[], (progress) => {
      emit("progress", progress as unknown as Record<string, unknown>);
    });
  } else if (request.payload.kind === "zip" && request.payload.file instanceof File) {
    files = await readZipFile(request.payload.file, (progress) => {
      emit("progress", progress as unknown as Record<string, unknown>);
    });
  } else {
    throw new Error("load expects a directory File list or ZIP File");
  }
  emit("progress", { completed: files.length, phase: "analyze", total: files.length });
  const sources = files.map((file) => new runtime.Source(file.path, file.data));
  let candidate: BrowserProject | null = null;
  try {
    candidate = runtime.Project.fromFiles(sources, {
      config: rootConfig(files),
      typescript: true,
    });
    const map = candidate.mapJson();
    const identity = mapIdentity(map);
    const next: CurrentGeneration = {
      files: new Map(files.map((file) => [file.path, file.data])),
      generation: identity.generation,
      map,
      project: candidate,
      projectName: identity.project,
    };
    const prior = current;
    current = next;
    candidate = null;
    prior?.project?.dispose();
    const snapshot: SnapshotRecord = {
      bytes: cloneBytes(map),
      files: identity.files,
      generation: identity.generation,
      project: identity.project,
      stored_at: Date.now(),
    };
    try {
      await persistSnapshot(snapshot);
    } catch (error) {
      emit("progress", { phase: "snapshot-warning", message: errorMessage(error) }, identity.generation);
    }
    emit("snapshot-ready", { files: identity.files, project: identity.project }, identity.generation);
    return artifact(map, next);
  } catch (error) {
    candidate?.dispose();
    emit("candidate-failed", { message: errorMessage(error) }, current?.generation);
    throw error;
  }
}

async function requireRuntime(wasmUrl?: string): Promise<BrowserRuntime> {
  if (!runtimePromise) {
    if (!wasmUrl) throw new Error("bootstrap must supply wasm_url");
    runtimePromise = fetch(wasmUrl).then(async (response) => {
      if (!response.ok) throw new Error(`cannot load Archbird Wasm (${response.status})`);
      return await browser.createBrowserArchbird({
        wasmBinary: await response.arrayBuffer(),
      }) as BrowserRuntime;
    });
  }
  return runtimePromise;
}

async function handle(request: HostRequest): Promise<unknown> {
  if (request.protocol_version !== VISUALIZATION_PROTOCOL_VERSION) {
    throw new Error(`unsupported visualization protocol ${request.protocol_version}`);
  }
  if (request.method === "bootstrap") {
    if (activeSession && request.session !== activeSession) throw new Error("Worker session is already bound");
    activeSession = request.session;
    const runtime = await requireRuntime(String(request.payload.wasm_url || ""));
    return {
      capabilities: ["directory", "zip", "map", "view", "query", "diff", "source", "snapshots"],
      engine: "wasm",
      version: runtime.VERSION,
    };
  }
  if (!activeSession || request.session !== activeSession) throw new Error("invalid Worker session");
  if (request.method === "load") return loadRepository(request);
  if (request.method === "state") {
    return current ? {
      generation: current.generation,
      project: current.projectName,
      source_available: current.files.size > 0,
    } : { generation: null, project: null, source_available: false };
  }
  if (request.method === "map") {
    const generation = requireCurrent();
    return artifact(generation.map, generation);
  }
  if (request.method === "view") {
    const generation = requireCurrent();
    const view = request.payload.view;
    if (view !== "components" && view !== "files" && view !== "symbols") {
      throw new Error(`unsupported graph view: ${String(view)}`);
    }
    const maxNodes = Number(request.payload.max_nodes ?? 0);
    const maxEdgeNames = Number(request.payload.max_edge_names ?? 3);
    let bytes: Uint8Array;
    if (generation.project) {
      bytes = generation.project.graphViewJson({
        maxEdgeNames,
        maxNodes,
        query: request.payload.query || {},
        view,
      });
    } else {
      const runtime = await requireRuntime();
      bytes = runtime.core.mapExportGraph(
        generation.map,
        "json",
        view,
        "LR",
        maxNodes,
        maxEdgeNames,
      );
    }
    return artifact(bytes, generation);
  }
  if (request.method === "query") {
    const generation = requireCurrent();
    if (!generation.project) throw new Error("saved snapshot has no live query provider");
    return artifact(
      generation.project.queryJson(
        (request.payload.query || {}) as Record<string, unknown>,
      ),
      generation,
    );
  }
  if (request.method === "diff") {
    const before = await findSnapshot(String(request.payload.before || ""));
    const after = await findSnapshot(String(request.payload.after || ""));
    const runtime = await requireRuntime();
    return {
      bytes: cloneBytes(runtime.core.mapDiff(new Uint8Array(before.bytes), new Uint8Array(after.bytes))),
      generation: after.generation,
      project: after.project,
    };
  }
  if (request.method === "source") {
    const generation = requireCurrent();
    const path = String(request.payload.path || "");
    const bytes = generation.files.get(path);
    if (!bytes) throw new Error(`source is unavailable in the current generation: ${path}`);
    const maximum = 256 * 1024;
    const visible = bytes.subarray(0, maximum);
    try {
      return {
        bytes: bytes.byteLength,
        encoding: "utf-8",
        path,
        sha256: (await requireRuntime()).core.sha256(bytes),
        text: new TextDecoder("utf-8", { fatal: true }).decode(visible),
        truncated: bytes.byteLength > visible.byteLength,
      };
    } catch {
      return {
        bytes: bytes.byteLength,
        encoding: "hex",
        path,
        sha256: (await requireRuntime()).core.sha256(bytes),
        text: [...visible.subarray(0, 64 * 1024)]
          .map((value) => value.toString(16).padStart(2, "0")).join(""),
        truncated: bytes.byteLength > Math.min(visible.byteLength, 64 * 1024),
      };
    }
  }
  if (request.method === "snapshots") {
    return (await snapshotRows()).sort(snapshotSort).map(({ bytes: _bytes, ...row }) => row);
  }
  if (request.method === "open-snapshot") {
    const snapshot = await findSnapshot(String(request.payload.generation || ""));
    current?.project?.dispose();
    current = {
      files: new Map(),
      generation: snapshot.generation,
      map: new Uint8Array(snapshot.bytes.slice(0)),
      project: null,
      projectName: snapshot.project,
    };
    return artifact(current.map, current);
  }
  if (["verification", "act-proposal", "act-contract", "act-result"].includes(request.method)) {
    return null;
  }
  if (request.method === "dispose") {
    current?.project?.dispose();
    current = null;
    return { disposed: true };
  }
  throw new Error(`unsupported visualization method: ${request.method}`);
}

context.addEventListener("message", async (event: MessageEvent<HostRequest>) => {
  const request = event.data;
  const base = {
    id: request?.id || "invalid",
    protocol_version: VISUALIZATION_PROTOCOL_VERSION,
    session: request?.session || activeSession || "invalid",
  };
  try {
    const result = await handle(request);
    const response: HostResponse = { ...base, ok: true, result };
    const transferable = result && typeof result === "object"
      && (result as Record<string, unknown>).bytes instanceof ArrayBuffer
      ? [(result as Record<string, unknown>).bytes as ArrayBuffer]
      : [];
    context.postMessage(response, transferable);
  } catch (error) {
    const response: HostResponse = {
      ...base,
      error: { code: "host-error", message: errorMessage(error) },
      ok: false,
    };
    context.postMessage(response);
  }
});
