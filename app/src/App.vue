<script setup lang="ts">
import {
  computed,
  onBeforeUnmount,
  onMounted,
  ref,
  shallowRef,
  watch,
} from "vue";
import { BrowserHost, type BrowserSource } from "./adapters/browser-host";
import type { HostEvent, SnapshotSummary } from "./adapters/protocol";
import {
  evaluateArtifactProjection,
  projectArtifact,
  queryArtifact,
  type GraphDirection,
  type GraphFormat,
} from "./adapters/saved-artifact";
import { ServerHost } from "./adapters/server-host";
import {
  isParsedArtifact,
  parseArtifact,
  parseGraphView,
  supportedViews,
  type GraphView,
  type GraphViewName,
  type LiveMapReference,
  type ParsedArtifact,
  type WorkspaceArtifact,
} from "./artifacts/model";
import {
  parseProjectionResult,
  type ProjectionResult,
} from "./artifacts/projection";
import ArtifactDrop from "./components/ArtifactDrop.vue";
import ConstraintPanel from "./components/ConstraintPanel.vue";
import DocumentPanel from "./components/DocumentPanel.vue";
import GraphCanvas from "./components/GraphCanvas.vue";
import InspectorPanel from "./components/InspectorPanel.vue";
import VerificationEditor from "./components/VerificationEditor.vue";
import {
  mapGraphProjectionPlan,
  presentGraphProjection,
  type GraphGrouping,
  type GraphPresentation,
  type MapView,
} from "./graph/presentation";
import { exportPresentedGraph } from "./graph/export";
import {
  composeArchitectureGraph,
  graphActivation,
  orientGraphEdges,
  type GraphEdgeFlow,
} from "./graph/explore";
import {
  applyVerificationOverlay,
  verificationConstraints,
} from "./graph/verification";

type Theme = "dark" | "light" | "system";
type TransportMode = "browser" | "none" | "server";
type WorkspaceMode = "live" | "saved" | "snapshot";
type GraphCanvasApi = {
  fit(): void;
  setZoom(value: number): void;
  zoomIn(): void;
  zoomOut(): void;
};

const serverDocument = document.querySelector<HTMLMetaElement>(
  'meta[name="archbird-host"]',
)?.content === "server";
const artifact = shallowRef<WorkspaceArtifact | null>(null);
const baseMap = shallowRef<WorkspaceArtifact | null>(null);
const graph = shallowRef<GraphView | null>(null);
const graphBytes = shallowRef<Uint8Array | null>(null);
const graphPresentations = shallowRef<
  Readonly<Record<string, GraphPresentation>> | null
>(null);
const fileGraph = shallowRef<GraphView | null>(null);
const symbolGraphs = shallowRef<ReadonlyMap<string, GraphView>>(new Map());
const verification = shallowRef<Record<string, unknown> | null>(null);
const source = shallowRef<Record<string, unknown> | null>(null);
const graphCanvas = ref<GraphCanvasApi | null>(null);
const selectedId = ref<string | null>(null);
const search = ref("");
const error = ref("");
const errorOrigin = ref<"candidate" | "operation">("operation");
const busy = ref(serverDocument);
const currentView = ref<GraphViewName>("components");
const mapView = ref<MapView>("overview");
const graphGrouping = ref<GraphGrouping>("directory");
const direction = ref<GraphDirection>("LR");
const edgeFlow = ref<GraphEdgeFlow>("flow");
const edgeKind = ref("all");
const edgeClassification = ref("all");
const progress = ref<Record<string, unknown> | null>(
  serverDocument ? { phase: "connecting to local analysis" } : null,
);
const graphLayoutBusy = ref(false);
const snapshots = ref<SnapshotSummary[]>([]);
const sourceAvailable = ref(false);
const transportMode = ref<TransportMode>("none");
const workspaceMode = ref<WorkspaceMode>(serverDocument ? "live" : "saved");
const inspectorOpen = ref(true);
const theme = ref<Theme>("system");
const systemDark = ref(window.matchMedia("(prefers-color-scheme: dark)").matches);
const queryKind = ref<"component" | "path" | "search" | "symbol">("symbol");
const queryValue = ref("");
const queryDirection = ref<"both" | "downstream" | "upstream">("both");
const queryDepth = ref(1);
const queryTestDepth = ref(2);
const exportFormat = ref<Exclude<GraphFormat, "json">>("graphml");
const diffBefore = ref("");
const diffAfter = ref("");
const expandedGroups = ref<ReadonlySet<string>>(new Set());
const expandedFiles = ref<ReadonlySet<string>>(new Set());
const revealedSymbols = ref<ReadonlySet<string>>(new Set());
const hiddenElements = ref<ReadonlySet<string>>(new Set());
const selectedConstraintId = ref<string | null>(null);
const zoomPercent = ref(100);
const graphSpacing = ref(60);
const baseGeneration = ref<string | null>(null);

let liveHost: BrowserHost | ServerHost | null = null;
let unsubscribe: (() => void) | null = null;
let operationId = 0;
let operationStartedAt = 0;
let themeMedia: MediaQueryList | null = null;

const availableViews = computed(() => {
  if (!artifact.value || artifact.value.artifact === "map") return [];
  return supportedViews(artifact.value.artifact);
});
const artifactProject = computed(() => {
  if (!artifact.value) return "";
  return isParsedArtifact(artifact.value)
    ? String(artifact.value.document.project || artifact.value.name)
    : artifact.value.project;
});
const artifactSchemaVersion = computed(() => {
  if (!artifact.value) return "";
  return isParsedArtifact(artifact.value)
    ? String(artifact.value.document.schema_version || "")
    : String(artifact.value.schemaVersion);
});
const availableGraphGroupings = computed<GraphGrouping[]>(() =>
  (["directory", "component", "layer", "language"] as const).filter((grouping) =>
    grouping !== "component" || Boolean(fileGraph.value?.nodes.some((node) =>
      node.kind === "file"
      && Array.isArray(node.attributes.components)
      && node.attributes.components.length))));
const mapViews: readonly MapView[] = [
  "overview",
  "architecture",
  "tests",
  "evidence",
];
function graphPresentationKey(
  view: MapView = mapView.value,
  grouping: GraphGrouping = graphGrouping.value,
): string {
  return `${view}:${grouping}`;
}
const overviewGraph = computed(() =>
  graphPresentations.value?.[graphPresentationKey()]?.overview || null);
