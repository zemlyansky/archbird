<script setup lang="ts">
import {
  computed,
  nextTick,
  onBeforeUnmount,
  onMounted,
  ref,
  shallowRef,
  watch,
} from "vue";
import { BrowserHost, type BrowserSource } from "./adapters/browser-host";
import {
  displayState,
  type DisplayState,
  type HostEvent,
  type ObservedDisplay,
  type SnapshotSummary,
} from "./adapters/protocol";
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
  pathGraphFromArtifact,
  pathStatusFromArtifact,
} from "./artifacts/path";
import {
  parseProjectionResult,
  type ProjectionResult,
} from "./artifacts/projection";
import {
  queryRetrievalHits,
  type QueryRetrievalHit,
} from "./artifacts/query";
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
  directoryPresentationId,
  filePresentationId,
  graphActivation,
  orientGraphEdges,
  withHiddenPresentation,
  type GraphEdgeFlow,
} from "./graph/explore";
import {
  applyVerificationOverlay,
  verificationConstraints,
} from "./graph/verification";
import {
  applyMetricOverlay,
  metricProjectionPlan,
  type MetricId,
  type MetricScopeMode,
} from "./graph/metrics";
import type { GraphLayoutMode } from "./graph/layout";
import type { GraphNotation } from "./graph/notation";
import type { SourceSelection } from "./graph/source";

