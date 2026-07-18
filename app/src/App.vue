<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import ArtifactDrop from "./components/ArtifactDrop.vue";
import GraphCanvas from "./components/GraphCanvas.vue";
import InspectorPanel from "./components/InspectorPanel.vue";
import { BrowserHost, type BrowserSource } from "./adapters/browser-host";
import type { HostEvent, SnapshotSummary } from "./adapters/protocol";
import { ServerHost } from "./adapters/server-host";
import { loadArtifact, projectArtifact } from "./adapters/saved-artifact";
import {
  parseArtifact,
  parseGraphView,
  supportedViews,
  type GraphView,
  type GraphViewName,
  type ParsedArtifact,
} from "./artifacts/model";

const artifact = ref<ParsedArtifact | null>(null);
const graph = ref<GraphView | null>(null);
const graphBytes = ref<Uint8Array | null>(null);
const selectedId = ref<string | null>(null);
const search = ref("");
const error = ref("");
const busy = ref(false);
const currentView = ref<GraphViewName>("components");
const liveSource = ref(false);
const progress = ref<Record<string, unknown> | null>(null);
const snapshots = ref<SnapshotSummary[]>([]);
const source = ref<Record<string, unknown> | null>(null);
const hostMode = ref<"browser" | "saved" | "server">("saved");
let liveHost: BrowserHost | ServerHost | null = null;
let unsubscribe: (() => void) | null = null;
let followingLive = true;

const availableViews = computed(() => artifact.value ? supportedViews(artifact.value.artifact) : []);
const canCancel = computed(() => hostMode.value === "browser");
const searchResults = computed(() => {
  const needle = search.value.trim().toLocaleLowerCase();
  if (!graph.value || !needle) return [];
  return graph.value.nodes.filter((node) =>
    `${node.label}\n${node.identity}\n${node.kind}`.toLocaleLowerCase().includes(needle),
  ).slice(0, 30);
});

const sourcePath = computed(() => {
  if (!graph.value || !selectedId.value) return null;
  const selected = graph.value.nodes.find((node) => node.id === selectedId.value);
  if (!selected) return null;
  let file = selected;
  if (selected.kind === "symbol" && selected.parent) {
    file = graph.value.nodes.find((node) => node.id === selected.parent) || selected;
  }
  return file.kind === "file" ? file.identity : null;
});

function hostEvent(event: HostEvent) {
  if (event.type === "progress" || event.type === "scan-started") {
    busy.value = true;
    progress.value = event.payload;
  }
  if (event.type === "candidate-failed") {
    busy.value = false;
    progress.value = event.payload;
    error.value = String(event.payload.message || "candidate analysis failed");
  }
  if (event.type === "snapshot-ready" && liveHost instanceof ServerHost && followingLive) {
    void refreshServer(liveHost);
  }
}

function ensureBrowserHost(): BrowserHost {
  if (!(liveHost instanceof BrowserHost)) {
    unsubscribe?.();
    void liveHost?.dispose();
    liveHost = new BrowserHost();
    unsubscribe = liveHost.subscribe(hostEvent);
  }
  hostMode.value = "browser";
  followingLive = true;
  return liveHost;
}

function graphFromBytes(bytes: Uint8Array): GraphView {
  const document = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  return parseGraphView(document);
}

async function liveProjection(host: BrowserHost | ServerHost, view: GraphViewName) {
  const payload = await host.view(view);
  const bytes = new Uint8Array(payload.bytes);
  graph.value = graphFromBytes(bytes);
  graphBytes.value = bytes;
}

async function open(file: File) {
  busy.value = true;
  error.value = "";
  selectedId.value = null;
  try {
    const loaded = await loadArtifact(file);
    liveSource.value = false;
    followingLive = false;
    hostMode.value = "saved";
    progress.value = null;
    source.value = null;
    artifact.value = loaded;
    const views = supportedViews(loaded.artifact);
    if (views.length) currentView.value = views[0];
    const projection = await projectArtifact(loaded, currentView.value);
    graph.value = projection?.graph || null;
    graphBytes.value = projection?.bytes || null;
  } catch (cause) {
    artifact.value = null;
    graph.value = null;
    graphBytes.value = null;
    error.value = (cause as Error).message;
  } finally {
    busy.value = false;
  }
}

