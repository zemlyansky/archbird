<script setup lang="ts">
import cytoscape, {
  type Core,
  type EventObject,
} from "cytoscape";
import ELK from "elkjs/lib/elk-api.js";
import ELKWorker from "elkjs/lib/elk-worker.min.js?worker";
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import type { GraphView } from "../artifacts/model";
import { edgePresentation, graphActivation } from "../graph/explore";
import { documentationPreview } from "../graph/documentation";
import {
  elkLeafSize,
  elkLayoutGraph,
  elkLayoutOptions,
  elkPositions,
  type GraphLayoutMode,
  type ElkNode,
} from "../graph/layout";
import {
  architectureNodeCard,
  architectureNodeLabel,
  type GraphNotation,
} from "../graph/notation";

const props = defineProps<{
  canGoBack?: boolean;
  direction: "BT" | "LR" | "RL" | "TB";
  graph: GraphView;
  layoutMode: GraphLayoutMode;
  notation: GraphNotation;
  selectedId: string | null;
  spacing: number;
  theme: "dark" | "light";
}>();
const emit = defineEmits<{
  activate: [id: string];
  back: [];
  hide: [id: string];
  layoutEnd: [];
  layoutStart: [];
  maximize: [id: string];
  minimize: [id: string];
  select: [id: string | null];
  zoom: [percent: number];
}>();
const container = ref<HTMLElement | null>(null);
let cy: Core | null = null;
let requestId = 0;
let lastNodeTap: { at: number; id: string } | null = null;
const layoutError = ref("");
const layoutReady = ref(false);
const layoutGeneration = ref(0);
const hasCompletedLayout = ref(false);
const layoutAnchorDrift = ref<number | null>(null);
const focusCount = ref(0);
const dimmedCount = ref(0);
const layoutExtent = ref<{ height: number; width: number } | null>(null);
const panPosition = ref<{ x: number; y: number } | null>(null);
const selectedModel = ref<{ x: number; y: number } | null>(null);
const selectedRendered = ref<{ x: number; y: number } | null>(null);
const nodeActionPosition = ref<{ x: number; y: number } | null>(null);
const nodeActionInset = ref<number | null>(null);
const hoveredNodeId = ref<string | null>(null);
const middlePanning = ref(false);
const hover = ref<{
  documentation: string;
  details: string[];
  eyebrow: string;
  subtitle: string;
  title: string;
  x: number;
  y: number;
} | null>(null);
let wheelDelta = 0;
let wheelFrame = 0;
let wheelPoint = { x: 0, y: 0 };
let middlePanPoint: { x: number; y: number } | null = null;
let actionHideTimer = 0;
const elk = new ELK({
  workerFactory: () => new ELKWorker(),
});
const actionNodeId = computed(() => hoveredNodeId.value || props.selectedId);
const actionNode = computed(() => actionNodeId.value
  ? props.graph.nodes.find((node) => node.id === actionNodeId.value) || null
  : null);
const actionHidden = computed(() =>
  Boolean(actionNode.value?.attributes.presentation_hidden));
const actionActivation = computed(() =>
  actionNode.value ? graphActivation(actionNode.value) : null);
const actionExpanded = computed(() => Boolean(
  actionNode.value?.kind === "symbol"
    ? actionNode.value.attributes.relations_revealed
    : actionNode.value?.attributes.expanded,
));

const colors: Record<string, string> = {
  component: "#7dd3fc",
  directory: "#fdba74",
  layer: "#f9a8d4",
  language: "#67e8f9",
  file: "#c4b5fd",
  inventory: "#a7b6c8",
  symbol: "#86efac",
  package: "#facc15",
  build: "#fb7185",
  builtin: "#fcd34d",
  external: "#94a3b8",
  unresolved: "#fca5a5",
  unknown: "#fca5a5",
};
const hiddenEdgeOpacity = 0;

function nodeAccent(node: GraphView["nodes"][number]): string {
  const status = String(node.attributes.verification_status || "");
  if (status === "fail") return "var(--danger)";
  if (status === "unknown") return "var(--warning)";
  if (status === "pass") return "var(--success)";
  return colors[node.kind] || "#94a3b8";
}

interface PreservedViewport {
  anchorId: string;
  anchorModel: { x: number; y: number };
  anchorRendered: { x: number; y: number };
  pan: { x: number; y: number };
  positions: Record<string, { x: number; y: number }>;
  zoom: number;
}

function metricColor(value: unknown): string {
  const ratio = Math.max(0, Math.min(1, Number(value) || 0));
  const low = [125, 211, 252];
  const high = [14, 116, 144];
  const channel = (index: number) => Math.round(
    low[index] + (high[index] - low[index]) * ratio,
  );
  return `rgb(${channel(0)}, ${channel(1)}, ${channel(2)})`;
}

