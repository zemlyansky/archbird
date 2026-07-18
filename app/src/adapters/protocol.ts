export const VISUALIZATION_PROTOCOL_VERSION = 1 as const;

export type HostMethod =
  | "bootstrap"
  | "load"
  | "state"
  | "map"
  | "view"
  | "query"
  | "diff"
  | "source"
  | "verification"
  | "act-proposal"
  | "act-contract"
  | "act-result"
  | "snapshots"
  | "open-snapshot"
  | "dispose";

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
  session: string;
  type:
    | "scan-started"
    | "progress"
    | "candidate-failed"
    | "snapshot-ready"
    | "verification-ready";
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
    && typeof row.session === "string"
    && typeof row.type === "string"
    && Boolean(row.payload)
    && typeof row.payload === "object";
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