const resolvedTheme = computed<"dark" | "light">(() =>
  theme.value === "system" ? (systemDark.value ? "dark" : "light") : theme.value);
const canCancel = computed(() => transportMode.value === "browser" && busy.value);
const operationVisible = computed(() =>
  Boolean(artifact.value && (busy.value || graphLayoutBusy.value)));
const operationPhase = computed(() =>
  busy.value ? String(progress.value?.phase || "working") : "Arranging graph");
const canReturnLive = computed(() =>
  transportMode.value !== "none" && workspaceMode.value !== "live");
const workspaceLabel = computed(() => workspaceMode.value === "saved"
  ? "saved"
  : `${transportMode.value}-${workspaceMode.value}`);
const inputStatus = computed(() => {
  if (workspaceMode.value === "saved") {
    return artifact.value
      ? "Saved artifact · opened in this browser"
      : "Artifact, project folder, or source ZIP · nothing is uploaded";
  }
  if (transportMode.value === "server") {
    if (!artifact.value) {
      return "Live project · analysis in progress on the local Archbird server";
    }
    return workspaceMode.value === "live"
      ? "Live project · analyzed by the local Archbird server"
      : "Repository snapshot · served by the local Archbird server";
  }
  if (transportMode.value === "browser") {
    return artifact.value
      ? "Local project snapshot · analyzed in this browser"
      : "Local project snapshot · analysis in progress in this browser";
  }
  return "Artifact, project folder, or source ZIP · nothing is uploaded";
});
const canExportGraph = computed(() =>
  Boolean(graph.value && artifact.value?.artifact === "map"));
const emptyMappedScope = computed(() =>
  artifact.value?.artifact === "map"
  && Boolean(fileGraph.value)
  && fileGraph.value?.nodes.length === 0);
const explorationGraph = computed<GraphView | null>(() => {
  if (
    artifact.value?.artifact !== "map" || !baseMap.value
    || !overviewGraph.value
  ) return graph.value;
  return composeArchitectureGraph(
    overviewGraph.value,
    fileGraph.value,
    symbolGraphs.value,
    {
      expandedGroups: expandedGroups.value,
      expandedFiles: expandedFiles.value,
      hidden: hiddenElements.value,
      revealedSymbols: revealedSymbols.value,
    },
  );
});
const overlay = computed(() => explorationGraph.value
  ? applyVerificationOverlay(
    explorationGraph.value,
    verification.value,
    selectedConstraintId.value,
  )
  : null);
const presentedGraph = computed(() => overlay.value?.graph || null);
const constraintRows = computed(() => verificationConstraints(verification.value));
const selectedConstraint = computed(() =>
  constraintRows.value.find((row) => row.id === selectedConstraintId.value) || null);
const edgeKinds = computed(() =>
  [...new Set((presentedGraph.value?.edges || []).map((edge) => edge.kind))].sort());
const edgeClassifications = computed(() =>
  [...new Set((presentedGraph.value?.edges || []).map((edge) => edge.classification))].sort());
const visibleGraph = computed<GraphView | null>(() => {
  if (!presentedGraph.value) return null;
  const edges = presentedGraph.value.edges.filter((edge) =>
    (edgeKind.value === "all" || edge.kind === edgeKind.value) &&
    (edgeClassification.value === "all" || edge.classification === edgeClassification.value));
  return orientGraphEdges({
    ...presentedGraph.value,
    edges,
    summary: { edges: edges.length, nodes: presentedGraph.value.nodes.length },
  }, edgeFlow.value);
});
const searchResults = computed(() => {
  const needle = search.value.trim().toLocaleLowerCase();
  if (!visibleGraph.value || !needle) return [];
  return visibleGraph.value.nodes.filter((node) =>
    `${node.label}\n${node.identity}\n${node.kind}`.toLocaleLowerCase().includes(needle),
  ).slice(0, 30);
});
const sourcePath = computed(() => {
  if (!visibleGraph.value || !selectedId.value || !sourceAvailable.value) return null;
  const selected = visibleGraph.value.nodes.find((node) => node.id === selectedId.value);
  if (!selected) return null;
  let file = selected;
  if (selected.kind === "symbol" && selected.parent) {
    file = visibleGraph.value.nodes.find((node) => node.id === selected.parent) || selected;
  }
  return file.kind === "file" ? file.identity : null;
});

function beginOperation(): number {
  const id = ++operationId;
  operationStartedAt = performance.now();
  busy.value = true;
  errorOrigin.value = "operation";
  return id;
}

function ownsOperation(id: number): boolean {
  return id === operationId;
}

function finishOperation(id: number) {
  if (!ownsOperation(id)) return;
  const remaining = Math.max(0, 300 - (performance.now() - operationStartedAt));
  window.setTimeout(() => {
    if (!ownsOperation(id)) return;
    busy.value = false;
    progress.value = null;
  }, remaining);
}

function reportOperationError(cause: unknown) {
  errorOrigin.value = "operation";
  error.value = cause instanceof Error ? cause.message : String(cause);
}

function resetGraphState() {
  selectedId.value = null;
  source.value = null;
  search.value = "";
  edgeKind.value = "all";
  edgeClassification.value = "all";
  inspectorOpen.value = true;
  expandedGroups.value = new Set();
  expandedFiles.value = new Set();
  revealedSymbols.value = new Set();
  hiddenElements.value = new Set();
  selectedConstraintId.value = null;
  zoomPercent.value = 100;
}

function resetMapProjections() {
  graphPresentations.value = null;
  fileGraph.value = null;
  symbolGraphs.value = new Map();
  verification.value = null;
}

function selectGraphElement(id: string | null) {
  selectedId.value = id;
  if (id) {
    selectedConstraintId.value = null;
    inspectorOpen.value = true;
  }
}

function selectConstraint(id: string | null) {
  selectedConstraintId.value = id;
  if (id) {
    selectedId.value = null;
    inspectorOpen.value = true;
  }
}

function replaceSet(
  current: ReadonlySet<string>,
  value: string,
  present: boolean,
): ReadonlySet<string> {
  const next = new Set(current);
  if (present) next.add(value);
  else next.delete(value);
  return next;
}

function graphFromBytes(bytes: Uint8Array): GraphView {
  return parseGraphView(JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes)));
}