function elements(
  viewport: PreservedViewport | null,
  architectureCards: ReadonlyMap<string, string> = new Map(),
) {
  const parents = new Set(
    props.graph.nodes.flatMap((node) => node.parent ? [node.parent] : []),
  );
  return [
    ...props.graph.nodes.map((node) => {
      const initial = viewport?.positions[node.id] || viewport?.anchorModel;
      const metricValue = Number(node.attributes.metric_value);
      const metricPresent = Number.isFinite(metricValue);
      return {
        data: {
          id: node.id,
          label: node.label,
          architectureImage: architectureCards.get(node.id) || "none",
          architectureLabel: architectureNodeLabel(node),
          kind: node.kind,
          identity: node.identity,
          expanded: Boolean(node.attributes.expanded),
          presentationHidden: node.attributes.presentation_hidden ? 1 : 0,
          presentationRole: String(node.attributes.presentation_role || ""),
          verificationFindings: Number(node.attributes.verification_findings || 0),
          verificationStatus: String(node.attributes.verification_status || ""),
          metricName: metricPresent ? String(node.attributes.metric_name || "metric") : "",
          metricPopulation: metricPresent
            ? Number(node.attributes.metric_population || 1)
            : 0,
          metricPresent: metricPresent ? 1 : 0,
          metricRatio: metricPresent ? Number(node.attributes.metric_ratio || 0) : 0,
          metricState: metricPresent ? String(node.attributes.metric_state || "") : "",
          metricValue: metricPresent ? metricValue : 0,
          ...(node.parent ? { parent: node.parent } : {}),
        },
        ...(!parents.has(node.id) && initial
          ? { position: { ...initial } }
          : {}),
      };
    }),
    ...props.graph.edges.map((edge) => ({
      data: {
        id: edge.id,
        source: edge.source,
        target: edge.target,
        label: edge.kind,
        classification: edge.classification,
        verificationFindings: Number(edge.attributes?.verification_findings || 0),
        verificationStatus: String(edge.attributes?.verification_status || ""),
        presentationHidden: edge.attributes?.presentation_hidden ? 1 : 0,
      },
    })),
  ];
}

function fitPadding(): number {
  return Math.min(42, Math.max(18, (cy?.width() || 0) * 0.06));
}

function updateSelectedPosition() {
  panPosition.value = cy ? { ...cy.pan() } : null;
  if (!cy || !props.selectedId) {
    selectedModel.value = null;
    selectedRendered.value = null;
    if (!cy) nodeActionPosition.value = null;
  } else {
    const selected = cy.getElementById(props.selectedId);
    if (selected.length && selected.isNode()) {
      selectedModel.value = { ...selected.position() };
      if (selected.isParent()) {
        const bounds = selected.renderedBoundingBox({
          includeEdges: false,
          includeLabels: false,
          includeNodes: true,
          includeOverlays: false,
        });
        const inset = Math.max(3, Math.min(8, bounds.w / 4, bounds.h / 4));
        selectedRendered.value = {
          x: bounds.x1 + inset,
          y: bounds.y1 + inset,
        };
      } else {
        selectedRendered.value = { ...selected.renderedPosition() };
      }
    } else {
      selectedModel.value = null;
      selectedRendered.value = null;
    }
  }
  const actionId = actionNodeId.value;
  const action = actionId ? cy?.getElementById(actionId) : null;
  if (action?.length && action.isNode()) {
    const actionBounds = action.renderedBoundingBox({
      includeEdges: false,
      includeLabels: false,
      includeNodes: true,
      includeOverlays: false,
    });
    const borderWidth = Number.parseFloat(String(action.style("border-width"))) || 1.5;
    const inset = Math.max(5, Math.ceil(borderWidth * (cy?.zoom() || 1)) + 2);
    nodeActionInset.value = inset;
    nodeActionPosition.value = {
      x: Math.max(52, Math.min(
        (container.value?.clientWidth || 0) - 4,
        actionBounds.x2 - inset,
      )),
      y: Math.max(3, Math.min(
        (container.value?.clientHeight || 0) - 22,
        actionBounds.y1 + inset,
      )),
    };
  } else {
    nodeActionPosition.value = null;
    nodeActionInset.value = null;
  }
}

function applySelectionFocus(id: string | null) {
  if (!cy) return;
  const all = cy.elements();
  all.removeClass("focus-context focus-dimmed");
  focusCount.value = 0;
  dimmedCount.value = 0;
  if (!id) return;
  const selected = cy.getElementById(id);
  if (!selected.length) return;
  let context = selected;
  const selectedNodes = selected.nodes();
  const selectedEdges = selected.edges();
  if (selectedNodes.length) {
    const hierarchy = selectedNodes
      .union(selectedNodes.ancestors())
      .union(selectedNodes.descendants());
    const incident = hierarchy.connectedEdges();
    const neighbors = incident.connectedNodes();
    context = hierarchy
      .union(incident)
      .union(neighbors)
      .union(neighbors.ancestors());
  } else if (selectedEdges.length) {
    const endpoints = selectedEdges.connectedNodes();
    context = selectedEdges.union(endpoints).union(endpoints.ancestors());
  }
  const dimmed = all.difference(context);
  context.addClass("focus-context");
  dimmed.addClass("focus-dimmed");
  focusCount.value = context.length;
  dimmedCount.value = dimmed.length;
}