async function openLive(input: BrowserSource) {
  busy.value = true;
  error.value = "";
  selectedId.value = null;
  source.value = null;
  progress.value = { completed: 0, phase: "starting", total: 1 };
  try {
    const host = ensureBrowserHost();
    const payload = await host.load(input);
    const bytes = new Uint8Array(payload.bytes);
    const loaded = parseArtifact(bytes, `${payload.project}.archbird.json`);
    artifact.value = loaded;
    liveSource.value = true;
    followingLive = true;
    currentView.value = "components";
    await liveProjection(host, currentView.value);
    snapshots.value = await host.snapshots();
    progress.value = null;
  } catch (cause) {
    error.value = (cause as Error).message;
  } finally {
    busy.value = false;
  }
}

async function openSnapshot(generation: string) {
  if (!liveHost) return;
  busy.value = true;
  error.value = "";
  selectedId.value = null;
  source.value = null;
  try {
    followingLive = false;
    const payload = await liveHost.openSnapshot(generation);
    artifact.value = parseArtifact(
      new Uint8Array(payload.bytes),
      `${payload.project}-${generation.slice(0, 12)}.archbird.json`,
    );
    liveSource.value = true;
    currentView.value = "components";
    await liveProjection(liveHost, currentView.value);
  } catch (cause) {
    error.value = (cause as Error).message;
  } finally {
    busy.value = false;
  }
}

async function changeView(view: GraphViewName) {
  if (!artifact.value || currentView.value === view) return;
  busy.value = true;
  error.value = "";
  try {
    currentView.value = view;
    selectedId.value = null;
    source.value = null;
    if (liveSource.value && liveHost) {
      await liveProjection(liveHost, view);
    } else {
      const projection = await projectArtifact(artifact.value, view);
      graph.value = projection?.graph || null;
      graphBytes.value = projection?.bytes || null;
    }
  } catch (cause) {
    error.value = (cause as Error).message;
  } finally {
    busy.value = false;
  }
}

async function openSource(path: string) {
  if (!liveHost || !liveSource.value) return;
  error.value = "";
  try {
    source.value = await liveHost.source(path);
  } catch (cause) {
    error.value = (cause as Error).message;
  }
}

function cancelLive() {
  if (!(liveHost instanceof BrowserHost)) return;
  liveHost.cancel();
  busy.value = false;
  liveSource.value = false;
  progress.value = { phase: "canceled" };
}

async function refreshServer(host: ServerHost) {
  busy.value = true;
  try {
    const payload = await host.map();
    artifact.value = parseArtifact(
      new Uint8Array(payload.bytes),
      `${payload.project}.archbird.json`,
    );
    liveSource.value = true;
    hostMode.value = "server";
    if (!supportedViews(artifact.value.artifact).includes(currentView.value)) {
      currentView.value = "components";
    }
    await liveProjection(host, currentView.value);
    snapshots.value = await host.snapshots();
    error.value = "";
    progress.value = null;
  } catch (cause) {
    error.value = (cause as Error).message;
  } finally {
    busy.value = false;
  }
}

function saveProjection() {
  if (!graph.value || !graphBytes.value) return;
  const bytes = Uint8Array.from(graphBytes.value);
  const url = URL.createObjectURL(new Blob([bytes.buffer], { type: "application/json" }));
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = `${graph.value.project}-${graph.value.request.view}.archbird-view.json`;
  anchor.hidden = true;
  document.body.append(anchor);
  anchor.click();
  window.setTimeout(() => {
    anchor.remove();
    URL.revokeObjectURL(url);
  }, 60_000);
}

onBeforeUnmount(() => {
  unsubscribe?.();
  void liveHost?.dispose();
});