function snapshotRows(rows: SnapshotSummary[], project: string): SnapshotSummary[] {
  return rows.filter((row) => row.project === project);
}

function initializeSnapshotSelection() {
  if (!snapshots.value.length) {
    diffBefore.value = "";
    diffAfter.value = "";
    return;
  }
  diffAfter.value = snapshots.value[0].generation;
  diffBefore.value = snapshots.value[Math.min(1, snapshots.value.length - 1)].generation;
}

async function setHost(host: BrowserHost | ServerHost | null, mode: TransportMode) {
  unsubscribe?.();
  unsubscribe = null;
  if (liveHost && liveHost !== host) await liveHost.dispose();
  liveHost = host;
  transportMode.value = mode;
  if (host) unsubscribe = host.subscribe(hostEvent);
}

async function ensureBrowserHost(): Promise<BrowserHost> {
  if (!(liveHost instanceof BrowserHost)) {
    await setHost(new BrowserHost(), "browser");
  }
  return liveHost as BrowserHost;
}

function hostEvent(event: HostEvent) {
  if (event.type === "progress" || event.type === "scan-started") {
    if (workspaceMode.value === "live") {
      busy.value = true;
      progress.value = event.payload;
    }
    return;
  }
  if (event.type === "candidate-failed") {
    if (workspaceMode.value === "live") {
      busy.value = false;
      progress.value = event.payload;
      errorOrigin.value = "candidate";
      error.value = String(event.payload.message || "candidate analysis failed");
    }
    return;
  }
  if (event.type === "snapshot-ready" && liveHost instanceof ServerHost &&
      workspaceMode.value === "live") {
    void refreshServer(liveHost);
  }
}

async function liveGraphView(
  host: BrowserHost | ServerHost,
  view: GraphViewName,
  query: Record<string, unknown> = {},
  generation?: string,
) {
  const payload = await host.view(view, query, generation);
  const bytes = new Uint8Array(payload.bytes);
  return { bytes, generation: payload.generation, graph: graphFromBytes(bytes) };
}

async function hostFactProjection(
  host: BrowserHost | ServerHost,
  plan: Record<string, unknown>,
  generation?: string,
): Promise<ProjectionResult> {
  const payload = await host.projection(plan, generation);
  if (generation && payload.generation !== generation) {
    throw new Error(
      `projection generation changed: expected ${generation}, got ${payload.generation}`,
    );
  }
  return parseProjectionResult(
    new Uint8Array(payload.bytes),
    String(plan.select),
  );
}

async function mapPresentation(
  loaded: WorkspaceArtifact,
  host: BrowserHost | ServerHost | null,
  view: MapView = "overview",
  grouping: GraphGrouping = "directory",
  generation?: string,
): Promise<GraphPresentation> {
  const plan = mapGraphProjectionPlan(view, grouping);
  const projection = host
    ? await hostFactProjection(host, plan, generation)
    : await evaluateArtifactProjection(
      requireParsedArtifact(loaded, "local projection"),
      plan,
    )
      .then((bytes) =>
        parseProjectionResult(bytes, plan.select));
  return presentGraphProjection(projection, view);
}

async function liveVerification(
  host: BrowserHost | ServerHost,
  generation?: string,
) {
  const payload = await host.verification(generation);
  if (!payload) return null;
  const loaded = parseArtifact(
    new Uint8Array(payload.bytes),
    `${payload.project}.verification.json`,
  );
  if (loaded.artifact !== "verification") {
    throw new Error("live host returned a non-Verification artifact");
  }
  return loaded.document;
}

function installMapPresentation(
  loaded: WorkspaceArtifact,
  presentation: GraphPresentation,
) {
  artifact.value = loaded;
  baseMap.value = loaded;
  currentView.value = "components";
  mapView.value = presentation.view;
  graphGrouping.value = presentation.grouping;
  graphPresentations.value = {
    [graphPresentationKey(presentation.view, presentation.grouping)]:
      presentation,
  };
  fileGraph.value = presentation.files;
  graph.value = presentation.overview;
  graphBytes.value = new TextEncoder().encode(JSON.stringify(graph.value));
}

function liveMapReference(state: Record<string, unknown>): LiveMapReference {
  const generation = state.generation;
  const project = state.project;
  const schemaVersion = state.schema_version;
  if (state.source_available !== true) {
    throw new Error("local host has no published live Map");
  }
  if (typeof generation !== "string" || !/^[0-9a-f]{64}$/.test(generation)) {
    throw new Error("local host returned an invalid live Map generation");
  }
  if (typeof project !== "string" || !project) {
    throw new Error("local host returned an invalid live Map project");
  }
  if (!Number.isSafeInteger(schemaVersion) || Number(schemaVersion) <= 0) {
    throw new Error("local host returned an invalid live Map schema version");
  }
  return {
    artifact: "map",
    generation,
    name: `${project}.archbird.json`,
    project,
    schemaVersion: Number(schemaVersion),
  };
}

function requireParsedArtifact(
  value: WorkspaceArtifact,
  operation: string,
): ParsedArtifact {
  if (!isParsedArtifact(value)) {
    throw new Error(`${operation} requires local artifact bytes`);
  }
  return value;
}

function installGraphView(projection: { bytes: Uint8Array; graph: GraphView } | null) {
  graph.value = projection?.graph || null;
  graphBytes.value = projection?.bytes || null;
}

function requireFileGraph(): GraphView {
  if (fileGraph.value) return fileGraph.value;
  throw new Error("file projection is unavailable for the selected Map");
}

async function ensureSymbolGraph(path: string): Promise<GraphView> {
  const cached = symbolGraphs.value.get(path);
  if (cached) return cached;
  if (!baseMap.value) throw new Error("symbol projection requires a Map");
  const options = {
    artifacts: [],
    components: [],
    depth: 0,
    direction: "both",
    focus: [],
    packages: [],
    paths: [path],
    search: [],
    searchLimit: 8,
    symbols: [],
    testDepth: 0,
  };
  const host = workspaceMode.value !== "saved" && liveHost &&
    !(workspaceMode.value === "snapshot" && liveHost instanceof BrowserHost)
    ? liveHost
    : null;
  const projection = host
    ? await liveGraphView(host, "symbols", options, baseGeneration.value || undefined)
    : await queryArtifact(
      requireParsedArtifact(baseMap.value, "local Query"),
      nativeQueryRequest(options),
    )
      .then((query) => projectArtifact(query, "symbols"));
  if (!projection) throw new Error(`symbol projection is unavailable for ${path}`);
  const next = new Map(symbolGraphs.value);
  next.set(path, projection.graph);
  symbolGraphs.value = next;
  return projection.graph;
}

