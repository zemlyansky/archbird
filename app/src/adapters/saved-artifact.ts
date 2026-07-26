import wasm from "archbird/wasm";
import {
  graphViewFromArtifact,
  parseArtifact,
  parseGraphView,
  supportedViews,
  type GraphView,
  type GraphViewName,
  type ParsedArtifact,
} from "../artifacts/model";

export interface Projection {
  bytes: Uint8Array;
  graph: GraphView;
}

export type GraphDirection = "BT" | "LR" | "RL" | "TB";
export type GraphFormat = "graphml" | "json" | "mermaid";

let corePromise: ReturnType<typeof wasm.createArchbirdCore> | null = null;

async function core() {
  if (!corePromise) {
    corePromise = fetch(`${import.meta.env.BASE_URL}archbird.wasm`)
      .then(async (response) => {
        if (!response.ok) throw new Error(`cannot load Archbird Wasm (${response.status})`);
        return wasm.createArchbirdCore({ wasmBinary: await response.arrayBuffer() });
      });
  }
  return corePromise;
}

export async function loadArtifact(file: File): Promise<ParsedArtifact> {
  return parseArtifact(new Uint8Array(await file.arrayBuffer()), file.name);
}

export async function evaluateArtifactProjection(
  artifact: ParsedArtifact,
  plan: Record<string, unknown>,
): Promise<Uint8Array> {
  if (artifact.artifact !== "map") {
    throw new Error("projection evaluation requires a canonical Map");
  }
  const archbird = await core();
  return archbird.projectionEvaluate(
    artifact.bytes,
    new Uint8Array(),
    new TextEncoder().encode(JSON.stringify(plan)),
    false,
  );
}

export async function projectArtifact(
  artifact: ParsedArtifact,
  requestedView?: GraphViewName,
): Promise<Projection | null> {
  const projected = graphViewFromArtifact(artifact);
  if (projected) return { bytes: artifact.bytes, graph: projected };
  const views = supportedViews(artifact.artifact);
  if (views.length === 0) return null;
  const view = requestedView && views.includes(requestedView) ? requestedView : views[0];
  const output = await exportArtifact(artifact, "json", view);
  return {
    bytes: output,
    graph: parseGraphView(JSON.parse(new TextDecoder().decode(output))),
  };
}

export async function exportArtifact(
  artifact: ParsedArtifact,
  format: GraphFormat,
  view: GraphViewName,
  direction: GraphDirection = "LR",
): Promise<Uint8Array> {
  const archbird = await core();
  return archbird.mapExportGraph(artifact.bytes, format, view, direction, 0, 3);
}

export async function queryArtifact(
  artifact: ParsedArtifact,
  request: Record<string, unknown>,
): Promise<ParsedArtifact> {
  if (artifact.artifact !== "map") throw new Error("focused Query requires a canonical Map");
  const archbird = await core();
  const bytes = archbird.mapQuery(
    artifact.bytes,
    new Uint8Array(),
    new TextEncoder().encode(JSON.stringify(request)),
    false,
  );
  return parseArtifact(bytes, `${String(artifact.document.project || "project")}.query.json`);
}
