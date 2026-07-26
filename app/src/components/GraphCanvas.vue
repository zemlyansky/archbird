<script setup lang="ts">
import cytoscape, {
  type Core,
  type EventObject,
} from "cytoscape";
import ELK from "elkjs/lib/elk-api.js";
import ELKWorker from "elkjs/lib/elk-worker.min.js?worker";
import { onBeforeUnmount, onMounted, ref, watch } from "vue";
import type { GraphView } from "../artifacts/model";
import { edgePresentation } from "../graph/explore";
import {
  elkHierarchy,
  elkLayoutOptions,
  elkPositions,
  type ElkNode,
} from "../graph/layout";

const props = defineProps<{
  direction: "BT" | "LR" | "RL" | "TB";
  graph: GraphView;
  selectedId: string | null;
  spacing: number;
  theme: "dark" | "light";
}>();
const emit = defineEmits<{
  activate: [id: string];
  layoutEnd: [];
  layoutStart: [];
  select: [id: string | null];
  zoom: [percent: number];
}>();
const container = ref<HTMLElement | null>(null);
let cy: Core | null = null;
let requestId = 0;
let lastNodeTap: { at: number; id: string } | null = null;
const layoutError = ref("");
const layoutReady = ref(false);
const hasCompletedLayout = ref(false);
const layoutAnchorDrift = ref<number | null>(null);
const focusCount = ref(0);
const dimmedCount = ref(0);
const layoutExtent = ref<{ height: number; width: number } | null>(null);
const panPosition = ref<{ x: number; y: number } | null>(null);
const selectedModel = ref<{ x: number; y: number } | null>(null);
const selectedRendered = ref<{ x: number; y: number } | null>(null);
const middlePanning = ref(false);
const hover = ref<{
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
const elk = new ELK({
  workerFactory: () => new ELKWorker(),
});

const colors: Record<string, string> = {
  component: "#7dd3fc",
  directory: "#fdba74",
  layer: "#f9a8d4",
  language: "#67e8f9",
  file: "#c4b5fd",
  symbol: "#86efac",
  package: "#facc15",
  build: "#fb7185",
  builtin: "#fcd34d",
  external: "#94a3b8",
  unresolved: "#fca5a5",
  unknown: "#fca5a5",
};

interface PreservedViewport {
  anchorId: string;
  anchorModel: { x: number; y: number };
  anchorRendered: { x: number; y: number };
  pan: { x: number; y: number };
  positions: Record<string, { x: number; y: number }>;
  zoom: number;
}

function elements(viewport: PreservedViewport | null) {
  const parents = new Set(
    props.graph.nodes.flatMap((node) => node.parent ? [node.parent] : []),
  );
  return [
    ...props.graph.nodes.map((node) => {
      const initial = viewport?.positions[node.id] || viewport?.anchorModel;
      return {
        data: {
          id: node.id,
          label: node.label,
          kind: node.kind,
          identity: node.identity,
          expanded: Boolean(node.attributes.expanded),
          presentationRole: String(node.attributes.presentation_role || ""),
          verificationFindings: Number(node.attributes.verification_findings || 0),
          verificationStatus: String(node.attributes.verification_status || ""),
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
    return;
  }
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
    const result = await elk.layout({
      id: "root",
      layoutOptions: elkLayoutOptions(props.direction, props.spacing),
      children: elkHierarchy(props.graph.nodes, props.spacing),
      edges: props.graph.edges.map((edge) => ({
        id: edge.id,
        sources: [edge.source],
        targets: [edge.target],
      })),
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
        {
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
  const tokens = getComputedStyle(document.documentElement);
  const token = (name: string, fallback: string) =>
    tokens.getPropertyValue(name).trim() || fallback;
  cy?.destroy();
  cy = cytoscape({
    container: container.value,
    elements: elements(viewport),
    layout: { name: "preset" },
    minZoom: 0.05,
    maxZoom: 3,
    style: [
      {
        selector: "node",
        style: {
          "background-color": (element) => colors[element.data("kind")] || "#94a3b8",
          "border-color": token("--bg", "#0b0f14"),
          "border-width": "1.5px",
          color: token("--text", "#e6edf5"),
          "font-family": "IBM Plex Mono, ui-monospace, monospace",
          "font-size": "10px",
          label: "data(label)",
          "text-background-color": token("--panel", "#141c27"),
          "text-background-opacity": 0.92,
          "text-background-padding": "3px",
          "text-margin-y": 15,
          "text-wrap": "ellipsis",
          "text-max-width": `${Math.max(
            48,
            Math.round(150 * props.spacing / 100),
          )}px`,
          height: "22px",
          width: "22px",
        },
      },
      {
        selector: 'node[presentationRole = "group"]',
        style: {
          shape: "round-rectangle",
          height: "28px",
          width: "28px",
        },
      },
      {
        selector: 'node[kind = "file"]',
        style: {
          shape: "rectangle",
          height: "24px",
          width: "20px",
        },
      },
      {
        selector: 'node[kind = "symbol"]',
        style: {
          shape: "ellipse",
        },
      },
      {
        selector: 'node[kind = "builtin"]',
        style: {
          shape: "diamond",
        },
      },
      {
        selector: 'node[kind = "unresolved"]',
        style: {
          shape: "triangle",
        },
      },
      {
        selector: ":parent",
        style: {
          "background-color": (element) => colors[element.data("kind")] || "#94a3b8",
          "background-opacity": 0.08,
          "border-color": token("--line-strong", "#43566c"),
          "border-style": "dashed",
          "font-size": "9px",
          "padding": "18px",
          shape: "roundrectangle",
          "text-halign": "left",
          "text-margin-x": 8,
          "text-margin-y": 8,
          "text-valign": "top",
        },
      },
      {
        selector: "edge",
        style: {
          "curve-style": "bezier",
          "line-color": token("--line-strong", "#43566c"),
          "target-arrow-color": token("--line-strong", "#43566c"),
          "target-arrow-shape": "triangle",
          width: "1.3px",
          "arrow-scale": 0.7,
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
  cy.on("mouseover mousemove", "node, edge", showHover);
  cy.on("mouseout", "node, edge", () => {
    hover.value = null;
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
    hover.value = {
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
  fit,
  setZoom,
  zoomIn: () => zoom(1.2),
  zoomOut: () => zoom(1 / 1.2),
});

watch(
  [() => props.graph, () => props.direction, () => props.spacing, () => props.theme],
  render,
  { deep: false },
);
watch(() => props.selectedId, (id) => {
  if (!cy) return;
  cy.elements().unselect();
  if (!id) {
    applySelectionFocus(null);
    updateSelectedPosition();
    return;
  }
  const element = cy.getElementById(id);
  if (element.length) {
    element.select();
  }
  applySelectionFocus(id);
  updateSelectedPosition();
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
      :data-layout-ready="layoutReady"
      :data-layout-height="layoutExtent === null ? undefined : layoutExtent.height.toFixed(3)"
      :data-layout-width="layoutExtent === null ? undefined : layoutExtent.width.toFixed(3)"
      :data-map-view="String(graph.request.query?.view || '')"
      :data-group-by="String(graph.request.query?.grouping || '')"
      :data-spacing="spacing"
      :data-node-count="graph.summary.nodes"
      :data-edge-count="graph.summary.edges"
      :data-focus-count="focusCount"
      :data-dimmed-count="dimmedCount"
      :data-pan-x="panPosition === null ? undefined : panPosition.x.toFixed(3)"
      :data-pan-y="panPosition === null ? undefined : panPosition.y.toFixed(3)"
      :data-selected-model-x="selectedModel === null ? undefined : selectedModel.x.toFixed(3)"
      :data-selected-model-y="selectedModel === null ? undefined : selectedModel.y.toFixed(3)"
      :data-selected-x="selectedRendered === null ? undefined : selectedRendered.x.toFixed(3)"
      :data-selected-y="selectedRendered === null ? undefined : selectedRendered.y.toFixed(3)"
      role="region"
      tabindex="0"
      @keydown="keyboard"
    ></div>
    <aside
      v-if="hover"
      class="graph-tooltip"
      role="tooltip"
      :style="{ left: `${hover.x}px`, top: `${hover.y}px` }"
    >
      <small>{{ hover.eyebrow }}</small>
      <strong>{{ hover.title }}</strong>
      <code v-if="hover.subtitle">{{ hover.subtitle }}</code>
      <span v-for="detail in hover.details" :key="detail">{{ detail }}</span>
    </aside>
    <p v-if="!layoutReady && !layoutError && !hasCompletedLayout" class="layout-status">Arranging graph…</p>
    <p v-if="layoutError" class="layout-error">Layout failed: {{ layoutError }}</p>
  </div>
</template>