async function activateGraphElement(id: string) {
  const node = visibleGraph.value?.nodes.find((row) => row.id === id);
  const activation = node ? graphActivation(node) : null;
  if (!node || !activation) return;
  const expanding = !Boolean(node.attributes.expanded);
  if (activation === "symbol") {
    revealedSymbols.value = replaceSet(
      revealedSymbols.value,
      node.id,
      !Boolean(node.attributes.relations_revealed),
    );
    return;
  }
  if (activation === "group") {
    if (expanding) {
      const operation = beginOperation();
      progress.value = { phase: `loading files for ${node.label}` };
      try {
        requireFileGraph();
        if (ownsOperation(operation)) {
          expandedGroups.value = replaceSet(expandedGroups.value, node.id, true);
          progress.value = null;
        }
      } catch (cause) {
        if (ownsOperation(operation)) reportOperationError(cause);
      } finally {
        finishOperation(operation);
      }
    } else {
      expandedGroups.value = replaceSet(expandedGroups.value, node.id, false);
    }
    return;
  }
  const path = typeof node.attributes.canonical_path === "string"
    ? node.attributes.canonical_path
    : node.identity;
  if (expanding) {
    const operation = beginOperation();
    progress.value = { phase: `loading symbols for ${path}` };
    try {
      await ensureSymbolGraph(path);
      if (ownsOperation(operation)) {
        expandedFiles.value = replaceSet(expandedFiles.value, node.id, true);
        progress.value = null;
      }
    } catch (cause) {
      if (ownsOperation(operation)) reportOperationError(cause);
    } finally {
      finishOperation(operation);
    }
  } else {
    expandedFiles.value = replaceSet(expandedFiles.value, node.id, false);
  }
}

function hideGraphElement(id: string) {
  hiddenElements.value = replaceSet(hiddenElements.value, id, true);
  if (selectedId.value === id) selectedId.value = null;
}

function restoreHiddenElements() {
  hiddenElements.value = new Set();
}

function collapseExplorer() {
  expandedGroups.value = new Set();
  expandedFiles.value = new Set();
  revealedSymbols.value = new Set();
  selectedId.value = null;
}

async function changeMapPresentation(
  view: MapView,
  grouping: GraphGrouping,
) {
  if (
    (view === mapView.value && grouping === graphGrouping.value)
    || !baseMap.value
  ) return;
  const key = graphPresentationKey(view, grouping);
  const cached = graphPresentations.value?.[key];
  if (cached) {
    resetGraphState();
    mapView.value = view;
    graphGrouping.value = grouping;
    fileGraph.value = cached.files;
    graph.value = cached.overview;
    graphBytes.value = new TextEncoder().encode(JSON.stringify(cached.overview));
    return;
  }
  const id = beginOperation();
  progress.value = {
    phase: `Projecting existing Map: ${view} grouped by ${grouping}`,
  };
  try {
    const host = workspaceMode.value !== "saved" && liveHost &&
      !(workspaceMode.value === "snapshot" && liveHost instanceof BrowserHost)
      ? liveHost
      : null;
    const presentation = await mapPresentation(
      baseMap.value,
      host,
      view,
      grouping,
      baseGeneration.value || undefined,
    );
    if (!ownsOperation(id)) return;
    resetGraphState();
    graphPresentations.value = {
      ...(graphPresentations.value || {}),
      [key]: presentation,
    };
    mapView.value = view;
    graphGrouping.value = grouping;
    fileGraph.value = presentation.files;
    graph.value = presentation.overview;
    graphBytes.value = new TextEncoder().encode(JSON.stringify(graph.value));
    progress.value = null;
  } catch (cause) {
    if (ownsOperation(id)) reportOperationError(cause);
  } finally {
    finishOperation(id);
  }
}

async function changeMapView(view: MapView) {
  await changeMapPresentation(view, graphGrouping.value);
}

async function changeGraphGrouping(grouping: GraphGrouping) {
  await changeMapPresentation(mapView.value, grouping);
}

async function open(file: File) {
  const id = beginOperation();
  error.value = "";
  progress.value = { phase: "opening saved artifact" };
  resetGraphState();
  resetMapProjections();
  try {
    const loaded = await parseArtifact(
      new Uint8Array(await file.arrayBuffer()),
      file.name,
    );
    if (!ownsOperation(id)) return;
    artifact.value = loaded;
    baseMap.value = loaded.artifact === "map" ? loaded : null;
    baseGeneration.value = null;
    workspaceMode.value = "saved";
    sourceAvailable.value = false;
    snapshots.value = [];
    initializeSnapshotSelection();
    graph.value = null;
    graphBytes.value = null;
    if (loaded.artifact === "map") {
      const presentation = await mapPresentation(loaded, null);
      if (!ownsOperation(id)) return;
      installMapPresentation(loaded, presentation);
      progress.value = null;
      return;
    }
    const views = supportedViews(loaded.artifact);
    if (views.length) currentView.value = views[0];
    const projection = await projectArtifact(loaded, currentView.value);
    if (!ownsOperation(id)) return;
    installGraphView(projection);
    progress.value = null;
  } catch (cause) {
    if (!ownsOperation(id)) return;
    artifact.value = null;
    baseMap.value = null;
    graph.value = null;
    graphBytes.value = null;
    error.value = (cause as Error).message;
  } finally {
    finishOperation(id);
  }
}

async function openLive(input: BrowserSource) {
  const id = beginOperation();
  error.value = "";
  resetGraphState();
  resetMapProjections();
  progress.value = { completed: 0, phase: "starting", total: 1 };
  try {
    const host = await ensureBrowserHost();
    const payload = await host.load(input);
    if (!ownsOperation(id)) return;
    const loaded = parseArtifact(
      new Uint8Array(payload.bytes),
      `${payload.project}.archbird.json`,
    );
    const [presentation, verified] = await Promise.all([
      mapPresentation(loaded, host, "overview", "directory", payload.generation),
      liveVerification(host, payload.generation),
    ]);
    if (!ownsOperation(id)) return;
    installMapPresentation(loaded, presentation);
    baseGeneration.value = payload.generation;
    workspaceMode.value = "live";
    sourceAvailable.value = true;
    verification.value = verified;
    snapshots.value = snapshotRows(await host.snapshots(), payload.project);
    initializeSnapshotSelection();
    progress.value = null;
  } catch (cause) {
    if (ownsOperation(id)) error.value = (cause as Error).message;
  } finally {
    finishOperation(id);
  }
}