function preservedViewport(): PreservedViewport | null {
  if (!cy || !cy.nodes().length) return null;
  const selected = props.selectedId ? cy.getElementById(props.selectedId) : null;
  const center = { x: cy.width() / 2, y: cy.height() / 2 };
  const anchor = selected?.length && selected.isNode()
    ? selected
    : cy.nodes().toArray().reduce((closest, node) => {
      const position = node.renderedPosition();
      const distance = Math.hypot(position.x - center.x, position.y - center.y);
      return distance < closest.distance ? { distance, node } : closest;
    }, {
      distance: Number.POSITIVE_INFINITY,
      node: cy.nodes().first(),
    }).node;
  if (!anchor.length) return null;
  return {
    anchorId: anchor.id(),
    anchorModel: { ...anchor.position() },
    anchorRendered: { ...anchor.renderedPosition() },
    pan: { ...cy.pan() },
    positions: Object.fromEntries(cy.nodes().map((node) => [
      node.id(),
      { ...node.position() },
    ])),
    zoom: cy.zoom(),
  };
}

function pinRenderedAnchor(
  instance: Core,
  viewport: PreservedViewport,
): number | null {
  const anchor = instance.getElementById(viewport.anchorId);
  if (!anchor.length || !anchor.isNode()) return null;
  const rendered = anchor.renderedPosition();
  instance.panBy({
    x: viewport.anchorRendered.x - rendered.x,
    y: viewport.anchorRendered.y - rendered.y,
  });
  const adjusted = anchor.renderedPosition();
  return Math.hypot(
    adjusted.x - viewport.anchorRendered.x,
    adjusted.y - viewport.anchorRendered.y,
  );
}

async function layout(viewport: PreservedViewport | null) {
  if (!cy) return;
  const instance = cy;
  const id = ++requestId;
  layoutError.value = "";
  layoutReady.value = false;
  layoutExtent.value = null;
  layoutAnchorDrift.value = null;
  hover.value = null;
  emit("layoutStart");
  let anchorFrame = 0;
  let pinning = false;
  try {
    const layoutGraph = elkLayoutGraph(
      props.graph,
      props.spacing,
      props.notation,
      props.layoutMode,
      props.selectedId,
    );
    const result = await elk.layout({
      id: "root",
      layoutOptions: elkLayoutOptions(
        props.direction,
        props.spacing,
        props.layoutMode,
      ),
      ...layoutGraph,
    }) as { children?: ElkNode[] };
    if (cy !== instance || id !== requestId) return;
    const positions = elkPositions(result.children || []);
    const targetAnchor = viewport ? positions[viewport.anchorId] : null;
    const translation = viewport && targetAnchor
      ? {
          x: viewport.anchorModel.x - targetAnchor.x,
          y: viewport.anchorModel.y - targetAnchor.y,
        }
      : { x: 0, y: 0 };
    const leafPositions = Object.fromEntries(
      instance.nodes().not(":parent").map((node) => [
        node.id(),
        node.data("presentationHidden") && viewport?.positions[node.id()]
          ? viewport.positions[node.id()]
          : {
              x: (positions[node.id()]?.x || 0) + translation.x,
              y: (positions[node.id()]?.y || 0) + translation.y,
            },
      ]),
    );
    const animate = Boolean(viewport && hasCompletedLayout.value);
    pinning = animate;
    const pinDuringAnimation = () => {
      if (!pinning || !viewport || cy !== instance || id !== requestId) return;
      pinRenderedAnchor(instance, viewport);
      anchorFrame = requestAnimationFrame(pinDuringAnimation);
    };
    if (animate) anchorFrame = requestAnimationFrame(pinDuringAnimation);
    await new Promise<void>((resolve) => {
      instance.layout({
        animate,
        animationDuration: 280,
        animationEasing: "ease-out-cubic",
        fit: false,
        name: "preset",
        positions: leafPositions,
        stop: () => resolve(),
      }).run();
    });
    pinning = false;
    if (anchorFrame) cancelAnimationFrame(anchorFrame);
    if (cy !== instance || id !== requestId) return;
    const anchor = viewport ? instance.getElementById(viewport.anchorId) : null;
    if (viewport && anchor?.length && anchor.isNode()) {
      instance.zoom(viewport.zoom);
      instance.pan(viewport.pan);
      layoutAnchorDrift.value = pinRenderedAnchor(instance, viewport);
    } else {
      fit();
    }
    if (props.selectedId) {
      const selected = instance.getElementById(props.selectedId);
      if (selected.length) {
        selected.select();
      }
    }
    applySelectionFocus(props.selectedId);
    updateSelectedPosition();
    const bounds = instance.elements().boundingBox({
      includeEdges: false,
      includeLabels: false,
      includeNodes: true,
      includeOverlays: false,
    });
    layoutExtent.value = { height: bounds.h, width: bounds.w };
    layoutGeneration.value += 1;
    layoutReady.value = true;
    hasCompletedLayout.value = true;
    emit("layoutEnd");
  } catch (error) {
    pinning = false;
    if (anchorFrame) cancelAnimationFrame(anchorFrame);
    if (id === requestId) {
      layoutError.value = (error as Error).message;
      layoutReady.value = false;
      emit("layoutEnd");
    }
  }
}

