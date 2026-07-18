<script setup lang="ts">
import cytoscape, { type Core, type EventObject } from "cytoscape";
import ELK from "elkjs/lib/elk-api.js";
import ELKWorker from "elkjs/lib/elk-worker.min.js?worker";
import { onBeforeUnmount, onMounted, ref, watch } from "vue";
import type { GraphView } from "../artifacts/model";

const props = defineProps<{
  graph: GraphView;
  selectedId: string | null;
}>();
const emit = defineEmits<{ select: [id: string | null] }>();
const container = ref<HTMLElement | null>(null);
let cy: Core | null = null;
let requestId = 0;
const layoutError = ref("");
const elk = new ELK({
  workerFactory: () => new ELKWorker(),
});

const colors: Record<string, string> = {
  component: "#7dd3fc",
  file: "#c4b5fd",
  symbol: "#86efac",
  builtin: "#fcd34d",
  unresolved: "#fca5a5",
};

function elements() {
  return [
    ...props.graph.nodes.map((node) => ({
      data: {
        id: node.id,
        label: node.label,
        kind: node.kind,
        identity: node.identity,
        ...(node.parent ? { parent: node.parent } : {}),
      },
    })),
    ...props.graph.edges.map((edge) => ({
      data: {
        id: edge.id,
        source: edge.source,
        target: edge.target,
        label: edge.kind,
        classification: edge.classification,
      },
    })),
  ];
}

async function layout() {
  if (!cy) return;
  const id = ++requestId;
  layoutError.value = "";
  try {
    const result = await elk.layout({
      id: "root",
      layoutOptions: {
        "elk.algorithm": "layered",
        "elk.direction": "RIGHT",
        "elk.edgeRouting": "ORTHOGONAL",
        "elk.layered.spacing.nodeNodeBetweenLayers": "90",
        "elk.spacing.nodeNode": "44",
      },
      children: props.graph.nodes.map((node) => ({
        id: node.id,
        width: 170,
        height: 52,
      })),
      edges: props.graph.edges.map((edge) => ({
        id: edge.id,
        sources: [edge.source],
        targets: [edge.target],
      })),
    }) as { children?: Array<{ id: string; x?: number; y?: number }> };
    if (!cy || id !== requestId) return;
    const positions = Object.fromEntries(
      (result.children || []).map((node) => [
        node.id,
        { x: node.x || 0, y: node.y || 0 },
      ]),
    );
    cy.nodes().positions((node) => positions[node.id()] || { x: 0, y: 0 });
    cy.fit(undefined, 42);
  } catch (error) {
    if (id === requestId) layoutError.value = (error as Error).message;
  }
}

function render() {
  if (!container.value) return;
  cy?.destroy();
  cy = cytoscape({
    container: container.value,
    elements: elements(),
    layout: { name: "preset" },
    minZoom: 0.05,
    maxZoom: 3,
    style: [
      {
        selector: "node",
        style: {
          "background-color": (element) => colors[element.data("kind")] || "#94a3b8",
          "border-color": "#0f172a",
          "border-width": "1.5px",
          color: "#e2e8f0",
          "font-family": "IBM Plex Mono, ui-monospace, monospace",
          "font-size": "10px",
          label: "data(label)",
          "text-background-color": "#0f172a",
          "text-background-opacity": 0.84,
          "text-background-padding": "3px",
          "text-margin-y": 15,
          "text-wrap": "ellipsis",
          "text-max-width": "150px",
          height: "22px",
          width: "22px",
        },
      },
      {
        selector: ":parent",
        style: {
          "background-opacity": 0.06,
          "border-color": "#64748b",
          "border-style": "dashed",
          "padding": "18px",
          shape: "roundrectangle",
        },
      },
      {
        selector: "edge",
        style: {
          "curve-style": "bezier",
          "line-color": "#475569",
          "target-arrow-color": "#475569",
          "target-arrow-shape": "triangle",
          width: "1.3px",
          "arrow-scale": 0.7,
        },
      },
      {
        selector: 'edge[classification = "unresolved"]',
        style: { "line-style": "dashed", "line-color": "#ef4444" },
      },
      {
        selector: ":selected",
        style: { "border-color": "#f8fafc", "border-width": "4px" },
      },
    ],
  });
  cy.on("tap", "node, edge", (event: EventObject) => emit("select", event.target.id()));
  cy.on("tap", (event: EventObject) => {
    if (event.target === cy) emit("select", null);
  });
  void layout();
}

watch(() => props.graph, render, { deep: false });
watch(() => props.selectedId, (id) => {
  if (!cy) return;
  cy.elements().unselect();
  if (!id) return;
  const element = cy.getElementById(id);
  if (element.length) {
    element.select();
    cy.animate({ center: { eles: element }, duration: 180 });
  }
});
onMounted(render);
onBeforeUnmount(() => {
  elk.terminateWorker();
  cy?.destroy();
});
</script>

<template>
  <div class="graph-stage">
    <div ref="container" class="graph-canvas" aria-label="Architecture graph"></div>
    <p v-if="layoutError" class="layout-error">Layout failed: {{ layoutError }}</p>
  </div>
</template>