async function openSnapshot(generation: string) {
  if (!liveHost) return;
  const id = beginOperation();
  error.value = "";
  resetGraphState();
  resetMapProjections();
  try {
    const payload = await liveHost.openSnapshot(generation);
    const loaded = parseArtifact(
      new Uint8Array(payload.bytes),
      `${payload.project}-${generation.slice(0, 12)}.archbird.json`,
    );
    const [presentation, verified] = liveHost instanceof ServerHost
      ? await Promise.all([
        mapPresentation(
          loaded,
          liveHost,
          "overview",
          "directory",
          payload.generation,
        ),
        liveVerification(liveHost, payload.generation),
      ])
      : [await mapPresentation(loaded, null), null];
    if (!ownsOperation(id)) return;
    installMapPresentation(loaded, presentation);
    baseGeneration.value = payload.generation;
    workspaceMode.value = "snapshot";
    sourceAvailable.value = liveHost instanceof ServerHost;
    verification.value = verified;
  } catch (cause) {
    if (ownsOperation(id)) error.value = (cause as Error).message;
  } finally {
    finishOperation(id);
  }
}

async function returnToLive() {
  if (!liveHost) return;
  const id = beginOperation();
  error.value = "";
  resetGraphState();
  resetMapProjections();
  try {
    let host = liveHost;
    if (host instanceof ServerHost) {
      const replacement = await ServerHost.connect();
      if (!replacement) throw new Error("cannot reconnect to the live repository");
      await setHost(replacement, "server");
      host = replacement;
    }
    let loaded: WorkspaceArtifact;
    let generation: string;
    if (host instanceof ServerHost) {
      loaded = liveMapReference(await host.state());
      generation = loaded.generation;
    } else {
      const payload = await host.map();
      loaded = parseArtifact(
        new Uint8Array(payload.bytes),
        `${payload.project}.archbird.json`,
      );
      generation = payload.generation;
    }
    const [presentation, verified] = await Promise.all([
      mapPresentation(loaded, host, "overview", "directory", generation),
      liveVerification(host, generation),
    ]);
    if (!ownsOperation(id)) return;
    installMapPresentation(loaded, presentation);
    baseGeneration.value = generation;
    workspaceMode.value = "live";
    sourceAvailable.value = true;
    verification.value = verified;
    snapshots.value = snapshotRows(await host.snapshots(), artifactProject.value);
    initializeSnapshotSelection();
    progress.value = null;
  } catch (cause) {
    if (ownsOperation(id)) error.value = (cause as Error).message;
  } finally {
    finishOperation(id);
  }
}

async function showBaseMap() {
  if (!baseMap.value) return;
  const id = beginOperation();
  error.value = "";
  resetGraphState();
  resetMapProjections();
  try {
    const host = workspaceMode.value !== "saved" && liveHost &&
      !(workspaceMode.value === "snapshot" && liveHost instanceof BrowserHost)
      ? liveHost
      : null;
    const presentation = await mapPresentation(
      baseMap.value,
      host,
      "overview",
      "directory",
      baseGeneration.value || undefined,
    );
    if (!ownsOperation(id)) return;
    installMapPresentation(baseMap.value, presentation);
    sourceAvailable.value = workspaceMode.value === "live" ||
      (workspaceMode.value === "snapshot" && liveHost instanceof ServerHost);
  } catch (cause) {
    if (ownsOperation(id)) error.value = (cause as Error).message;
  } finally {
    finishOperation(id);
  }
}

async function changeView(view: GraphViewName) {
  if (!artifact.value || currentView.value === view) return;
  const id = beginOperation();
  error.value = "";
  resetGraphState();
  try {
    const projection = workspaceMode.value !== "saved" && liveHost &&
      artifact.value.artifact === "map" &&
      !(workspaceMode.value === "snapshot" && liveHost instanceof BrowserHost)
      ? await liveGraphView(liveHost, view)
      : await projectArtifact(
        requireParsedArtifact(artifact.value, "local graph projection"),
        view,
      );
    if (!ownsOperation(id)) return;
    currentView.value = view;
    graph.value = projection?.graph || null;
    graphBytes.value = projection?.bytes || null;
  } catch (cause) {
    if (ownsOperation(id)) error.value = (cause as Error).message;
  } finally {
    finishOperation(id);
  }
}

function queryOptions(): Record<string, unknown> {
  const value = queryValue.value.trim();
  if (!value) throw new Error("enter a Query selector");
  const options: Record<string, unknown> = {
    artifacts: [],
    components: [],
    depth: queryDepth.value,
    direction: queryDirection.value,
    focus: [],
    packages: [],
    paths: [],
    search: [],
    searchLimit: 8,
    symbols: [],
    testDepth: queryTestDepth.value,
  };
  const field = {
    component: "components",
    path: "paths",
    search: "search",
    symbol: "symbols",
  }[queryKind.value];
  options[field] = [value];
  return options;
}

function nativeQueryRequest(options: Record<string, unknown>): Record<string, unknown> {
  return {
    artifacts: options.artifacts,
    components: options.components,
    depth: options.depth,
    direction: options.direction,
    focus: options.focus,
    packages: options.packages,
    paths: options.paths,
    producer_policy: "compatible",
    search: options.search,
    search_limit: options.searchLimit,
    symbols: options.symbols,
    test_depth: options.testDepth,
  };
}

