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

function sessionId(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(16));
  return `server-${[...bytes].map((value) => value.toString(16).padStart(2, "0")).join("")}`;
}

function hex(bytes: ArrayBuffer): string {
  return [...new Uint8Array(bytes)].map((value) => value.toString(16).padStart(2, "0")).join("");
}

export class ServerHost {
  private readonly session = sessionId();
  private nextId = 0;
  private listeners = new Set<Listener>();
  private events: EventSource | null = null;

  static async connect(): Promise<ServerHost | null> {
    try {
      const response = await fetch(`${import.meta.env.BASE_URL}api/v1/bootstrap`, {
        cache: "no-store",
        headers: { Accept: "application/json" },
      });
      if (!response.ok || !(response.headers.get("content-type") || "").includes("application/json")) {
        return null;
      }
      const document = await response.json() as Record<string, unknown>;
      if (document.protocol_version !== VISUALIZATION_PROTOCOL_VERSION) return null;
      const host = new ServerHost();
      host.openEvents();
      return host;
    } catch {
      return null;
    }
  }

  private openEvents(): void {
    this.events = new EventSource(`${import.meta.env.BASE_URL}api/v1/events`);
    for (const type of [
      "scan-started", "progress", "candidate-failed", "snapshot-ready", "verification-ready",
    ]) {
      this.events.addEventListener(type, (event) => {
        try {
          const value = JSON.parse((event as MessageEvent<string>).data);
          if (isHostEvent(value)) {
            for (const listener of this.listeners) listener(value);
          }
        } catch {
          // A malformed transport event is ignored; request/response evidence is unaffected.
        }
      });
    }
  }

  subscribe(listener: Listener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private async request(method: HostMethod, payload: Record<string, unknown> = {}): Promise<unknown> {
    const request: HostRequest = {
      id: `r${++this.nextId}`,
      method,
      payload,
      protocol_version: VISUALIZATION_PROTOCOL_VERSION,
      session: this.session,
    };
    const response = await fetch(`${import.meta.env.BASE_URL}api/v1/request`, {
      body: JSON.stringify(request),
      cache: "no-store",
      headers: { Accept: "application/json", "Content-Type": "application/json" },
      method: "POST",
    });
    const value = await response.json();
    if (!isHostResponse(value) || value.id !== request.id || value.session !== this.session) {
      throw new Error("local host returned an invalid response envelope");
    }
    if (!value.ok) throw new Error(value.error?.message || `local host request failed (${response.status})`);
    return value.result;
  }

  private async artifact(method: HostMethod, payload: Record<string, unknown> = {}): Promise<ArtifactPayload> {
    const value = await this.request(method, payload);
    if (!value || typeof value !== "object" || Array.isArray(value)) {
      throw new Error("local host artifact descriptor is invalid");
    }
    const descriptor = value as Record<string, unknown>;
    const digest = descriptor.blob_sha256;
    if (typeof digest !== "string" || !/^[0-9a-f]{64}$/.test(digest)) {
      throw new Error("local host artifact digest is invalid");
    }
    const response = await fetch(`${import.meta.env.BASE_URL}api/v1/blobs/${digest}`, {
      cache: "force-cache",
    });
    if (!response.ok) throw new Error(`local host artifact is unavailable (${response.status})`);
    const bytes = await response.arrayBuffer();
    if (Number(descriptor.bytes) !== bytes.byteLength) throw new Error("local host artifact size changed");
    const actual = hex(await crypto.subtle.digest("SHA-256", bytes));
    if (actual !== digest || response.headers.get("x-content-sha256") !== digest) {
      throw new Error("local host artifact digest changed in transport");
    }
    return artifactPayload({ ...descriptor, bytes });
  }

  map(): Promise<ArtifactPayload> {
    return this.artifact("map");
  }

  view(view: GraphViewName): Promise<ArtifactPayload> {
    return this.artifact("view", { max_edge_names: 3, max_nodes: 0, view });
  }

  query(query: Record<string, unknown>): Promise<ArtifactPayload> {
    return this.artifact("query", { query });
  }

  async state(): Promise<Record<string, unknown>> {
    return await this.request("state") as Record<string, unknown>;
  }

  async source(path: string): Promise<Record<string, unknown>> {
    return await this.request("source", { path }) as Record<string, unknown>;
  }

  async snapshots(): Promise<SnapshotSummary[]> {
    const value = await this.request("snapshots");
    if (!Array.isArray(value)) throw new Error("local host snapshots result must be an array");
    return value as SnapshotSummary[];
  }

  openSnapshot(generation: string): Promise<ArtifactPayload> {
    return this.artifact("open-snapshot", { generation });
  }

  async dispose(): Promise<void> {
    this.events?.close();
    this.events = null;
    try {
      await this.request("dispose");
    } catch {
      // The local server may already be shutting down.
    }
    this.listeners.clear();
  }
}