function render() {
  if (!container.value) return;
  const candidate = preservedViewport();
  const viewport = candidate && props.graph.nodes.some((node) =>
    node.id === candidate.anchorId)
    ? candidate
    : null;
  if (!viewport) hasCompletedLayout.value = false;
  layoutReady.value = false;
  hover.value = null;
  const architecture = props.notation === "architecture";
  const leafSize = elkLeafSize(props.spacing, props.notation);
  const tokens = getComputedStyle(document.documentElement);
  const token = (name: string, fallback: string) =>
    tokens.getPropertyValue(name).trim() || fallback;
  const architectureCards = new Map(props.graph.nodes.map((node) => [
    node.id,
    architectureNodeCard(node, {
      accent: colors[node.kind] || "#94a3b8",
      danger: token("--danger", "#e46f6f"),
      success: token("--success", "#49b979"),
      text: token("--text", "#e6edf5"),
      warning: token("--warning", "#e2ae52"),
    }, leafSize),
  ]));
  cy?.destroy();
  cy = cytoscape({
    container: container.value,
    elements: elements(viewport, architectureCards),
    layout: { name: "preset" },
    minZoom: 0.05,
    maxZoom: 3,
    style: [
      {
        selector: "node",
        style: {
          "background-color": architecture
            ? token("--panel-strong", "#1a2431")
            : (element) => colors[element.data("kind")] || "#94a3b8",
          "background-fit": "none",
          "background-height": "100%",
          "background-height-relative-to": "inner",
          "background-image": architecture ? "data(architectureImage)" : "none",
          "background-image-opacity": 1,
          "background-opacity": 1,
          "background-position-x": "0%",
          "background-position-y": "0%",
          "background-width": "100%",
          "background-width-relative-to": "inner",
          "border-color": architecture
            ? (element) => colors[element.data("kind")] || "#94a3b8"
            : token("--bg", "#0b0f14"),
          "border-width": "1.5px",
          color: token("--text", "#e6edf5"),
          "font-family": "IBM Plex Mono, ui-monospace, monospace",
          "font-size": "10px",
          label: architecture ? "" : "data(label)",
          "text-background-color": token("--panel", "#141c27"),
          "text-background-opacity": architecture ? 0 : 0.92,
          "text-background-padding": "3px",
          "text-halign": "center",
          "text-margin-y": architecture ? 0 : 15,
          "text-valign": "center",
          "text-wrap": architecture ? "wrap" : "ellipsis",
          "text-max-width": `${Math.max(
            48,
            architecture ? leafSize.width - 12 : Math.round(150 * props.spacing / 100),
          )}px`,
          height: architecture ? `${leafSize.height}px` : "22px",
          padding: "0px",
          shape: architecture ? "roundrectangle" : "ellipse",
          width: architecture ? `${leafSize.width}px` : "22px",
        },
      },
      {
        selector: 'node[presentationRole = "group"]',
        style: {
          shape: "round-rectangle",
          height: architecture ? `${leafSize.height}px` : "28px",
          width: architecture ? `${leafSize.width}px` : "28px",
        },
      },
      {
        selector: 'node[kind = "file"]',
        style: {
          shape: architecture ? "roundrectangle" : "rectangle",
          height: architecture ? `${leafSize.height}px` : "24px",
          width: architecture ? `${leafSize.width}px` : "20px",
        },
      },
      {
        selector: 'node[kind = "symbol"]',
        style: {
          shape: architecture ? "roundrectangle" : "ellipse",
        },
      },
      {
        selector: 'node[kind = "builtin"]',
        style: {
          shape: architecture ? "roundrectangle" : "diamond",
        },
      },
      {
        selector: 'node[kind = "unresolved"]',
        style: {
          shape: architecture ? "roundrectangle" : "triangle",
        },
      },
      {
        selector: ":parent",
        style: {
          "background-image": "none",
          "background-color": (element) => colors[element.data("kind")] || "#94a3b8",
          "background-opacity": 0.08,
          "border-color": token("--line-strong", "#43566c"),
          "border-style": "dashed",
          "font-size": "9px",
          label: "data(label)",
          "padding": "18px",
          shape: "roundrectangle",
          "text-halign": "left",
          "text-margin-x": 8,
          "text-margin-y": 8,
          "text-valign": "top",
        },
      },
      {
        selector: "node[presentationHidden = 1]",
        style: {
          "border-style": "dashed",
          opacity: 0.22,
        },
      },
      {
        selector: "node[metricPresent = 1]",
        style: {
          "background-color": (element) => metricColor(element.data("metricRatio")),
          "border-color": token("--metric-edge", "#d7f3fa"),
          "border-width": "2px",
        },
      },
      {
        selector: "node[metricPresent = 1]:parent",
        style: {
          "background-opacity": 0.2,
          "border-style": "solid",
        },
      },
      {
        selector: "edge",
        style: {
          label: architecture ? "data(label)" : "",
          color: token("--muted", "#98a7b8"),
          "curve-style": "bezier",
          "font-family": "IBM Plex Mono, ui-monospace, monospace",
          "font-size": "8px",
          "line-color": token("--line-strong", "#43566c"),
          "min-zoomed-font-size": "8px",
          "target-arrow-color": token("--line-strong", "#43566c"),
          "target-arrow-shape": "triangle",
          "text-background-color": token("--canvas", "#0f1621"),
          "text-background-opacity": architecture ? 0.9 : 0,
          "text-background-padding": "2px",
          width: "1.3px",
          "arrow-scale": 0.7,
        },
      },
      {
        selector: "edge[presentationHidden = 1]",
        style: {
          opacity: hiddenEdgeOpacity,
          "text-opacity": hiddenEdgeOpacity,
        },
      },
      {
        selector: 'edge[classification = "unresolved"]',
        style: {
          "line-style": "dashed",
          "line-color": token("--danger", "#e46f6f"),
          "target-arrow-color": token("--danger", "#e46f6f"),
        },
      },
      {
        selector: 'node[verificationStatus = "pass"]',
        style: {
          "border-color": token("--success", "#49b979"),
        },
      },
      {
        selector: 'edge[verificationStatus = "pass"]',
        style: {
          "line-color": token("--success", "#49b979"),
          "target-arrow-color": token("--success", "#49b979"),
        },
      },
      {
        selector: 'node[verificationStatus = "unknown"]',
        style: {
          "border-color": token("--warning", "#e2ae52"),
          "border-style": "dashed",
        },
      },
      {
        selector: 'edge[verificationStatus = "unknown"]',
        style: {
          "line-style": "dashed",
          "line-color": token("--warning", "#e2ae52"),
          "target-arrow-color": token("--warning", "#e2ae52"),
        },
      },
      {
        selector: 'node[verificationStatus = "fail"]',
        style: {
          "border-color": token("--danger", "#e46f6f"),
          "border-width": "4px",
        },
      },
      {
        selector: 'edge[verificationStatus = "fail"]',
        style: {
          "line-color": token("--danger", "#e46f6f"),
          "target-arrow-color": token("--danger", "#e46f6f"),
          width: "3px",
        },
      },
      {
        selector: ":selected",
        style: { "border-color": token("--text", "#e6edf5"), "border-width": "4px" },
      },
      {
        selector: "node.focus-dimmed",
        style: {
          opacity: 0.3,
          "text-opacity": 0.3,
        },
      },
      {
        selector: "edge.focus-dimmed",
        style: {
          opacity: 0.25,
        },
      },
    ],
  });
  if (viewport) {
    cy.zoom(viewport.zoom);
    cy.pan(viewport.pan);
  }
  cy.on("tap", "node, edge", (event: EventObject) => {
    const id = event.target.id();
    applySelectionFocus(id);
    emit("select", id);
    if (!event.target.isNode()) return;
    const now = performance.now();
    const previous = lastNodeTap;
    if (previous !== null && previous.id === id && now - previous.at < 350) {
      lastNodeTap = null;
      emit("activate", id);
    } else {
      lastNodeTap = { at: now, id };
    }
  });
  cy.on("tap", (event: EventObject) => {
    if (event.target === cy) {
      applySelectionFocus(null);
      emit("select", null);
    }
  });
  cy.on("mouseover mousemove", "node, edge", (event: EventObject) => {
    if (event.target.isNode()) {
      if (actionHideTimer) window.clearTimeout(actionHideTimer);
      hoveredNodeId.value = event.target.id();
      updateSelectedPosition();
    }
    showHover(event);
  });
  cy.on("mouseout", "node, edge", (event: EventObject) => {
    hover.value = null;
    if (event.target.isNode()) scheduleActionHide();
  });
  cy.on("pan zoom", () => {
    hover.value = null;
    updateSelectedPosition();
  });
  cy.on("zoom", () => emit("zoom", Math.round((cy?.zoom() || 1) * 100)));
  void layout(viewport);
}