async function runQuery() {
  if (!baseMap.value) return;
  const id = beginOperation();
  error.value = "";
  resetGraphState();
  try {
    const options = queryOptions();
    const host = workspaceMode.value !== "saved" && liveHost &&
      !(workspaceMode.value === "snapshot" && liveHost instanceof BrowserHost)
      ? liveHost
      : null;
    let loaded: ParsedArtifact;
    let projection;
    if (host) {
      const payload = await host.query(options, baseGeneration.value || undefined);
      loaded = parseArtifact(
        new Uint8Array(payload.bytes),
        `${payload.project}.query.json`,
      );
      projection = await liveGraphView(
        host,
        "symbols",
        options,
        payload.generation,
      );
      if (projection.generation !== payload.generation) {
        throw new Error("live Query and symbol view generations differ");
      }
    } else if (isParsedArtifact(baseMap.value)) {
      loaded = await queryArtifact(baseMap.value, nativeQueryRequest(options));
      projection = await projectArtifact(loaded, "symbols");
    } else {
      throw new Error("saved Map bytes are unavailable for local Query");
    }
    if (!ownsOperation(id)) return;
    artifact.value = loaded;
    currentView.value = "symbols";
    graph.value = projection?.graph || null;
    graphBytes.value = projection?.bytes || null;
  } catch (cause) {
    if (ownsOperation(id)) error.value = (cause as Error).message;
  } finally {
    finishOperation(id);
  }
}

async function runDiff() {
  if (!liveHost || !diffBefore.value || !diffAfter.value) return;
  const id = beginOperation();
  error.value = "";
  resetGraphState();
  try {
    const payload = await liveHost.diff(diffBefore.value, diffAfter.value);
    if (!ownsOperation(id)) return;
    artifact.value = parseArtifact(
      new Uint8Array(payload.bytes),
      `${payload.project}.diff.json`,
    );
    graph.value = null;
    graphBytes.value = null;
    sourceAvailable.value = false;
  } catch (cause) {
    if (ownsOperation(id)) error.value = (cause as Error).message;
  } finally {
    finishOperation(id);
  }
}

async function openSource(path: string) {
  if (!liveHost || !sourceAvailable.value) return;
  error.value = "";
  try {
    source.value = await liveHost.source(path, baseGeneration.value || undefined);
  } catch (cause) {
    error.value = (cause as Error).message;
  }
}

function cancelLive() {
  if (!(liveHost instanceof BrowserHost)) return;
  ++operationId;
  liveHost.cancel();
  busy.value = false;
  sourceAvailable.value = false;
  progress.value = { phase: "canceled" };
}

async function refreshServer(host: ServerHost) {
  const id = beginOperation();
  try {
    const loaded = liveMapReference(await host.state());
    const [presentation, verified] = await Promise.all([
      mapPresentation(loaded, host, "overview", "directory", loaded.generation),
      liveVerification(host, loaded.generation),
    ]);
    if (!ownsOperation(id) || workspaceMode.value !== "live") return;
    resetGraphState();
    resetMapProjections();
    installMapPresentation(loaded, presentation);
    baseGeneration.value = loaded.generation;
    sourceAvailable.value = true;
    transportMode.value = "server";
    verification.value = verified;
    snapshots.value = snapshotRows(await host.snapshots(), loaded.project);
    initializeSnapshotSelection();
    error.value = "";
    progress.value = null;
  } catch (cause) {
    if (ownsOperation(id)) error.value = (cause as Error).message;
  } finally {
    finishOperation(id);
  }
}

function download(bytes: Uint8Array, name: string, type = "application/json") {
  const copy = Uint8Array.from(bytes);
  const url = URL.createObjectURL(new Blob([copy.buffer], { type }));
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = name;
  anchor.hidden = true;
  document.body.append(anchor);
  anchor.click();
  window.setTimeout(() => {
    anchor.remove();
    URL.revokeObjectURL(url);
  }, 60_000);
}

function safeProject(): string {
  return String(artifactProject.value || "archbird")
    .replace(/[^A-Za-z0-9._-]+/g, "-");
}

async function saveArtifact() {
  if (!artifact.value) return;
  if (isParsedArtifact(artifact.value)) {
    download(
      artifact.value.bytes,
      `${safeProject()}.${artifact.value.artifact}.json`,
    );
    return;
  }
  if (!(liveHost instanceof ServerHost)) {
    throw new Error("live Map bytes are unavailable");
  }
  const payload = await liveHost.map(artifact.value.generation);
  download(
    new Uint8Array(payload.bytes),
    `${safeProject()}.${artifact.value.artifact}.json`,
  );
}

function saveProjection() {
  if (!visibleGraph.value) return;
  const bytes = artifact.value?.artifact === "archbird-graph-view" && graphBytes.value
    ? graphBytes.value
    : new TextEncoder().encode(JSON.stringify(visibleGraph.value));
  download(bytes, `${safeProject()}-${visibleGraph.value.request.view}.archbird-view.json`);
}

async function saveGraphExport() {
  if (!visibleGraph.value || !canExportGraph.value) return;
  try {
    const bytes = exportPresentedGraph(
      visibleGraph.value,
      exportFormat.value,
      direction.value,
    );
    const extension = exportFormat.value === "graphml" ? "graphml" : "mmd";
    const type = exportFormat.value === "graphml"
      ? "application/graphml+xml"
      : "text/plain";
    download(bytes, `${safeProject()}-${currentView.value}.${extension}`, type);
  } catch (cause) {
    error.value = (cause as Error).message;
  }
}

function applyTheme(value: Theme) {
  theme.value = value;
  if (value === "system") delete document.documentElement.dataset.theme;
  else document.documentElement.dataset.theme = value;
  localStorage.setItem("archbird-theme", value);
}

watch(graph, () => {
  edgeKind.value = "all";
  edgeClassification.value = "all";
});

onBeforeUnmount(() => {
  unsubscribe?.();
  themeMedia?.removeEventListener("change", systemThemeChanged);
  void liveHost?.dispose();
});

function systemThemeChanged(event: MediaQueryListEvent) {
  systemDark.value = event.matches;
}

onMounted(async () => {
  themeMedia = window.matchMedia("(prefers-color-scheme: dark)");
  systemDark.value = themeMedia.matches;
  themeMedia.addEventListener("change", systemThemeChanged);
  const storedTheme = localStorage.getItem("archbird-theme");
  applyTheme(storedTheme === "light" || storedTheme === "dark" ? storedTheme : "system");
  if (window.matchMedia("(max-width: 700px)").matches) direction.value = "TB";
  const server = await ServerHost.connect();
  if (!server) {
    if (serverDocument) {
      busy.value = false;
      error.value = "cannot connect to the local Archbird analysis host";
    }
    return;
  }
  if (liveHost) {
    await server.dispose();
    return;
  }
  await setHost(server, "server");
  workspaceMode.value = "live";
  const state = await server.state();
  if (state.source_available) {
    await refreshServer(server);
  } else {
    busy.value = state.phase !== "failed";
    progress.value = { phase: String(state.phase || "waiting") };
    if (state.phase === "failed") {
      error.value = String(state.last_error || "candidate analysis failed");
    }
  }
});
</script>

