<script setup lang="ts">
import { computed } from "vue";
import type { GraphEdge, GraphNode, GraphView } from "../artifacts/model";
import { edgePresentation } from "../graph/explore";
import type { ComponentDetails } from "../graph/explore";

const props = defineProps<{
  graph: GraphView;
  fileGraph: GraphView | null;
  selectedId: string | null;
  source: Record<string, unknown> | null;
  sourcePath: string | null;
}>();
const emit = defineEmits<{
  activate: [id: string];
  close: [];
  hide: [id: string];
  source: [path: string];
}>();

const selected = computed<GraphNode | GraphEdge | null>(() => {
  if (!props.selectedId) return null;
  return props.graph.nodes.find((node) => node.id === props.selectedId)
    || props.graph.edges.find((edge) => edge.id === props.selectedId)
    || null;
});
const selectedNode = computed(() =>
  selected.value && "identity" in selected.value ? selected.value : null);
const selectedEdge = computed(() =>
  selected.value && !("identity" in selected.value) ? selected.value : null);
const nodeById = computed(() =>
  new Map(props.graph.nodes.map((node) => [node.id, node])));
const edgeEndpoints = computed(() => {
  if (!selectedEdge.value) return null;
  return {
    source: nodeById.value.get(selectedEdge.value.source)?.label || selectedEdge.value.source,
    target: nodeById.value.get(selectedEdge.value.target)?.label || selectedEdge.value.target,
  };
});
const selectedEdgePresentation = computed(() =>
  selectedEdge.value ? edgePresentation(props.graph, selectedEdge.value) : null);
const relations = computed(() => {
  if (!selectedNode.value) return { incoming: [], outgoing: [] };
  return {
    incoming: props.graph.edges.filter((edge) => edge.target === selectedNode.value?.id),
    outgoing: props.graph.edges.filter((edge) => edge.source === selectedNode.value?.id),
  };
});
const component = computed<ComponentDetails | null>(() => {
  const node = selectedNode.value;
  if (node?.kind !== "component") return null;
  const files = Array.isArray(node.attributes.member_files)
    ? node.attributes.member_files.filter(
      (value): value is string => typeof value === "string",
    )
    : [];
  return {
    description: typeof node.attributes.description === "string"
      ? node.attributes.description
      : "",
    files,
    name: node.label,
    symbolCount: Number(
      node.attributes.symbol_count || node.attributes.symbols || 0,
    ),
  };
});
const attributes = computed(() => Object.entries(selectedNode.value?.attributes || {})
  .filter(([name]) =>
    !name.startsWith("presentation_")
    && !name.startsWith("verification_")
    && !["canonical_id", "canonical_path", "member_files", "expanded"].includes(name)));
const canActivate = computed(() =>
  selectedNode.value?.attributes.presentation_role === "group"
  || selectedNode.value?.kind === "file"
  || (selectedNode.value?.kind === "symbol"
    && Number(selectedNode.value.attributes.external_relations || 0) > 0));