onMounted(async () => {
  const server = await ServerHost.connect();
  if (!server) return;
  if (liveHost) {
    await server.dispose();
    return;
  }
  liveHost = server;
  unsubscribe = server.subscribe(hostEvent);
  followingLive = true;
  hostMode.value = "server";
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
          <small>architecture evidence viewer</small>
        </div>
      </div>
      <ArtifactDrop
        @select="open"
        @directory="openLive({ kind: 'directory', files: $event })"
        @zip="openLive({ kind: 'zip', file: $event })"
      />
      <label class="search">
        <span>Search graph</span>
        <input v-model="search" :disabled="!graph" placeholder="symbol, file, component…" />
      </label>
    </header>

    <section v-if="!artifact && !error" class="welcome">
      <p class="eyebrow">Map → Verify → Act</p>
      <h1>The architecture, with receipts.</h1>
      <p>
        Open a canonical Archbird Map or Query, or a deterministic graph-view JSON.
        Source stays in this browser.
      </p>
      <ArtifactDrop
        @select="open"
        @directory="openLive({ kind: 'directory', files: $event })"
        @zip="openLive({ kind: 'zip', file: $event })"
      />
      <p v-if="hostMode === 'server' && progress" class="eyebrow">
        Local repository · {{ progress.phase || 'waiting' }}
      </p>
    </section>

    <section v-else class="workspace">
      <nav class="rail">
        <div class="artifact-meta">
          <span class="status" :class="{ busy }"></span>
          <div>
            <strong>{{ artifact?.document.project || artifact?.name || 'Artifact' }}</strong>
            <small>{{ artifact?.artifact }} · schema {{ artifact?.document.schema_version }} · {{ hostMode }}</small>
          </div>
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
        <button v-if="graph" class="save-view" type="button" @click="saveProjection">
          Save exact view JSON
        </button>
        <div v-if="busy && progress" class="progress-card">
          <p>{{ progress.phase || 'working' }}</p>
          <progress
            v-if="typeof progress.completed === 'number' && typeof progress.total === 'number'"
            :value="Number(progress.completed)"
            :max="Math.max(1, Number(progress.total))"
          ></progress>
          <button
            v-if="canCancel"
            class="cancel-button"
            type="button"
            @click="cancelLive"
          >Cancel Worker</button>
        </div>
        <div v-if="snapshots.length" class="snapshots">
          <h3>Local snapshots</h3>
          <button
            v-for="snapshot in snapshots"
            :key="snapshot.generation"
            class="snapshot-button"
            type="button"
            :title="snapshot.generation"
            @click="openSnapshot(snapshot.generation)"
          >{{ snapshot.project }} · {{ snapshot.generation.slice(0, 10) }}</button>
        </div>
        <div v-if="searchResults.length" class="results">
          <button
            v-for="node in searchResults"
            :key="node.id"
            type="button"
            @click="selectedId = node.id"
          >
            <span>{{ node.label }}</span>
            <small>{{ node.kind }}</small>
          </button>
        </div>
      </nav>

      <div v-if="error && !graph" class="error-card">
        <strong>Could not load artifact</strong>
        <p>{{ error }}</p>
      </div>
      <template v-if="graph">
        <div v-if="error" class="error-banner">
          <strong>Candidate failed; showing last good view.</strong>
          <span>{{ error }}</span>
        </div>
        <GraphCanvas :graph="graph" :selected-id="selectedId" @select="selectedId = $event" />
        <InspectorPanel
          :graph="graph"
          :selected-id="selectedId"
          :source="source"
          :source-path="sourcePath"
          @source="openSource"
        />
      </template>
      <div v-else-if="artifact && !error" class="document-card">
        <p class="eyebrow">Loaded {{ artifact.artifact }}</p>
        <h1>Structured document view</h1>
        <p>
          This artifact is valid JSON, but it has no graph projection in visualization schema 1.
          Verify, Diff, and Act overlays arrive in a later increment.
        </p>
        <pre>{{ JSON.stringify(artifact.document, null, 2) }}</pre>
      </div>
    </section>
  </main>
</template>