function hoverPosition(event: EventObject): { x: number; y: number } {
  const point = event.renderedPosition || { x: 0, y: 0 };
  const width = container.value?.clientWidth || 0;
  const height = container.value?.clientHeight || 0;
  return {
    x: Math.max(12, Math.min(Math.max(12, width - 292), point.x + 14)),
    y: Math.max(12, Math.min(Math.max(12, height - 156), point.y + 14)),
  };
}

function showHover(event: EventObject) {
  if (!cy) return;
  const id = event.target.id();
  const position = hoverPosition(event);
  if (event.target.isNode()) {
    const node = props.graph.nodes.find((candidate) => candidate.id === id);
    if (!node) return;
    const incoming = event.target.incomers("edge").length;
    const outgoing = event.target.outgoers("edge").length;
    const details: string[] = [`${incoming} incoming · ${outgoing} outgoing`];
    const documentation = documentationPreview(node.attributes.documentation);
    const files = Number(node.attributes.files || 0);
    const symbols = Number(node.attributes.symbols || 0);
    if (files || symbols) details.push(`${files} files · ${symbols} symbols`);
    const external = Number(node.attributes.external_relations || 0);
    if (external) details.push(`${external} external relations`);
    const findings = Number(node.attributes.verification_findings || 0);
    const status = String(node.attributes.verification_status || "");
    if (status || findings) {
      details.push(`${status || "verification"} · ${findings} findings`);
    }
    const metricValue = node.attributes.metric_value;
    if (typeof metricValue === "number") {
      const population = Number(node.attributes.metric_population || 1);
      details.push(
        `${String(node.attributes.metric_name || "metric")}: ${metricValue}`
        + `${population > 1 ? ` · max across ${population} values` : ""}`,
      );
    }
    hover.value = {
      documentation,
      details,
      eyebrow: node.kind,
      subtitle: node.identity === node.label ? "" : node.identity,
      title: node.label,
      ...position,
    };
    return;
  }
  const edge = props.graph.edges.find((candidate) => candidate.id === id);
  if (!edge) return;
  const presentation = edgePresentation(props.graph, edge);
  const namedRelations = edge.names.length + edge.omitted_names;
  const details = [
    `${edge.classification} evidence`,
    `${namedRelations} referenced name${namedRelations === 1 ? "" : "s"}`
      + `${edge.omitted_names ? ` · ${edge.omitted_names} omitted` : ""}`,
  ];
  const findings = Number(edge.attributes?.verification_findings || 0);
  const status = String(edge.attributes?.verification_status || "");
  if (status || findings) {
    details.push(`${status || "verification"} · ${findings} findings`);
  }
  hover.value = {
    documentation: "",
    details,
    eyebrow: presentation.eyebrow,
    subtitle: edge.names.length
      ? `${presentation.subtitle} · ${edge.names.join(", ")}`
      : presentation.subtitle,
    title: presentation.title,
    ...position,
  };
}