const verificationConstraints = computed(() => {
  const value = selectedNode.value?.attributes.verification_constraints
    || selectedEdge.value?.attributes?.verification_constraints;
  return Array.isArray(value)
    ? value.filter((id): id is string => typeof id === "string")
    : [];
});
const verificationStatus = computed(() => String(
  selectedNode.value?.attributes.verification_status
  || selectedEdge.value?.attributes?.verification_status
  || "",
));
const verificationMeaning = computed(() => ({
  fail: "At least one applicable constraint reports a violation for this item.",
  not_applicable: "No configured constraint applies to this item.",
  pass: "All applicable evaluated constraints pass for this item.",
  unknown: "At least one applicable constraint could not be decided from complete evidence.",
  waived: "An applicable violation is covered by a reviewed waiver.",
}[verificationStatus.value] || ""));
const underlyingFileRelations = computed(() => {
  const edge = selectedEdge.value;
  if (!edge) return [];
  const sourceNode = nodeById.value.get(edge.source);
  const targetNode = nodeById.value.get(edge.target);
  if (!sourceNode || !targetNode) return [];
  function paths(node: GraphNode): Set<string> | null {
    if (node.attributes.presentation_role === "group") {
      const files = node.attributes.member_files;
      return Array.isArray(files)
        ? new Set(files.filter((path): path is string => typeof path === "string"))
        : null;
    }
    if (node.kind === "file") {
      const path = typeof node.attributes.canonical_path === "string"
        ? node.attributes.canonical_path
        : node.identity;
      return new Set([path]);
    }
    return null;
  }
  const sources = paths(sourceNode);
  const targets = paths(targetNode);
  if (!sources || !targets) return [];
  const names = new Set(edge.names);
  if (!props.fileGraph) return [];
  const nodes = new Map(props.fileGraph.nodes.map((node) => [node.id, node]));
  return props.fileGraph.edges.flatMap((relation) => {
    const source = nodes.get(relation.source)?.identity;
    const target = nodes.get(relation.target)?.identity;
    if (!source || !target) return [];
    return relation.kind === edge.kind
    && sources.has(source)
    && targets.has(target)
    && (
      !names.size
      || edge.omitted_names > 0
      || relation.names.some((name) => names.has(name))
    ) ? [{
        kind: relation.kind,
        names: relation.names,
        source,
        target,
      }] : [];
  });
});

function relatedLabel(edge: GraphEdge, direction: "incoming" | "outgoing"): string {
  const id = direction === "incoming" ? edge.source : edge.target;
  return nodeById.value.get(id)?.label || id;
}
</script>

