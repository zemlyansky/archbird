<script setup lang="ts">
import { computed } from "vue";
import type { GraphEdge, GraphNode, GraphView } from "../artifacts/model";

const props = defineProps<{
  graph: GraphView;
  selectedId: string | null;
  source: Record<string, unknown> | null;
  sourcePath: string | null;
}>();
const emit = defineEmits<{ source: [path: string] }>();

const selected = computed<GraphNode | GraphEdge | null>(() => {
  if (!props.selectedId) return null;
  return props.graph.nodes.find((node) => node.id === props.selectedId)
    || props.graph.edges.find((edge) => edge.id === props.selectedId)
    || null;
});
</script>

<template>
  <aside class="inspector">
    <template v-if="selected">
      <div class="eyebrow">{{ 'identity' in selected ? selected.kind : selected.classification }}</div>
      <h2>{{ 'identity' in selected ? selected.label : selected.kind }}</h2>
      <code>{{ 'identity' in selected ? selected.identity : `${selected.source} → ${selected.target}` }}</code>
      <button
        v-if="sourcePath"
        class="source-button"
        type="button"
        @click="emit('source', sourcePath)"
      >Open {{ sourcePath }}</button>
      <details v-if="source && source.path === sourcePath" open>
        <summary>Source · {{ source.encoding }}{{ source.truncated ? ' · truncated' : '' }}</summary>
        <pre>{{ source.text }}</pre>
      </details>
      <dl v-if="'names' in selected && selected.names.length">
        <dt>Names</dt>
        <dd>{{ selected.names.join(', ') }}</dd>
      </dl>
      <details open>
        <summary>Evidence</summary>
        <pre>{{ JSON.stringify(selected.evidence, null, 2) }}</pre>
      </details>
      <details>
        <summary>Raw projection</summary>
        <pre>{{ JSON.stringify(selected, null, 2) }}</pre>
      </details>
    </template>
    <template v-else>
      <div class="eyebrow">Evidence inspector</div>
      <h2>Select a node or edge</h2>
      <p>Every rendered relation comes from the deterministic graph-view artifact.</p>
      <dl>
        <dt>Projection</dt>
        <dd>{{ graph.request.view }}</dd>
        <dt>Nodes / edges</dt>
        <dd>{{ graph.summary.nodes }} / {{ graph.summary.edges }}</dd>
        <dt>Diagnostics</dt>
        <dd>{{ graph.diagnostics.length }}</dd>
        <dt>Omissions</dt>
        <dd>{{ graph.omissions.length }}</dd>
      </dl>
    </template>
  </aside>
</template>
