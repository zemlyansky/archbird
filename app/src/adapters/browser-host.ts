import BrowserWorker from "../worker/browser.worker?worker";
import {
  VISUALIZATION_PROTOCOL_VERSION,
  artifactPayload,
  isHostEvent,
  isHostResponse,
  type ArtifactPayload,
  type HostEvent,
  type HostMethod,
  type HostRequest,
  type SnapshotSummary,
} from "./protocol";
import type { GraphViewName } from "../artifacts/model";

type Listener = (event: HostEvent) => void;

interface Pending {
  reject: (error: Error) => void;
  resolve: (result: unknown) => void;
}

export type BrowserSource =
  | { kind: "directory"; files: File[] }
  | { kind: "zip"; file: File };

function sessionId(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(16));
  return `browser-${[...bytes].map((value) => value.toString(16).padStart(2, "0")).join("")}`;
}

export class BrowserHost {
  private worker: Worker;
  private readonly session = sessionId();
  private nextId = 0;
  private pending = new Map<string, Pending>();
  private listeners = new Set<Listener>();
  private ready: Promise<unknown>;

  constructor() {
    this.worker = this.spawn();
    this.ready = this.requestRaw("bootstrap", {
      wasm_url: new URL(`${import.meta.env.BASE_URL}archbird.wasm`, location.href).href,
    });
  }

  private spawn(): Worker {
    const worker = new BrowserWorker();
    worker.addEventListener("message", (event: MessageEvent<unknown>) => this.message(event.data));
    worker.addEventListener("error", (event) => {
      this.rejectAll(new Error(event.message || "Archbird browser Worker failed"));
    });
    return worker;
  }

  private message(value: unknown): void {
    if (isHostEvent(value)) {
      if (value.session === this.session) {
        for (const listener of this.listeners) listener(value);
      }
      return;
    }
    if (!isHostResponse(value) || value.session !== this.session) return;
    const pending = this.pending.get(value.id);
    if (!pending) return;
    this.pending.delete(value.id);
    if (value.ok) pending.resolve(value.result);
    else pending.reject(new Error(value.error?.message || "Archbird host request failed"));
  }

  private rejectAll(error: Error): void {
    for (const pending of this.pending.values()) pending.reject(error);
    this.pending.clear();
  }

  private requestRaw(method: HostMethod, payload: Record<string, unknown>): Promise<unknown> {
    const id = `r${++this.nextId}`;
    const request: HostRequest = {
      id,
      method,
      payload,
      protocol_version: VISUALIZATION_PROTOCOL_VERSION,
      session: this.session,
    };
    return new Promise((resolve, reject) => {
      this.pending.set(id, { reject, resolve });
      this.worker.postMessage(request);
    });
  }

  private async request(method: HostMethod, payload: Record<string, unknown> = {}): Promise<unknown> {
    if (method !== "bootstrap") await this.ready;
    return this.requestRaw(method, payload);
  }

  subscribe(listener: Listener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  async load(source: BrowserSource): Promise<ArtifactPayload> {
    const payload = source.kind === "directory"
      ? { kind: source.kind, files: source.files }
      : { kind: source.kind, file: source.file };
    return artifactPayload(await this.request("load", payload));
  }

  async map(generation?: string): Promise<ArtifactPayload> {
    return artifactPayload(await this.request("map", generation ? { generation } : {}));
  }

  async projection(
    plan: Record<string, unknown>,
    generation?: string,
  ): Promise<ArtifactPayload> {
    return artifactPayload(await this.request("projection", {
      ...(generation ? { generation } : {}),
      plan,
    }));
  }

  async view(
    view: GraphViewName,
    query: Record<string, unknown> = {},
    generation?: string,
  ): Promise<ArtifactPayload> {
    return artifactPayload(await this.request("view", {
      ...(generation ? { generation } : {}),
      max_edge_names: 3,
      max_nodes: 0,
      query,
      view,
    }));
  }

  async query(query: Record<string, unknown>, generation?: string): Promise<ArtifactPayload> {
    return artifactPayload(await this.request("query", {
      ...(generation ? { generation } : {}),
      query,
    }));
  }

  async verification(generation?: string): Promise<ArtifactPayload | null> {
    const value = await this.request("verification", generation ? { generation } : {});
    return value === null ? null : artifactPayload(value);
  }

  async diff(before: string, after: string): Promise<ArtifactPayload> {
    return artifactPayload(await this.request("diff", { after, before }));
  }

  async source(path: string, generation?: string): Promise<Record<string, unknown>> {
    return await this.request("source", {
      ...(generation ? { generation } : {}),
      path,
    }) as Record<string, unknown>;
  }

  async snapshots(): Promise<SnapshotSummary[]> {
    const value = await this.request("snapshots");
    if (!Array.isArray(value)) throw new Error("host snapshots result must be an array");
    return value as SnapshotSummary[];
  }

  async openSnapshot(generation: string): Promise<ArtifactPayload> {
    return artifactPayload(await this.request("open-snapshot", { generation }));
  }

  cancel(): void {
    this.worker.terminate();
    this.rejectAll(new Error("analysis canceled"));
    this.worker = this.spawn();
    this.ready = this.requestRaw("bootstrap", {
      wasm_url: new URL(`${import.meta.env.BASE_URL}archbird.wasm`, location.href).href,
    });
  }

  dispose(): void {
    this.worker.postMessage({
      id: `r${++this.nextId}`,
      method: "dispose",
      payload: {},
      protocol_version: VISUALIZATION_PROTOCOL_VERSION,
      session: this.session,
    } satisfies HostRequest);
    this.worker.terminate();
    this.rejectAll(new Error("browser host disposed"));
    this.listeners.clear();
  }
}