function flushWheel() {
  wheelFrame = 0;
  if (!cy || wheelDelta === 0) return;
  const delta = Math.max(-140, Math.min(140, wheelDelta));
  wheelDelta = 0;
  cy.zoom({
    level: Math.max(
      cy.minZoom(),
      Math.min(cy.maxZoom(), cy.zoom() * Math.exp(-delta * 0.0025)),
    ),
    renderedPosition: wheelPoint,
  });
}

function wheel(event: WheelEvent) {
  if (!cy || !container.value) return;
  event.preventDefault();
  event.stopImmediatePropagation();
  const scale = event.deltaMode === WheelEvent.DOM_DELTA_LINE
    ? 32
    : event.deltaMode === WheelEvent.DOM_DELTA_PAGE
      ? container.value.clientHeight
      : 1;
  wheelDelta += event.deltaY * scale;
  const bounds = container.value.getBoundingClientRect();
  wheelPoint = {
    x: event.clientX - bounds.left,
    y: event.clientY - bounds.top,
  };
  if (!wheelFrame) wheelFrame = requestAnimationFrame(flushWheel);
}

function startMiddlePan(event: MouseEvent) {
  if (!cy || event.button !== 1) return;
  event.preventDefault();
  event.stopImmediatePropagation();
  middlePanPoint = { x: event.clientX, y: event.clientY };
  middlePanning.value = true;
  hover.value = null;
}

function moveMiddlePan(event: MouseEvent) {
  if (!cy || !middlePanPoint) return;
  event.preventDefault();
  const next = { x: event.clientX, y: event.clientY };
  cy.panBy({
    x: next.x - middlePanPoint.x,
    y: next.y - middlePanPoint.y,
  });
  middlePanPoint = next;
}

function endMiddlePan(event: MouseEvent) {
  if (!middlePanPoint || (event.button !== 1 && (event.buttons & 4) !== 0)) return;
  event.preventDefault();
  middlePanPoint = null;
  middlePanning.value = false;
}

function preventMiddleAuxClick(event: MouseEvent) {
  if (event.button === 1) event.preventDefault();
}

function fit() {
  cy?.fit(undefined, fitPadding());
  if (cy && cy.zoom() > 1.25) {
    cy.zoom({
      level: 1.25,
      renderedPosition: { x: cy.width() / 2, y: cy.height() / 2 },
    });
  }
}

function zoom(multiplier: number) {
  if (!cy) return;
  cy.zoom({
    level: Math.max(cy.minZoom(), Math.min(cy.maxZoom(), cy.zoom() * multiplier)),
    renderedPosition: { x: cy.width() / 2, y: cy.height() / 2 },
  });
}