<template>
  <main>
    <header class="topbar">
      <div class="brand">
        <span class="bird">A</span>
        <div>
          <strong>Archbird</strong>
          <small>architecture evidence</small>
        </div>
      </div>
      <ArtifactDrop
        :status="inputStatus"
        @select="open"
        @directory="openLive({ kind: 'directory', files: $event })"
        @zip="openLive({ kind: 'zip', file: $event })"
      />
      <label class="search">
        <span>Filter current graph</span>
        <input v-model="search" :disabled="!graph" placeholder="symbol, file, component…" />
      </label>
      <label class="theme-control">
        <span>Theme</span>
        <select :value="theme" @change="applyTheme(($event.target as HTMLSelectElement).value as Theme)">
          <option value="system">System</option>
          <option value="light">Light</option>
          <option value="dark">Dark</option>
        </select>
      </label>
    </header>

    <div
      v-if="operationVisible"
      class="operation-progress"
      role="status"
      aria-live="polite"
    >
      <span>{{ operationPhase }}</span>
      <progress
        v-if="busy && typeof progress?.completed === 'number' && typeof progress?.total === 'number'"
        :value="Number(progress.completed)"
        :max="Math.max(1, Number(progress.total))"
      ></progress>
      <progress v-else></progress>
    </div>

    <section v-if="!artifact && !error && busy" class="analysis-status" aria-live="polite">
      <span class="status busy"></span>
      <div>
        <p class="eyebrow">Live repository</p>
        <h1>Mapping project</h1>
        <p>{{ progress?.phase || 'analyzing' }}</p>
        <progress
          v-if="typeof progress?.completed === 'number' && typeof progress?.total === 'number'"
          :value="Number(progress.completed)"
          :max="Math.max(1, Number(progress.total))"
        ></progress>
      </div>
    </section>

    <section v-else-if="!artifact && !error" class="welcome">
      <p class="eyebrow">Architecture evidence workspace</p>
      <h1>Open a project or saved artifact</h1>
      <p>
        Map a local folder or ZIP in this browser, inspect a live repository,
        or review canonical Archbird JSON.
      </p>
    </section>

    <section v-else class="workspace" :class="{ 'inspector-hidden': !inspectorOpen }">
      <nav class="rail" aria-label="Artifact controls">
        <div class="artifact-meta">
          <span class="status" :class="{ busy }"></span>
          <div>
            <strong>{{ artifactProject || artifact?.name || 'Artifact' }}</strong>
            <small>{{ artifact?.artifact }} · schema {{ artifactSchemaVersion }} · {{ workspaceLabel }}</small>
          </div>
        </div>

        <div class="primary-actions">
          <button v-if="canReturnLive" type="button" @click="returnToLive">Return to live Map</button>
          <button v-if="baseMap && artifact !== baseMap" type="button" @click="showBaseMap">Back to Map</button>
          <button v-if="artifact" type="button" @click="saveArtifact">Save canonical artifact</button>
        </div>

        <div v-if="availableViews.length > 1" class="view-switcher">
          <button
            v-for="view in availableViews"
            :key="view"
            type="button"
            :class="{ active: currentView === view }"
            @click="changeView(view)"
          >{{ view }}</button>
        </div>
        <div
          v-if="artifact?.artifact === 'map'"
          class="view-switcher"
          aria-label="Map view"
        >
          <button
            v-for="view in mapViews"
            :key="view"
            type="button"
            :class="{ active: mapView === view }"
            :disabled="busy || graphLayoutBusy"
            @click="changeMapView(view)"
          >{{ view }}</button>
        </div>
        <div
          v-if="artifact?.artifact === 'map' && availableGraphGroupings.length > 1"
          class="view-switcher"
          aria-label="Group graph by"
        >
          <button
            v-for="grouping in availableGraphGroupings"
            :key="grouping"
            type="button"
            :class="{ active: graphGrouping === grouping }"
            :disabled="busy || graphLayoutBusy"
            @click="changeGraphGrouping(grouping)"
          >{{ grouping }}</button>
        </div>

        <template v-if="graph">
          <div v-if="artifact?.artifact === 'map'" class="explorer-actions">
            <button
              type="button"
              :disabled="!expandedGroups.size && !expandedFiles.size"
              @click="collapseExplorer"
            >Collapse all</button>
            <button
              type="button"
              :disabled="!hiddenElements.size"
              @click="restoreHiddenElements"
            >Restore hidden ({{ hiddenElements.size }})</button>
          </div>
          <div class="graph-toolbar" aria-label="Graph viewport controls">
            <button type="button" aria-label="Zoom out" title="Zoom out" @click="graphCanvas?.zoomOut()">−</button>
            <button type="button" aria-label="Fit graph" title="Fit graph" @click="graphCanvas?.fit()">Fit</button>
            <button type="button" aria-label="Zoom in" title="Zoom in" @click="graphCanvas?.zoomIn()">+</button>
            <button type="button" @click="inspectorOpen = !inspectorOpen">
              {{ inspectorOpen ? 'Hide details' : 'Show details' }}
            </button>
          </div>
          <label class="zoom-control">
            <span>Zoom</span>
            <input
              v-model.number="zoomPercent"
              type="range"
              min="5"
              max="300"
              step="5"
              @input="graphCanvas?.setZoom(zoomPercent)"
            />
            <output>{{ zoomPercent }}%</output>
          </label>
          <label class="zoom-control">
            <span>Spacing</span>
            <input
              v-model.number="graphSpacing"
              type="range"
              min="5"
              max="140"
              step="5"
            />
            <output>{{ graphSpacing }}%</output>
          </label>
          <div class="graph-options">
            <label>Direction
              <select v-model="direction">
                <option value="LR">Left to right</option>
                <option value="TB">Top to bottom</option>
                <option value="RL">Right to left</option>
                <option value="BT">Bottom to top</option>
              </select>
            </label>
            <label>Arrows
              <select v-model="edgeFlow">
                <option value="flow">Flow · provider → consumer</option>
                <option value="uses">Uses · consumer → provider</option>
              </select>
            </label>
            <label>Relations
              <select v-model="edgeKind">
                <option value="all">All types</option>
                <option v-for="kind in edgeKinds" :key="kind" :value="kind">{{ kind }}</option>
              </select>
            </label>
            <label>Evidence
              <select v-model="edgeClassification">
                <option value="all">All classes</option>
                <option v-for="classification in edgeClassifications" :key="classification" :value="classification">
                  {{ classification }}
                </option>
              </select>
            </label>
          </div>
          <div class="graph-legend" aria-label="Graph entity legend">
            <span><i data-kind="component"></i>Component</span>
            <span><i data-kind="layer"></i>Inferred layer</span>
            <span><i data-kind="language"></i>Language</span>
            <span><i data-kind="directory"></i>Directory</span>
            <span><i data-kind="package"></i>Package</span>
            <span><i data-kind="build"></i>Build</span>
            <span><i data-kind="file"></i>File</span>
            <span><i data-kind="symbol"></i>Symbol</span>
            <span><i data-kind="issue"></i>Finding</span>
          </div>
          <div class="export-actions">
            <button type="button" @click="saveProjection">Save view JSON</button>
            <template v-if="canExportGraph">
              <select v-model="exportFormat" aria-label="Structural export format">
                <option value="graphml">GraphML</option>
                <option value="mermaid">Mermaid</option>
              </select>
              <button type="button" @click="saveGraphExport">Export</button>
            </template>
          </div>
        </template>

        <ConstraintPanel
          :constraints="constraintRows"
          :selected-id="selectedConstraintId"
          :unmapped-findings="overlay?.unmappedFindings.length || 0"
          @select="selectConstraint"
        />

        <details v-if="baseMap" class="query-panel">
          <summary>Focused Query</summary>
          <label>Selector
            <select v-model="queryKind">
              <option value="symbol">Symbol</option>
              <option value="path">Path</option>
              <option value="component">Component</option>
              <option value="search">Lexical search</option>
            </select>
          </label>
          <label>Value
            <input v-model="queryValue" @keydown.enter="runQuery" />
          </label>
          <div class="query-numbers">
            <label>Depth <input v-model.number="queryDepth" type="number" min="0" max="12" /></label>
            <label>Test depth <input v-model.number="queryTestDepth" type="number" min="0" max="12" /></label>
          </div>
          <label>Direction
            <select v-model="queryDirection">
              <option value="both">Both</option>
              <option value="upstream">Upstream</option>
              <option value="downstream">Downstream</option>
            </select>
          </label>
          <button type="button" @click="runQuery">Run Query</button>
        </details>

        <div v-if="busy && progress" class="progress-card">
          <p>{{ progress.phase || 'working' }}</p>
          <progress
            v-if="typeof progress.completed === 'number' && typeof progress.total === 'number'"
            :value="Number(progress.completed)"
            :max="Math.max(1, Number(progress.total))"
          ></progress>
          <button v-if="canCancel" class="cancel-button" type="button" @click="cancelLive">
            Cancel Worker
          </button>
        </div>

        <div v-if="workspaceMode !== 'saved' && snapshots.length" class="snapshots">
          <h3>Repository snapshots</h3>
          <button
            v-for="snapshot in snapshots"
            :key="snapshot.generation"
            class="snapshot-button"
            type="button"
            :title="snapshot.generation"
            @click="openSnapshot(snapshot.generation)"
          >{{ snapshot.project }} · {{ snapshot.generation.slice(0, 10) }}</button>
          <div v-if="snapshots.length > 1" class="diff-controls">
            <label>Before
              <select v-model="diffBefore">
                <option v-for="snapshot in snapshots" :key="`before:${snapshot.generation}`" :value="snapshot.generation">
                  {{ snapshot.generation.slice(0, 10) }}
                </option>
              </select>
            </label>
            <label>After
              <select v-model="diffAfter">
                <option v-for="snapshot in snapshots" :key="`after:${snapshot.generation}`" :value="snapshot.generation">
                  {{ snapshot.generation.slice(0, 10) }}
                </option>
              </select>
            </label>
            <button type="button" :disabled="diffBefore === diffAfter" @click="runDiff">Compare</button>
          </div>
        </div>

        <div v-if="searchResults.length" class="results" aria-label="Graph filter results">
          <button
            v-for="node in searchResults"
            :key="node.id"
            type="button"
            @click="selectGraphElement(node.id)"
          >
            <span>{{ node.label }}</span>
            <small>{{ node.kind }}</small>
          </button>
        </div>
      </nav>

      <div v-if="error && !graph" class="error-card">
        <strong>Could not complete operation</strong>
        <p>{{ error }}</p>
      </div>
      <template v-if="visibleGraph">
        <div v-if="error" class="error-banner">
          <strong>{{
            errorOrigin === 'candidate'
              ? 'Candidate failed; showing last good view.'
              : 'Could not complete operation; current view is unchanged.'
          }}</strong>
          <span>{{ error }}</span>
        </div>
        <section
          v-if="emptyMappedScope"
          class="graph-empty"
          aria-live="polite"
        >
          <p class="eyebrow">Mapped scope</p>
          <h2>No source files were mapped</h2>
          <p>
            Discovery completed successfully, but the selected repository scope
            contains no supported source evidence.
          </p>
        </section>
        <GraphCanvas
          v-else
          ref="graphCanvas"
          :direction="direction"
          :graph="visibleGraph"
          :selected-id="selectedId"
          :spacing="graphSpacing"
          :theme="resolvedTheme"
          @activate="activateGraphElement"
          @layout-end="graphLayoutBusy = false"
          @layout-start="graphLayoutBusy = true"
          @select="selectGraphElement"
          @zoom="zoomPercent = $event"
        />
        <InspectorPanel
          v-if="!emptyMappedScope"
          v-show="inspectorOpen"
          :constraint="selectedConstraint"
          :file-graph="fileGraph"
          :graph="visibleGraph"
          :selected-id="selectedId"
          :source="source"
          :source-path="sourcePath"
          @activate="activateGraphElement"
          @close="inspectorOpen = false"
          @hide="hideGraphElement"
          @source="openSource"
        />
      </template>
      <VerificationEditor
        v-else-if="artifact && isParsedArtifact(artifact) && !error && ['verification', 'project-configuration'].includes(artifact.artifact)"
        :artifact="artifact"
      />
      <DocumentPanel
        v-else-if="artifact && isParsedArtifact(artifact) && !error"
        :artifact="artifact"
      />
    </section>
  </main>
</template>
