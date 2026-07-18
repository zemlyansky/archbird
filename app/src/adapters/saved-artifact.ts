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

export async function projectArtifact(
  artifact: ParsedArtifact,
  requestedView?: GraphViewName,
): Promise<Projection | null> {
  const projected = graphViewFromArtifact(artifact);
  if (projected) return { bytes: artifact.bytes, graph: projected };
  const views = supportedViews(artifact.artifact);
  if (views.length === 0) return null;
  const view = requestedView && views.includes(requestedView) ? requestedView : views[0];
  const archbird = await core();
  const output = archbird.mapExportGraph(
    artifact.bytes,
    "json",
    view,
    "LR",
    0,
    3,
  );
  return {
    bytes: output,
    graph: parseGraphView(JSON.parse(new TextDecoder().decode(output))),
  };
}