type Theme = "dark" | "light" | "system";
type TransportMode = "browser" | "none" | "server";
type WorkspaceMode = "live" | "saved" | "snapshot";
type GraphCanvasApi = {
  captureViewport(): GraphViewport | null;
  center(id: string): void;
  fit(): void;
  restoreViewport(viewport: GraphViewport): void;
  setZoom(value: number): void;
  zoomIn(): void;
  zoomOut(): void;
};
type GraphViewport = { pan: { x: number; y: number }; zoom: number };
const GRAPH_HISTORY_LIMIT = 16;
type GraphNavigationEntry = {
  artifact: WorkspaceArtifact;
  currentView: GraphViewName;
  direction: GraphDirection;
  edgeClassification: string;
  edgeFlow: GraphEdgeFlow;
  edgeKind: string;
  expandedFiles: ReadonlySet<string>;
  expandedGroups: ReadonlySet<string>;
  fileGraph: GraphView | null;
  graph: GraphView;
  graphBytes: Uint8Array | null;
  graphGrouping: GraphGrouping;
  graphLayout: GraphLayoutMode;
  graphNotation: GraphNotation;
  graphSpacing: number;
  hiddenElements: ReadonlySet<string>;
  inspectorOpen: boolean;
  mapView: MapView;
  metricOverlay: MetricId | null;
  metricScope: MetricScopeMode;
  queryDepth: number;
  queryDirection: "both" | "downstream" | "upstream";
  queryKind: "component" | "path" | "search" | "symbol";
  queryTestDepth: number;
  queryValue: string;
  revealedSymbols: ReadonlySet<string>;
  selectedConstraintId: string | null;
  selectedId: string | null;
  source: Record<string, unknown> | null;
  sourceSelection: SourceSelection | null;
  viewport: GraphViewport | null;
  zoomPercent: number;
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
const sourceSelection = shallowRef<SourceSelection | null>(null);
const graphCanvas = ref<GraphCanvasApi | null>(null);
const selectedId = ref<string | null>(null);
const entitySearchRoot = ref<HTMLElement | null>(null);
const entitySearch = ref("");
const entitySearchHits = shallowRef<readonly QueryRetrievalHit[]>([]);
const entitySearchError = ref("");
const entitySearchLoading = ref(false);
const entitySearchOpen = ref(false);
const entitySearchActiveIndex = ref(0);
const error = ref("");
const errorOrigin = ref<"candidate" | "operation">("operation");
const busy = ref(serverDocument);
const currentView = ref<GraphViewName>("components");
const mapView = ref<MapView>("overview");
const graphGrouping = ref<GraphGrouping>("directory");
const direction = ref<GraphDirection>("LR");
const graphLayout = ref<GraphLayoutMode>("flow");
const graphNotation = ref<GraphNotation>("architecture");
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
const metricScope = ref<MetricScopeMode>("selection");
const metricOverlay = ref<MetricId | null>(null);
const metricCache = shallowRef<ReadonlyMap<MetricId, ProjectionResult>>(new Map());
const metricLoading = shallowRef<ReadonlySet<MetricId>>(new Set());
const metricErrors = shallowRef<ReadonlyMap<MetricId, string>>(new Map());
const graphHistory = shallowRef<readonly GraphNavigationEntry[]>([]);

let liveHost: BrowserHost | ServerHost | null = null;
let unsubscribe: (() => void) | null = null;
let operationId = 0;
let operationStartedAt = 0;
let metricGeneration = 0;
let entitySearchGeneration = 0;
let entitySearchTimer: number | null = null;
let observedDisplayTimer: number | null = null;
let pendingGraphViewport: GraphViewport | null = null;
let appliedDisplayRevision = 0;
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
const pathStatus = computed(() => {
  if (!artifact.value || !isParsedArtifact(artifact.value)
      || artifact.value.artifact !== "path") return null;
  return pathStatusFromArtifact(artifact.value.document);
});
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
      hidden: new Set(),
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
const measuredGraph = computed(() =>
  metricOverlay.value && presentedGraph.value && metricCache.value.get(metricOverlay.value)
    ? applyMetricOverlay(
      presentedGraph.value,
      metricCache.value.get(metricOverlay.value)!,
    )
    : presentedGraph.value);
const shadowedGraph = computed(() => measuredGraph.value
  ? withHiddenPresentation(measuredGraph.value, hiddenElements.value)
  : null);
const constraintRows = computed(() => verificationConstraints(verification.value));
const selectedConstraint = computed(() =>
  constraintRows.value.find((row) => row.id === selectedConstraintId.value) || null);
const edgeKinds = computed(() =>
  [...new Set((measuredGraph.value?.edges || []).map((edge) => edge.kind))].sort());
const edgeClassifications = computed(() =>
  [...new Set((measuredGraph.value?.edges || []).map((edge) => edge.classification))].sort());
const visibleGraph = computed<GraphView | null>(() => {
  if (!shadowedGraph.value) return null;
  const edges = shadowedGraph.value.edges.filter((edge) =>
    (edgeKind.value === "all" || edge.kind === edgeKind.value) &&
    (edgeClassification.value === "all" || edge.classification === edgeClassification.value));
  return orientGraphEdges({
    ...shadowedGraph.value,
    edges,
    summary: { edges: edges.length, nodes: shadowedGraph.value.nodes.length },
  }, edgeFlow.value);
});
const sourcePath = computed(() => {
  if (!visibleGraph.value || !selectedId.value || !sourceAvailable.value) return null;
  const selected = visibleGraph.value.nodes.find((node) => node.id === selectedId.value);
  if (!selected) return null;
  let file = selected;
  if (selected.kind === "symbol" && selected.parent) {
    file = visibleGraph.value.nodes.find((node) => node.id === selected.parent) || selected;
  }
  if (file.kind === "file") return file.identity;
  return selected.kind === "symbol" && typeof selected.attributes.path === "string"
    ? selected.attributes.path
    : null;
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
  sourceSelection.value = null;
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
  if (entitySearchTimer !== null) window.clearTimeout(entitySearchTimer);
  entitySearchTimer = null;
  entitySearchGeneration += 1;
  entitySearchHits.value = [];
  entitySearchError.value = "";
  entitySearchLoading.value = false;
  entitySearchOpen.value = false;
  entitySearchActiveIndex.value = 0;
  metricGeneration += 1;
  graphPresentations.value = null;
  fileGraph.value = null;
  symbolGraphs.value = new Map();
  verification.value = null;
  metricOverlay.value = null;
  metricCache.value = new Map();
  metricLoading.value = new Set();
  metricErrors.value = new Map();
  graphHistory.value = [];
  pendingGraphViewport = null;
}

// App retains semantic view state; GraphCanvas owns its raw viewport snapshot.
function captureGraphNavigation(): GraphNavigationEntry | null {
  if (!artifact.value || !graph.value) return null;
  return {
    artifact: artifact.value,
    currentView: currentView.value,
    direction: direction.value,
    edgeClassification: edgeClassification.value,
    edgeFlow: edgeFlow.value,
    edgeKind: edgeKind.value,
    expandedFiles: new Set(expandedFiles.value),
    expandedGroups: new Set(expandedGroups.value),
    fileGraph: fileGraph.value,
    graph: graph.value,
    graphBytes: graphBytes.value,
    graphGrouping: graphGrouping.value,
    graphLayout: graphLayout.value,
    graphNotation: graphNotation.value,
    graphSpacing: graphSpacing.value,
    hiddenElements: new Set(hiddenElements.value),
    inspectorOpen: inspectorOpen.value,
    mapView: mapView.value,
    metricOverlay: metricOverlay.value,
    metricScope: metricScope.value,
    queryDepth: queryDepth.value,
    queryDirection: queryDirection.value,
    queryKind: queryKind.value,
    queryTestDepth: queryTestDepth.value,
    queryValue: queryValue.value,
    revealedSymbols: new Set(revealedSymbols.value),
    selectedConstraintId: selectedConstraintId.value,
    selectedId: selectedId.value,
    source: source.value,
    sourceSelection: sourceSelection.value,
    viewport: graphCanvas.value?.captureViewport() || null,
    zoomPercent: zoomPercent.value,
  };
}

async function backToPreviousGraph(): Promise<void> {
  const previous = graphHistory.value.at(-1);
  if (!previous) return;
  graphHistory.value = graphHistory.value.slice(0, -1);
  artifact.value = previous.artifact;
  currentView.value = previous.currentView;
  direction.value = previous.direction;
  edgeClassification.value = previous.edgeClassification;
  edgeFlow.value = previous.edgeFlow;
  edgeKind.value = previous.edgeKind;
  expandedFiles.value = previous.expandedFiles;
  expandedGroups.value = previous.expandedGroups;
  fileGraph.value = previous.fileGraph;
  graph.value = previous.graph;
  graphBytes.value = previous.graphBytes;
  graphGrouping.value = previous.graphGrouping;
  graphLayout.value = previous.graphLayout;
  graphNotation.value = previous.graphNotation;
  graphSpacing.value = previous.graphSpacing;
  hiddenElements.value = previous.hiddenElements;
  inspectorOpen.value = previous.inspectorOpen;
  mapView.value = previous.mapView;
  metricOverlay.value = previous.metricOverlay;
  metricScope.value = previous.metricScope;
  queryDepth.value = previous.queryDepth;
  queryDirection.value = previous.queryDirection;
  queryKind.value = previous.queryKind;
  queryTestDepth.value = previous.queryTestDepth;
  queryValue.value = previous.queryValue;
  revealedSymbols.value = previous.revealedSymbols;
  selectedConstraintId.value = previous.selectedConstraintId;
  selectedId.value = previous.selectedId;
  source.value = previous.source;
  sourceSelection.value = previous.sourceSelection;
  zoomPercent.value = previous.zoomPercent;
  pendingGraphViewport = previous.viewport;
  await nextTick();
}

function graphLayoutEnded(): void {
  graphLayoutBusy.value = false;
  if (!pendingGraphViewport) return;
  const viewport = pendingGraphViewport;
  pendingGraphViewport = null;
  requestAnimationFrame(() => graphCanvas.value?.restoreViewport(viewport));
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

function boundedDisplayIds(values: ReadonlySet<string>): {
  ids: string[];
  omitted: number;
} {
  const rows = [...values].sort();
  return { ids: rows.slice(0, 128), omitted: Math.max(0, rows.length - 128) };
}

function observedDisplay(): ObservedDisplay | null {
  if (!(liveHost instanceof ServerHost) || !artifact.value || !baseGeneration.value
      || workspaceMode.value === "saved") return null;
  const visible = visibleGraph.value;
  const selected = selectedId.value && visible
    ? visible.nodes.find((node) => node.id === selectedId.value) || null
    : null;
  const selectedFile = selected?.kind === "file"
    ? selected
    : selected?.parent && visible
      ? visible.nodes.find((node) => node.id === selected.parent && node.kind === "file") || null
      : null;
  const selectedPath = selectedFile?.identity
    || (typeof selected?.attributes.path === "string" ? selected.attributes.path : null);
  const expandedGroupRows = boundedDisplayIds(expandedGroups.value);
  const expandedFileRows = boundedDisplayIds(expandedFiles.value);
  const revealedSymbolRows = boundedDisplayIds(revealedSymbols.value);
  const hiddenRows = boundedDisplayIds(hiddenElements.value);
  const range = sourceSelection.value;
  return {
    artifact: artifact.value.artifact,
    expanded: {
      files: expandedFileRows.ids,
      groups: expandedGroupRows.ids,
      omitted: expandedFileRows.omitted + expandedGroupRows.omitted + revealedSymbolRows.omitted,
      symbols: revealedSymbolRows.ids,
    },
    generation: baseGeneration.value,
    graph: visible ? {
      direction: direction.value,
      edge_classification: edgeClassification.value,
      edge_flow: edgeFlow.value,
      edge_kind: edgeKind.value,
      edges: visible.edges.length,
      group_by: artifact.value.artifact === "map" ? graphGrouping.value : "none",
      layout: graphLayout.value,
      nodes: visible.nodes.length,
      notation: graphNotation.value,
      perspective: artifact.value.artifact === "map" ? mapView.value : "focused",
      view: currentView.value,
    } : null,
    hidden: hiddenRows,
    history_depth: graphHistory.value.length,
    selected: selected ? {
      id: selected.id,
      identity: selected.identity,
      kind: selected.kind,
      label: selected.label,
      ...(selectedPath ? { path: selectedPath } : {}),
    } : null,
    selected_constraint: selectedConstraintId.value,
    source: range ? {
      path: range.path,
      ...(range.start === undefined || range.end === undefined
        ? {}
        : { end: range.end, start: range.start }),
    } : null,
    workspace: workspaceMode.value,
  };
}

function scheduleObservedDisplay() {
  if (observedDisplayTimer !== null) window.clearTimeout(observedDisplayTimer);
  observedDisplayTimer = window.setTimeout(() => {
    observedDisplayTimer = null;
    const host = liveHost;
    const state = observedDisplay();
    if (host instanceof ServerHost && state) {
      void host.observeDisplay(state).catch(() => {
        // A generation may change while this non-evidence UI observation is in flight.
      });
    }
  }, 50);
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
  if (liveHost !== host) appliedDisplayRevision = 0;
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
    return;
  }
  if (event.type === "display-updated" && liveHost instanceof ServerHost) {
    try {
      void applyDisplay(displayState(event.payload), liveHost);
    } catch (cause) {
      reportOperationError(cause);
    }
  }
}

function rememberGraph(previous: GraphNavigationEntry | null) {
  if (!previous) return;
  graphHistory.value = [
    ...graphHistory.value.slice(-(GRAPH_HISTORY_LIMIT - 1)),
    previous,
  ];
}

function displaySourceSelection(params: Record<string, unknown>): {
  path: string;
  symbol: string | null;
} | null {
  const paths = Array.isArray(params.paths) ? params.paths : [];
  if (typeof paths[0] === "string" && paths[0]) {
    return { path: paths[0], symbol: null };
  }
  const symbols = Array.isArray(params.symbols) ? params.symbols : [];
  if (typeof symbols[0] !== "string") return null;
  const separator = symbols[0].lastIndexOf(":");
  return separator > 0
    ? {
      path: symbols[0].slice(0, separator),
      symbol: symbols[0].slice(separator + 1),
    }
    : null;
}

function installPathPresentation(loaded: ParsedArtifact, hasSource: boolean) {
  const projected = pathGraphFromArtifact(loaded.document);
  artifact.value = loaded;
  currentView.value = projected.request.view;
  graph.value = projected;
  graphBytes.value = new TextEncoder().encode(JSON.stringify(projected));
  edgeFlow.value = "uses";
  sourceAvailable.value = hasSource;
}

async function applyDisplay(state: DisplayState, host: ServerHost) {
  if (state.revision <= appliedDisplayRevision) return;
  const previous = captureGraphNavigation();
  const id = beginOperation();
  error.value = "";
  try {
    if (state.method === "map") {
      if (!baseMap.value || state.generation !== baseGeneration.value) {
        throw new Error("displayed Map does not match the current live generation");
      }
      const requestedView = state.display.view;
      const requestedGrouping = state.display.group_by;
      const view: MapView = ["architecture", "evidence", "metrics", "overview", "tests"]
        .includes(String(requestedView))
        ? requestedView as MapView
        : "overview";
      const grouping: GraphGrouping = ["component", "directory", "language", "layer"]
        .includes(String(requestedGrouping))
        ? requestedGrouping as GraphGrouping
        : "directory";
      const presentation = await mapPresentation(
        baseMap.value,
        host,
        view,
        grouping,
        state.generation,
      );
      if (!ownsOperation(id)) return;
      resetGraphState();
      installMapPresentation(baseMap.value, presentation);
      rememberGraph(previous);
    } else {
      const payload = await host.artifactDescriptor(state.artifact);
      const loaded = parseArtifact(
        new Uint8Array(payload.bytes),
        `${payload.project}.${state.method}.json`,
      );
      if (state.method === "query") {
        if (!state.presentation) {
          throw new Error("displayed Query has no graph presentation");
        }
        const graphPayload = await host.artifactDescriptor(state.presentation);
        const projected = {
          bytes: new Uint8Array(graphPayload.bytes),
          graph: graphFromBytes(new Uint8Array(graphPayload.bytes)),
        };
        const requestedSource = state.display.view === "source"
          ? displaySourceSelection(state.params)
          : null;
        const displayedSource = requestedSource
          ? await host.source(requestedSource.path, state.generation)
          : null;
        if (!ownsOperation(id)) return;
        resetGraphState();
        artifact.value = loaded;
        currentView.value = projected.graph.request.view;
        graph.value = projected.graph;
        graphBytes.value = projected.bytes;
        sourceAvailable.value = true;
        if (requestedSource) {
          const file = projected.graph.nodes.find((node) =>
            node.kind === "file" && node.identity === requestedSource.path);
          const symbol = requestedSource.symbol && file
            ? projected.graph.nodes.find((node) =>
              node.kind === "symbol"
              && node.label === requestedSource.symbol
              && node.parent === file.id)
            : null;
          if (symbol || file) selectedId.value = (symbol || file)!.id;
          sourceSelection.value = { path: requestedSource.path };
          source.value = displayedSource;
        }
        rememberGraph(previous);
      } else if (state.method === "path") {
        if (!ownsOperation(id)) return;
        resetGraphState();
        installPathPresentation(loaded, true);
        rememberGraph(previous);
      } else {
        if (!ownsOperation(id)) return;
        artifact.value = loaded;
        graph.value = null;
        graphBytes.value = null;
        graphHistory.value = [];
        sourceAvailable.value = false;
      }
    }
    appliedDisplayRevision = state.revision;
  } catch (cause) {
    if (ownsOperation(id)) reportOperationError(cause);
  } finally {
    finishOperation(id);
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
  const mapChanged = baseMap.value !== loaded;
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
  queueMicrotask(() => {
    if (mapChanged && baseMap.value === loaded && entitySearch.value.trim()) {
      scheduleEntitySearch(entitySearch.value);
    }
  });
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

async function ensureSymbolGraphs(paths: readonly string[]): Promise<void> {
  const missing = [...new Set(paths)]
    .filter((path) => !symbolGraphs.value.has(path))
    .sort();
  if (!missing.length) return;
  if (!baseMap.value) throw new Error("symbol projection requires a Map");
  const options = {
    artifacts: [],
    components: [],
    depth: 0,
    direction: "both",
    focus: [],
    packages: [],
    paths: missing,
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
  if (!projection) throw new Error("symbol projection is unavailable");
  const next = new Map(symbolGraphs.value);
  for (const path of missing) next.set(path, projection.graph);
  symbolGraphs.value = next;
}

async function ensureSymbolGraph(path: string): Promise<GraphView> {
  await ensureSymbolGraphs([path]);
  const graph = symbolGraphs.value.get(path);
  if (!graph) throw new Error(`symbol projection is unavailable for ${path}`);
  return graph;
}

async function activateGraphElement(id: string) {
  const node = visibleGraph.value?.nodes.find((row) => row.id === id);
  if (!node) return;
  if (node.attributes.presentation_hidden) {
    restoreGraphElement(id);
    return;
  }
  const activation = graphActivation(node);
  if (!activation) return;
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

function minimizeGraphElement(id: string) {
  const graph = visibleGraph.value;
  if (!graph) return;
  const descendants = new Set([id]);
  let changed = true;
  while (changed) {
    changed = false;
    for (const node of graph.nodes) {
      if (node.parent && descendants.has(node.parent) && !descendants.has(node.id)) {
        descendants.add(node.id);
        changed = true;
      }
    }
  }
  expandedGroups.value = new Set(
    [...expandedGroups.value].filter((value) => !descendants.has(value)),
  );
  expandedFiles.value = new Set(
    [...expandedFiles.value].filter((value) => !descendants.has(value)),
  );
  revealedSymbols.value = new Set(
    [...revealedSymbols.value].filter((value) => !descendants.has(value)),
  );
}

async function maximizeGraphElement(id: string) {
  const node = visibleGraph.value?.nodes.find((row) => row.id === id);
  if (!node) return;
  if (node.attributes.presentation_hidden) {
    restoreGraphElement(id);
    return;
  }
  const activation = graphActivation(node);
  if (activation === "symbol") {
    revealedSymbols.value = replaceSet(revealedSymbols.value, id, true);
    return;
  }
  if (activation === "file") {
    const path = typeof node.attributes.canonical_path === "string"
      ? node.attributes.canonical_path
      : node.identity;
    const operation = beginOperation();
    progress.value = { phase: `loading symbols for ${path}` };
    try {
      await ensureSymbolGraph(path);
      if (ownsOperation(operation)) {
        expandedFiles.value = replaceSet(expandedFiles.value, id, true);
        progress.value = null;
      }
    } catch (cause) {
      if (ownsOperation(operation)) reportOperationError(cause);
    } finally {
      finishOperation(operation);
    }
    return;
  }
  if (activation !== "group") return;
  const operation = beginOperation();
  progress.value = { phase: `expanding ${node.label}` };
  try {
    requireFileGraph();
    const rootGroup = typeof node.attributes.root_group === "string"
      ? node.attributes.root_group
      : node.id;
    const selectedPath = typeof node.attributes.path === "string"
      ? node.attributes.path
      : null;
    const rootPath = selectedPath || (typeof node.attributes.root_path === "string"
      ? node.attributes.root_path
      : null);
    const memberFiles = Array.isArray(node.attributes.member_files)
      ? node.attributes.member_files.filter(
        (value): value is string => typeof value === "string",
      )
      : [];
    const expanded = new Set(expandedGroups.value);
    const expandedFileIds = new Set(expandedFiles.value);
    expanded.add(id);
    for (const file of memberFiles) {
      const segments = file.split("/");
      let prefix = "";
      for (const segment of segments.slice(0, -1)) {
        prefix = prefix ? `${prefix}/${segment}` : segment;
        if (rootPath && prefix !== rootPath && !prefix.startsWith(`${rootPath}/`)) {
          continue;
        }
        if (prefix !== rootPath || selectedPath) {
          expanded.add(directoryPresentationId(rootGroup, prefix));
        }
      }
    }
    await ensureSymbolGraphs(memberFiles);
    const canonicalGroup = graph.value?.nodes.find((row) => row.id === rootGroup);
    const instanceGroup = canonicalGroup?.kind === "component"
      ? canonicalGroup.label
      : rootGroup;
    for (const file of memberFiles) {
      expandedFileIds.add(filePresentationId(instanceGroup, file));
    }
    if (ownsOperation(operation)) {
      expandedGroups.value = expanded;
      expandedFiles.value = expandedFileIds;
      progress.value = null;
    }
  } catch (cause) {
    if (ownsOperation(operation)) reportOperationError(cause);
  } finally {
    finishOperation(operation);
  }
}

function hideGraphElement(id: string) {
  minimizeGraphElement(id);
  hiddenElements.value = replaceSet(hiddenElements.value, id, true);
}

function restoreGraphElement(id: string) {
  hiddenElements.value = replaceSet(hiddenElements.value, id, false);
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

async function loadMetric(metric: MetricId) {
  if (metricCache.value.has(metric) || metricLoading.value.has(metric) || !baseMap.value) {
    return;
  }
  const generation = metricGeneration;
  metricLoading.value = new Set(metricLoading.value).add(metric);
  const errors = new Map(metricErrors.value);
  errors.delete(metric);
  metricErrors.value = errors;
  try {
    const plan = metricProjectionPlan(metric);
    const host = workspaceMode.value !== "saved" && liveHost
      && !(workspaceMode.value === "snapshot" && liveHost instanceof BrowserHost)
      ? liveHost
      : null;
    const projection = host
      ? await hostFactProjection(host, plan, baseGeneration.value || undefined)
      : await evaluateArtifactProjection(
        requireParsedArtifact(baseMap.value, "local Metrics projection"),
        plan,
      ).then((bytes) => parseProjectionResult(bytes, "metrics"));
    if (generation !== metricGeneration) return;
    metricCache.value = new Map(metricCache.value).set(metric, projection);
  } catch (cause) {
    if (generation === metricGeneration) {
      metricErrors.value = new Map(metricErrors.value).set(
        metric,
        cause instanceof Error ? cause.message : String(cause),
      );
    }
  } finally {
    if (generation === metricGeneration) {
      const loading = new Set(metricLoading.value);
      loading.delete(metric);
      metricLoading.value = loading;
    }
  }
}

function setMetricOverlay(metric: MetricId | null): void {
  metricOverlay.value = metric;
}

function openMetricContext(path: string, symbol: string | null) {
  queryKind.value = symbol ? "symbol" : "path";
  queryValue.value = symbol ? `${path}:${symbol}` : path;
  void runQuery();
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
    if (loaded.artifact === "path") {
      installPathPresentation(loaded, false);
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
  const retainedVerification = verification.value;
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
    verification.value = retainedVerification;
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
  const request: Record<string, unknown> = {
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
  if (Array.isArray(options.searchFields) && options.searchFields.length) {
    request.search_fields = options.searchFields;
  }
  return request;
}

function queryHost(): BrowserHost | ServerHost | null {
  return workspaceMode.value !== "saved" && liveHost
    && !(workspaceMode.value === "snapshot" && liveHost instanceof BrowserHost)
    ? liveHost
    : null;
}

function entitySearchOptions(value: string): Record<string, unknown> {
  return {
    artifacts: [],
    components: [],
    depth: 0,
    direction: "both",
    focus: [],
    packages: [],
    paths: [],
    search: [value],
    searchFields: ["default", "documentation", "type"],
    searchLimit: 30,
    symbols: [],
    testDepth: 0,
  };
}

async function loadEntitySearch(value: string, generation: number) {
  if (!baseMap.value || generation !== entitySearchGeneration) return;
  entitySearchLoading.value = true;
  entitySearchError.value = "";
  try {
    const options = entitySearchOptions(value);
    const host = queryHost();
    const loaded = host
      ? parseArtifact(
        new Uint8Array((await host.query(
          options,
          baseGeneration.value || undefined,
        )).bytes),
        `${artifactProject.value}.search.query.json`,
      )
      : await queryArtifact(
        requireParsedArtifact(baseMap.value, "local entity search"),
        nativeQueryRequest(options),
      );
    if (generation !== entitySearchGeneration) return;
    entitySearchHits.value = queryRetrievalHits(loaded.document);
    entitySearchActiveIndex.value = 0;
  } catch (cause) {
    if (generation !== entitySearchGeneration) return;
    entitySearchHits.value = [];
    entitySearchError.value = cause instanceof Error ? cause.message : String(cause);
  } finally {
    if (generation === entitySearchGeneration) entitySearchLoading.value = false;
  }
}

function scheduleEntitySearch(value: string) {
  if (entitySearchTimer !== null) window.clearTimeout(entitySearchTimer);
  const generation = ++entitySearchGeneration;
  entitySearchHits.value = [];
  entitySearchError.value = "";
  entitySearchLoading.value = false;
  const query = value.trim();
  entitySearchOpen.value = Boolean(query && baseMap.value);
  entitySearchActiveIndex.value = 0;
  if (!query || !baseMap.value) return;
  entitySearchTimer = window.setTimeout(() => {
    entitySearchTimer = null;
    void loadEntitySearch(query, generation);
  }, 180);
}

function queryOptionsForHit(hit: QueryRetrievalHit): Record<string, unknown> {
  const options = entitySearchOptions("");
  options.search = [];
  options.searchFields = [];
  if (hit.kind === "symbol") {
    if (!hit.path) throw new Error("symbol retrieval hit has no source path");
    options.symbols = [`${hit.path}:${hit.name}`];
  } else if (hit.kind === "file") {
    if (!hit.path) throw new Error("file retrieval hit has no source path");
    options.paths = [hit.path];
  }
  else if (hit.kind === "component") options.components = [hit.name];
  else if (hit.kind === "package") options.packages = [hit.name];
  else if (hit.kind === "artifact") options.artifacts = [hit.name];
  else throw new Error(`unsupported Query retrieval kind: ${hit.kind}`);
  return options;
}

function resultNode(view: GraphView, hit: QueryRetrievalHit) {
  if (hit.kind === "symbol") {
    return view.nodes.find((node) => {
      if (node.kind !== "symbol" || node.label !== hit.name || !node.parent) return false;
      if (hit.line !== undefined && node.attributes.line !== hit.line) return false;
      if (hit.symbolKind && node.attributes.kind !== hit.symbolKind) return false;
      return view.nodes.find((parent) => parent.id === node.parent)?.identity === hit.path;
    });
  }
  return view.nodes.find((node) => node.kind === hit.kind && (
    (hit.path !== undefined && node.identity === hit.path) || node.label === hit.name
  )) || view.nodes.find((node) =>
    node.kind === "file" && hit.path !== undefined && node.identity === hit.path);
}

async function runQueryOptions(
  options: Record<string, unknown>,
  hit: QueryRetrievalHit | null = null,
) {
  if (!baseMap.value) return;
  const previous = captureGraphNavigation();
  const id = beginOperation();
  error.value = "";
  resetGraphState();
  try {
    const host = queryHost();
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
    rememberGraph(previous);
    if (hit && projection?.graph) {
      const selected = resultNode(projection.graph, hit);
      if (selected) {
        selectGraphElement(selected.id);
        await nextTick();
        requestAnimationFrame(() => graphCanvas.value?.center(selected.id));
      }
    }
  } catch (cause) {
    if (ownsOperation(id)) error.value = (cause as Error).message;
  } finally {
    finishOperation(id);
  }
}

async function runQuery() {
  await runQueryOptions(queryOptions());
}

async function openEntitySearchHit(hit: QueryRetrievalHit) {
  entitySearchOpen.value = false;
  entitySearchHits.value = [];
  entitySearch.value = "";
  if (["artifact", "component", "package"].includes(hit.kind)) {
    if (!baseMap.value) return;
    const previous = captureGraphNavigation();
    const id = beginOperation();
    error.value = "";
    resetGraphState();
    try {
      const presentation = await mapPresentation(
        baseMap.value,
        queryHost(),
        "architecture",
        "component",
        baseGeneration.value || undefined,
      );
      if (!ownsOperation(id)) return;
      installMapPresentation(baseMap.value, presentation);
      rememberGraph(previous);
      const selected = resultNode(presentation.overview, hit);
      if (!selected) throw new Error(`${hit.kind} is absent from the architecture view`);
      selectGraphElement(selected.id);
      await nextTick();
      requestAnimationFrame(() => graphCanvas.value?.center(selected.id));
    } catch (cause) {
      if (ownsOperation(id)) reportOperationError(cause);
    } finally {
      finishOperation(id);
    }
    return;
  }
  await runQueryOptions(queryOptionsForHit(hit), hit);
}

function openFirstEntitySearchHit() {
  const hit = entitySearchHits.value[entitySearchActiveIndex.value];
  if (entitySearchOpen.value && hit) void openEntitySearchHit(hit);
}

function openEntitySearch() {
  if (entitySearch.value.trim()) entitySearchOpen.value = true;
  scheduleEntitySearch(entitySearch.value);
}

function closeEntitySearch() {
  entitySearchOpen.value = false;
}

function moveEntitySearchSelection(offset: number) {
  if (!entitySearchHits.value.length) return;
  entitySearchOpen.value = true;
  entitySearchActiveIndex.value = (
    entitySearchActiveIndex.value + offset + entitySearchHits.value.length
  ) % entitySearchHits.value.length;
}

function dismissEntitySearchOutside(event: PointerEvent) {
  if (!entitySearchRoot.value?.contains(event.target as Node)) {
    closeEntitySearch();
  }
}

function entitySearchOptionId(index: number): string {
  return `entity-search-option-${index}`;
}

function entitySearchResultType(hit: QueryRetrievalHit): string {
  return hit.symbolKind || hit.kind;
}

function entitySearchResultReason(hit: QueryRetrievalHit): string {
  const reason = hit.reasons[0];
  if (!reason) return "retrieval match";
  return `${reason.field.split(".").at(-1)} · ${reason.match}`;
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

async function openSource(selection: SourceSelection) {
  if (!liveHost || !sourceAvailable.value) return;
  error.value = "";
  sourceSelection.value = selection;
  try {
    source.value = await liveHost.source(selection.path, baseGeneration.value || undefined);
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
  let state: Record<string, unknown> | null = null;
  let refreshed = false;
  try {
    state = await host.state();
    const loaded = liveMapReference(state);
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
    refreshed = true;
  } catch (cause) {
    if (ownsOperation(id)) error.value = (cause as Error).message;
  } finally {
    finishOperation(id);
    if (refreshed && state?.display && liveHost === host && workspaceMode.value === "live") {
      queueMicrotask(() => {
        try {
          void applyDisplay(displayState(state!.display), host);
        } catch (cause) {
          reportOperationError(cause);
        }
      });
    }
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
watch(entitySearch, scheduleEntitySearch);
watch(
  [
    artifact,
    baseGeneration,
    currentView,
    direction,
    edgeClassification,
    edgeFlow,
    edgeKind,
    expandedFiles,
    expandedGroups,
    graphGrouping,
    graphLayout,
    graphNotation,
    hiddenElements,
    mapView,
    revealedSymbols,
    selectedConstraintId,
    selectedId,
    sourceSelection,
    visibleGraph,
    workspaceMode,
  ],
  scheduleObservedDisplay,
  { flush: "post" },
);

onBeforeUnmount(() => {
  if (entitySearchTimer !== null) window.clearTimeout(entitySearchTimer);
  if (observedDisplayTimer !== null) window.clearTimeout(observedDisplayTimer);
  document.removeEventListener("pointerdown", dismissEntitySearchOutside);
  unsubscribe?.();
  themeMedia?.removeEventListener("change", systemThemeChanged);
  void liveHost?.dispose();
});

function systemThemeChanged(event: MediaQueryListEvent) {
  systemDark.value = event.matches;
}

onMounted(async () => {
  document.addEventListener("pointerdown", dismissEntitySearchOutside);
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
    <header class="topbar" :class="{ 'source-empty': !artifact && !busy && !error }">
      <div class="brand">
        <div>
          <strong class="brand-wordmark" aria-label="ǎrchbird">
            <span aria-hidden="true"><span class="brand-initial">ǎ</span>rchbird</span>
          </strong>
        </div>
      </div>
      <ArtifactDrop
        v-if="artifact || busy || error"
        :status="inputStatus"
        @select="open"
        @directory="openLive({ kind: 'directory', files: $event })"
        @zip="openLive({ kind: 'zip', file: $event })"
      />
      <div v-if="artifact" ref="entitySearchRoot" class="search entity-search">
        <label>
          <span>Search project map</span>
          <input
            v-model="entitySearch"
            role="combobox"
            aria-autocomplete="list"
            aria-controls="entity-search-results"
            :aria-expanded="entitySearchOpen"
            :aria-activedescendant="entitySearchOpen && entitySearchHits.length
              ? entitySearchOptionId(entitySearchActiveIndex)
              : undefined"
            :disabled="!baseMap"
            placeholder="name, type, or documentation…"
            type="search"
            @focus="openEntitySearch"
            @keydown.down.prevent="moveEntitySearchSelection(1)"
            @keydown.up.prevent="moveEntitySearchSelection(-1)"
            @keydown.enter.prevent="openFirstEntitySearchHit"
            @keydown.esc.stop.prevent="closeEntitySearch"
          />
        </label>
        <div
          v-if="entitySearchOpen && entitySearch.trim()"
          id="entity-search-results"
          class="entity-search-results"
          aria-label="Project Map search results"
          role="listbox"
        >
          <p v-if="entitySearchLoading">Searching Map…</p>
          <button
            v-for="(hit, index) in entitySearchHits"
            :id="entitySearchOptionId(index)"
            :key="`${hit.kind}:${hit.path || ''}:${hit.name}:${hit.line || 0}`"
            :aria-selected="index === entitySearchActiveIndex"
            :class="{ active: index === entitySearchActiveIndex }"
            role="option"
            tabindex="-1"
            type="button"
            @click="openEntitySearchHit(hit)"
            @pointerenter="entitySearchActiveIndex = index"
          >
            <span>{{ hit.name }}</span>
            <small>{{ entitySearchResultType(hit) }}<template v-if="hit.path"> · {{ hit.path }}</template><template v-if="hit.line">:{{ hit.line }}</template></small>
            <small>{{ entitySearchResultReason(hit) }}</small>
          </button>
          <p v-if="!entitySearchLoading && entitySearchError">{{ entitySearchError }}</p>
        </div>
      </div>
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
      <p class="eyebrow">Architecture explorer</p>
      <h1>See how a codebase is built.</h1>
      <p>
        Map files, symbols, dependencies, tests, builds, and constraints. Then
        follow exact source evidence from the overview into focused context.
      </p>
      <ArtifactDrop
        prominent
        :status="inputStatus"
        @select="open"
        @directory="openLive({ kind: 'directory', files: $event })"
        @zip="openLive({ kind: 'zip', file: $event })"
      />
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
          <button
            v-if="baseMap && artifact !== baseMap && !graphHistory.length"
            type="button"
            @click="showBaseMap"
          >Back to Map</button>
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
            <label>Layout
              <select v-model="graphLayout">
                <option value="flow">Flow</option>
                <option value="force">Force</option>
                <option value="radial">Radial</option>
              </select>
            </label>
            <label>Notation
              <select v-model="graphNotation">
                <option value="architecture">Architecture</option>
                <option value="compact">Compact</option>
              </select>
            </label>
            <label>Direction
              <select v-model="direction" :disabled="graphLayout !== 'flow'">
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
          :can-go-back="Boolean(graphHistory.length)"
          :direction="direction"
          :graph="visibleGraph"
          :layout-mode="graphLayout"
          :notation="graphNotation"
          :selected-id="selectedId"
          :spacing="graphSpacing"
          :theme="resolvedTheme"
          @activate="activateGraphElement"
          @back="backToPreviousGraph"
          @hide="hideGraphElement"
          @layout-end="graphLayoutEnded"
          @layout-start="graphLayoutBusy = true"
          @maximize="maximizeGraphElement"
          @minimize="minimizeGraphElement"
          @select="selectGraphElement"
          @zoom="zoomPercent = $event"
        >
          <div
            v-if="pathStatus"
            class="path-graph-status"
            :data-status="pathStatus.outcome"
          >
            <strong>Path · {{ pathStatus.outcome }}</strong>
            <span>{{ pathStatus.detail }}</span>
          </div>
        </GraphCanvas>
        <InspectorPanel
          v-if="!emptyMappedScope"
          v-show="inspectorOpen"
          :constraint="selectedConstraint"
          :file-graph="fileGraph"
          :graph="visibleGraph"
          :metric-errors="metricErrors"
          :metrics-enabled="Boolean(baseMap)"
          :metric-loading="metricLoading"
          :metric-overlay="metricOverlay"
          :metric-projections="metricCache"
          :metric-scope="metricScope"
          :selected-id="selectedId"
          :source="source"
          :source-path="sourcePath"
          :source-selection="sourceSelection"
          @activate="activateGraphElement"
          @close="inspectorOpen = false"
          @hide="hideGraphElement"
          @metric-load="loadMetric"
          @metric-overlay="setMetricOverlay"
          @metric-query="openMetricContext"
          @metric-scope="metricScope = $event"
          @restore="restoreGraphElement"
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