<template>
  <aside class="inspector">
    <button class="inspector-close" type="button" aria-label="Close inspector" @click="emit('close')">
      Close
    </button>
    <template v-if="selected">
      <div class="eyebrow">{{
        selectedNode
          ? selectedNode.kind === 'component'
            ? 'project component'
            : selectedNode.attributes.presentation_role === 'group'
              ? `${selectedNode.attributes.origin || 'derived'} ${selectedNode.kind} group`
              : selectedNode.kind
          : selectedEdgePresentation?.eyebrow
      }}</div>
      <h2>{{ selectedNode ? selectedNode.label : selectedEdgePresentation?.title }}</h2>
      <code>{{ selectedNode ? selectedNode.identity : selectedEdgePresentation?.subtitle }}</code>
      <p v-if="component?.description">{{ component.description }}</p>
      <p v-if="selectedEdgePresentation">{{ selectedEdgePresentation.summary }}</p>
      <button
        v-if="canActivate && selectedNode"
        class="source-button"
        type="button"
        @click="emit('activate', selectedNode.id)"
      >{{
        selectedNode.kind === 'symbol'
          ? selectedNode.attributes.relations_revealed
            ? 'Hide connections'
            : 'Show connections'
          : selectedNode.attributes.expanded
            ? 'Collapse'
            : selectedNode.attributes.presentation_role === 'group'
              ? selectedNode.kind === 'directory'
                ? selectedNode.attributes.root_group
                  ? 'Expand directory'
                  : 'Expand area'
                : 'Expand member files'
              : 'Expand symbols'
      }}</button>
      <button
        v-if="selectedNode"
        class="source-button"
        type="button"
        @click="emit('hide', selectedNode.id)"
      >Hide from view</button>
      <button
        v-if="selectedEdge"
        class="source-button"
        type="button"
        @click="emit('hide', selectedEdge.id)"
      >Hide relation</button>
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
      <dl v-if="selectedNode && attributes.length">
        <template v-for="[name, value] in attributes" :key="name">
          <dt>{{ name.replaceAll('_', ' ') }}</dt>
          <dd>{{ value }}</dd>
        </template>
        <dt>Incoming relations</dt>
        <dd>{{ relations.incoming.length }}</dd>
        <dt>Outgoing relations</dt>
        <dd>{{ relations.outgoing.length }}</dd>
      </dl>
      <dl v-if="component">
        <dt>Definition</dt>
        <dd>Project component</dd>
        <dt>Concrete member files</dt>
        <dd>{{ component.files.length }}</dd>
        <dt>Mapped symbols</dt>
        <dd>{{ component.symbolCount }}</dd>
      </dl>
      <dl v-if="verificationStatus">
        <dt>Verification</dt>
        <dd :data-status="verificationStatus">{{ verificationStatus }}</dd>
        <dt>Constraints</dt>
        <dd>{{ verificationConstraints.length }}</dd>
        <dt>Findings</dt>
        <dd>{{ selectedNode?.attributes.verification_findings || selectedEdge?.attributes?.verification_findings || 0 }}</dd>
      </dl>
      <p v-if="verificationMeaning">{{ verificationMeaning }}</p>
      <details v-if="verificationConstraints.length" open>
        <summary>Applicable constraints</summary>
        <ul class="evidence-list">
          <li v-for="id in verificationConstraints" :key="id"><code>{{ id }}</code></li>
        </ul>
      </details>
      <details v-if="component?.files.length" open>
        <summary>Member files · {{ component.files.length }}</summary>
        <ul class="evidence-list">
          <li v-for="file in component.files.slice(0, 100)" :key="file"><code>{{ file }}</code></li>
          <li v-if="component.files.length > 100">{{ component.files.length - 100 }} more files</li>
        </ul>
      </details>
      <details v-if="selectedNode && relations.outgoing.length">
        <summary>Outgoing · {{ relations.outgoing.length }}</summary>
        <ul class="evidence-list">
          <li v-for="edge in relations.outgoing" :key="edge.id">
            <strong>{{ edge.kind }}</strong> → {{ relatedLabel(edge, 'outgoing') }}
            <small>{{ edge.classification }}{{ edge.names.length ? ` · ${edge.names.join(', ')}` : '' }}</small>
          </li>
        </ul>
      </details>
      <details v-if="selectedNode && relations.incoming.length">
        <summary>Incoming · {{ relations.incoming.length }}</summary>
        <ul class="evidence-list">
          <li v-for="edge in relations.incoming" :key="edge.id">
            <strong>{{ edge.kind }}</strong> ← {{ relatedLabel(edge, 'incoming') }}
            <small>{{ edge.classification }}{{ edge.names.length ? ` · ${edge.names.join(', ')}` : '' }}</small>
          </li>
        </ul>
      </details>
      <dl v-if="selectedEdge">
        <dt>Evidence class</dt>
        <dd>{{ selectedEdge.classification }}</dd>
        <dt>Referenced names</dt>
        <dd>{{ selectedEdge.names.length + selectedEdge.omitted_names }}</dd>
      </dl>
      <details v-if="selectedEdge?.names.length" open>
        <summary>Referenced names · {{ selectedEdge.names.length }} shown</summary>
        <ul class="evidence-list">
          <li v-for="name in selectedEdge.names" :key="name"><code>{{ name }}</code></li>
          <li v-if="selectedEdge.omitted_names">{{ selectedEdge.omitted_names }} more omitted by the view budget</li>
        </ul>
      </details>
      <details v-if="underlyingFileRelations.length" open>
        <summary>Underlying file relations · {{ underlyingFileRelations.length }}</summary>
        <ul class="evidence-list">
          <li
            v-for="relation in underlyingFileRelations.slice(0, 100)"
            :key="`${relation.kind}:${relation.source}:${relation.target}:${relation.names.join(',')}`"
          >
            <code>{{ relation.source }}</code> → <code>{{ relation.target }}</code>
            <small>{{ relation.kind }}{{ relation.names.length ? ` · ${relation.names.join(', ')}` : '' }}</small>
          </li>
          <li v-if="underlyingFileRelations.length > 100">
            {{ underlyingFileRelations.length - 100 }} more file relations
          </li>
        </ul>
      </details>
      <details v-if="selected.evidence.length" open>
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