function setZoom(percent: number) {
  if (!cy) return;
  cy.zoom({
    level: Math.max(cy.minZoom(), Math.min(cy.maxZoom(), percent / 100)),
    renderedPosition: { x: cy.width() / 2, y: cy.height() / 2 },
  });
}

function captureViewport(): { pan: { x: number; y: number }; zoom: number } | null {
  return cy ? { pan: { ...cy.pan() }, zoom: cy.zoom() } : null;
}

function restoreViewport(viewport: { pan: { x: number; y: number }; zoom: number }): void {
  if (!cy) return;
  cy.zoom(viewport.zoom);
  cy.pan(viewport.pan);
  updateSelectedPosition();
}

function removeSelected() {
  if (actionNode.value && !actionHidden.value) {
    emit("hide", actionNode.value.id);
  }
}

function minimizeSelected() {
  if (actionNode.value && actionActivation.value && actionExpanded.value) {
    emit("minimize", actionNode.value.id);
  }
}

function maximizeSelected() {
  if (actionNode.value && (actionHidden.value || actionActivation.value)) {
    emit("maximize", actionNode.value.id);
  }
}

function keepActionsVisible() {
  if (actionHideTimer) window.clearTimeout(actionHideTimer);
}

function scheduleActionHide() {
  keepActionsVisible();
  actionHideTimer = window.setTimeout(() => {
    hoveredNodeId.value = null;
    updateSelectedPosition();
  }, 180);
}

function selectNode(offset: number) {
  if (!cy) return;
  const nodes = cy.nodes().filter((node) => !node.isParent()).toArray();
  if (!nodes.length) return;
  const current = nodes.findIndex((node) => node.selected());
  const index = (current + offset + nodes.length) % nodes.length;
  const next = nodes[index];
  cy.elements().unselect();
  next.select();
  emit("select", next.id());
}

function keyboard(event: KeyboardEvent) {
  if (event.key === "ArrowRight" || event.key === "ArrowDown") {
    event.preventDefault();
    selectNode(1);
  } else if (event.key === "ArrowLeft" || event.key === "ArrowUp") {
    event.preventDefault();
    selectNode(-1);
  } else if (event.key === "Escape") {
    cy?.elements().unselect();
    applySelectionFocus(null);
    emit("select", null);
  } else if (event.key === "+" || event.key === "=") {
    event.preventDefault();
    zoom(1.2);
  } else if (event.key === "-") {
    event.preventDefault();
    zoom(1 / 1.2);
  } else if (event.key === "0") {
    event.preventDefault();
    fit();
  } else if (event.key === "Enter") {
    const selected = cy?.nodes(":selected").first();
    if (selected?.length) {
      event.preventDefault();
      emit("activate", selected.id());
    }
  }
}

defineExpose({
  captureViewport,
  fit,
  restoreViewport,
  setZoom,
  zoomIn: () => zoom(1.2),
  zoomOut: () => zoom(1 / 1.2),
});

watch(
  [
    () => props.graph,
    () => props.direction,
    () => props.layoutMode,
    () => props.notation,
    () => props.spacing,
    () => props.theme,
  ],
  render,
  { deep: false },
);
watch(() => props.selectedId, (id) => {
  if (!cy) return;
  cy.elements().unselect();
  if (!id) {
    applySelectionFocus(null);
    updateSelectedPosition();
    if (props.layoutMode === "radial") void layout(preservedViewport());
    return;
  }
  const element = cy.getElementById(id);
  if (element.length) {
    element.select();
  }
  applySelectionFocus(id);
  updateSelectedPosition();
  if (props.layoutMode === "radial") void layout(preservedViewport());
});
onMounted(() => {
  container.value?.addEventListener("wheel", wheel, {
    capture: true,
    passive: false,
  });
  container.value?.addEventListener("mousedown", startMiddlePan, {
    capture: true,
  });
  container.value?.addEventListener("auxclick", preventMiddleAuxClick, {
    capture: true,
  });
  window.addEventListener("mousemove", moveMiddlePan, { capture: true });
  window.addEventListener("mouseup", endMiddlePan, { capture: true });
  render();
});
onBeforeUnmount(() => {
  emit("layoutEnd");
  if (actionHideTimer) window.clearTimeout(actionHideTimer);
  if (wheelFrame) cancelAnimationFrame(wheelFrame);
  container.value?.removeEventListener("wheel", wheel, { capture: true });
  container.value?.removeEventListener("mousedown", startMiddlePan, {
    capture: true,
  });
  container.value?.removeEventListener("auxclick", preventMiddleAuxClick, {
    capture: true,
  });
  window.removeEventListener("mousemove", moveMiddlePan, { capture: true });
  window.removeEventListener("mouseup", endMiddlePan, { capture: true });
  elk.terminateWorker();
  cy?.destroy();
});
</script>

