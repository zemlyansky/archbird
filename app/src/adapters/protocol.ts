export const VISUALIZATION_PROTOCOL_VERSION = 2 as const;

export type HostMethod =
  | "bootstrap"
  | "load"
  | "state"
  | "map"
  | "projection"
  | "projections"
  | "view"
  | "export"
  | "query"
  | "diff"
  | "source"
  | "verification"
  | "snapshots"
  | "open-snapshot"
  | "dispose";

export type ServerMethod = HostMethod | "path" | "verify";

export interface HostRequest {
  id: string;
  method: HostMethod;
  payload: Record<string, unknown>;
  protocol_version: typeof VISUALIZATION_PROTOCOL_VERSION;
  session: string;
}

export interface HostFailure {
  code: string;
  details?: unknown;
  message: string;
}

export interface HostResponse {
  error?: HostFailure;
  generation?: string;
  id: string;
  ok: boolean;
  protocol_version: typeof VISUALIZATION_PROTOCOL_VERSION;
  result?: unknown;
  session: string;
}

export interface HostEvent {
  event_id: number;
  generation?: string;
  payload: Record<string, unknown>;
  protocol_version: typeof VISUALIZATION_PROTOCOL_VERSION;
  session?: string;
  type:
    | "scan-started"
    | "progress"
    | "candidate-failed"
    | "snapshot-ready"
    | "verification-ready"
    | "display-updated";
}

export interface ArtifactDescriptor {
  blob_sha256: string;
  bytes: number;
  generation: string;
  project: string;
}

export interface DisplayState {
  artifact: ArtifactDescriptor;
  display: Record<string, unknown>;
  generation: string;
  method: "diff" | "map" | "path" | "query" | "verify";
  params: Record<string, unknown>;
  presentation?: ArtifactDescriptor;
  revision: number;
  session?: string;
}

export interface ObservedDisplay {
  artifact: string;
  expanded: {
    files: string[];
    groups: string[];
    omitted: number;
    symbols: string[];
  };
  generation: string;
  graph: {
    direction: string;
    edge_classification: string;
    edge_flow: string;
    edge_kind: string;
    edges: number;
    group_by: string;
    layout: string;
    nodes: number;
    notation: string;
    perspective: string;
    view: string;
  } | null;
  hidden: { ids: string[]; omitted: number };
  history_depth: number;
  selected: {
    id: string;
    identity: string;
    kind: string;
    label: string;
    path?: string;
  } | null;
  selected_constraint: string | null;
  source: { end?: number; path: string; start?: number } | null;
  workspace: "live" | "snapshot";
}

export interface ServerResponse {
  error?: HostFailure;
  generation?: string;
  ok: boolean;
  protocol_version: typeof VISUALIZATION_PROTOCOL_VERSION;
  result?: unknown;
  session?: string;
}

export interface ArtifactPayload {
  bytes: ArrayBuffer;
  generation: string;
  project: string;
}

export interface SnapshotSummary {
  files: number;
  generation: string;
  project: string;
  schema_version: number;
  stored_at: number;
}

export function isHostResponse(value: unknown): value is HostResponse {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const row = value as Record<string, unknown>;
  return row.protocol_version === VISUALIZATION_PROTOCOL_VERSION
    && typeof row.id === "string"
    && typeof row.session === "string"
    && typeof row.ok === "boolean";
}

export function isHostEvent(value: unknown): value is HostEvent {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const row = value as Record<string, unknown>;
  return row.protocol_version === VISUALIZATION_PROTOCOL_VERSION
    && Number.isSafeInteger(row.event_id)
    && (row.session === undefined || typeof row.session === "string")
    && typeof row.type === "string"
    && Boolean(row.payload)
    && typeof row.payload === "object";
}

export function isServerResponse(value: unknown): value is ServerResponse {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const row = value as Record<string, unknown>;
  return row.protocol_version === VISUALIZATION_PROTOCOL_VERSION
    && typeof row.ok === "boolean"
    && (row.session === undefined || typeof row.session === "string");
}

export function displayState(value: unknown): DisplayState {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error("display state must be an object");
  }
  const row = value as Record<string, unknown>;
  const methods = new Set(["diff", "map", "path", "query", "verify"]);
  if (!methods.has(String(row.method))) throw new Error("display method is invalid");
  if (!Number.isSafeInteger(row.revision) || Number(row.revision) < 1) {
    throw new Error("display revision is invalid");
  }
  if (!row.params || typeof row.params !== "object" || Array.isArray(row.params)
      || !row.display || typeof row.display !== "object" || Array.isArray(row.display)) {
    throw new Error("display params and controls must be objects");
  }
  if (typeof row.generation !== "string" || !/^[0-9a-f]{64}$/.test(row.generation)) {
    throw new Error("display generation is invalid");
  }
  if (row.session !== undefined && (typeof row.session !== "string" || !row.session)) {
    throw new Error("display session is invalid");
  }
  const artifact = artifactDescriptor(row.artifact);
  if (artifact.generation !== row.generation) {
    throw new Error("display artifact generation differs from display state");
  }
  if (row.presentation !== undefined) {
    const presentation = artifactDescriptor(row.presentation);
    if (presentation.generation !== artifact.generation
        || presentation.project !== artifact.project) {
      throw new Error("display presentation differs from its artifact");
    }
  }
  return row as unknown as DisplayState;
}

export function artifactDescriptor(value: unknown): ArtifactDescriptor {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error("artifact descriptor must be an object");
  }
  const row = value as Record<string, unknown>;
  if (typeof row.blob_sha256 !== "string" || !/^[0-9a-f]{64}$/.test(row.blob_sha256)
      || !Number.isSafeInteger(row.bytes) || Number(row.bytes) < 0
      || typeof row.generation !== "string" || !/^[0-9a-f]{64}$/.test(row.generation)
      || typeof row.project !== "string" || !row.project) {
    throw new Error("artifact descriptor is invalid");
  }
  return row as unknown as ArtifactDescriptor;
}

export function artifactPayload(value: unknown): ArtifactPayload {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error("host artifact result must be an object");
  }
  const row = value as Record<string, unknown>;
  if (!(row.bytes instanceof ArrayBuffer)) throw new Error("host artifact bytes are missing");
  if (typeof row.generation !== "string" || !/^[0-9a-f]{64}$/.test(row.generation)) {
    throw new Error("host artifact generation is invalid");
  }
  if (typeof row.project !== "string" || !row.project) {
    throw new Error("host artifact project is invalid");
  }
  return row as unknown as ArtifactPayload;
}