<template>
  <div class="graph-stage">
    <div
      ref="container"
      class="graph-canvas"
      :class="{ 'middle-panning': middlePanning }"
      aria-description="Use arrow keys to move between nodes, plus and minus to zoom, zero to fit, and Escape to clear selection."
      aria-label="Architecture graph"
      :data-anchor-drift="layoutAnchorDrift === null ? undefined : layoutAnchorDrift.toFixed(3)"
      :data-has-layout="hasCompletedLayout"
      :data-layout-generation="layoutGeneration"
      :data-layout-ready="layoutReady"
      :data-layout-height="layoutExtent === null ? undefined : layoutExtent.height.toFixed(3)"
      :data-layout-width="layoutExtent === null ? undefined : layoutExtent.width.toFixed(3)"
      :data-map-view="String(graph.request.query?.view || '')"
      :data-group-by="String(graph.request.query?.grouping || '')"
      :data-layout-mode="layoutMode"
      :data-notation="notation"
      :data-spacing="spacing"
      :data-node-count="graph.summary.nodes"
      :data-hidden-node-count="graph.nodes.filter((node) => node.attributes.presentation_hidden).length"
      :data-hidden-edge-count="graph.edges.filter((edge) => edge.attributes?.presentation_hidden).length"
      :data-hidden-edge-opacity="hiddenEdgeOpacity"
      :data-edge-count="graph.summary.edges"
      :data-focus-count="focusCount"
      :data-dimmed-count="dimmedCount"
      :data-pan-x="panPosition === null ? undefined : panPosition.x.toFixed(3)"
      :data-pan-y="panPosition === null ? undefined : panPosition.y.toFixed(3)"
      :data-selected-model-x="selectedModel === null ? undefined : selectedModel.x.toFixed(3)"
      :data-selected-model-y="selectedModel === null ? undefined : selectedModel.y.toFixed(3)"
      :data-selected-id="selectedId || undefined"
      :data-selected-x="selectedRendered === null ? undefined : selectedRendered.x.toFixed(3)"
      :data-selected-y="selectedRendered === null ? undefined : selectedRendered.y.toFixed(3)"
      role="region"
      tabindex="0"
      @keydown="keyboard"
    ></div>
    <button
      v-if="canGoBack"
      class="graph-back"
      type="button"
      aria-label="Back to previous graph"
      title="Back to previous graph"
      @click="emit('back')"
    >
      <svg viewBox="0 0 16 16" aria-hidden="true">
        <path d="M9.75 3.25 5 8l4.75 4.75M5.5 8h6" />
      </svg>
      <span>Back</span>
    </button>
    <div
      v-if="actionNode && nodeActionPosition"
      class="graph-node-actions"
      :data-node-id="actionNode.id"
      :data-rendered-inset="nodeActionInset"
      :style="{
        '--node-accent': nodeAccent(actionNode),
        left: `${nodeActionPosition.x}px`,
        top: `${nodeActionPosition.y}px`,
      }"
      @mouseenter="keepActionsVisible"
      @mouseleave="scheduleActionHide"
      @mousedown.stop
    >
      <button
        type="button"
        aria-label="Remove node from view"
        title="Remove from view"
        :disabled="actionHidden"
        @click.stop="removeSelected"
      >
        <svg viewBox="0 0 12 12" aria-hidden="true">
          <path d="M2.25 2.25 9.75 9.75M9.75 2.25 2.25 9.75" />
        </svg>
      </button>
      <button
        type="button"
        aria-label="Minimize node"
        title="Minimize"
        :disabled="actionHidden || !actionActivation || !actionExpanded"
        @click.stop="minimizeSelected"
      >
        <svg viewBox="0 0 12 12" aria-hidden="true">
          <path d="M2 8.5h8" />
        </svg>
      </button>
      <button
        type="button"
        :aria-label="actionHidden ? 'Restore node' : 'Maximize node'"
        :title="actionHidden ? 'Restore' : 'Expand subtree'"
        :disabled="!actionHidden && !actionActivation"
        @click.stop="maximizeSelected"
      >
        <svg viewBox="0 0 12 12" aria-hidden="true">
          <path d="M2 5V2h3M7 2h3v3M10 7v3H7M5 10H2V7" />
        </svg>
      </button>
    </div>
    <slot />
    <aside
      v-if="hover"
      class="graph-tooltip"
      role="tooltip"
      :style="{ left: `${hover.x}px`, top: `${hover.y}px` }"
    >
      <small>{{ hover.eyebrow }}</small>
      <strong>{{ hover.title }}</strong>
      <code v-if="hover.subtitle">{{ hover.subtitle }}</code>
      <p v-if="hover.documentation">{{ hover.documentation }}</p>
      <span v-for="detail in hover.details" :key="detail">{{ detail }}</span>
    </aside>
    <p v-if="!layoutReady && !layoutError && !hasCompletedLayout" class="layout-status">Arranging graph…</p>
    <p v-if="layoutError" class="layout-error">Layout failed: {{ layoutError }}</p>
  </div>
</template>
