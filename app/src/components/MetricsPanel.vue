<script setup lang="ts">
import { computed, ref, watch } from "vue";
import type { ProjectionResult } from "../artifacts/projection";
import {
  METRIC_DEFINITIONS,
  metricDefinition,
  summarizeMetric,
  type MetricId,
  type MetricRow,
} from "../graph/metrics";

const props = defineProps<{
  error: string;
  loading: boolean;
  projection: ProjectionResult | null;
  selected: MetricId;
}>();
const emit = defineEmits<{
  change: [metric: MetricId];
  query: [path: string, symbol: string | null];
}>();

const focused = ref<string | null>(null);
const collapsed = ref(false);
const summary = computed(() => props.projection
  ? summarizeMetric(props.projection)
  : null);
const definition = computed(() => metricDefinition(props.selected));
const selectedRow = computed(() =>
  summary.value?.rows.find((row) => row.key === focused.value) || null);
const maximumBucket = computed(() => Math.max(
  1,
  ...(summary.value?.histogram.map((bucket) => bucket.count) || []),
));

watch(() => props.projection, () => {
  focused.value = null;
});

function value(row: MetricRow): string {
  if (row.value !== null) return String(row.value);
  return row.lowerBound !== null ? `≥${row.lowerBound}` : "unknown";
}

function evidence(row: MetricRow): string[] {
  return row.evidence.map((entry) => {
    const path = typeof entry.path === "string" ? entry.path : row.path || "source";
    const line = typeof entry.line === "number" ? `:${entry.line}` : "";
    const detail = typeof entry.detail === "string" ? entry.detail : JSON.stringify(entry);
    return `${path}${line} · ${detail}`;
  });
}

function select(row: MetricRow): void {
  focused.value = focused.value === row.key ? null : row.key;
}
</script>

<template>
  <section class="metrics-board" :class="{ collapsed }" aria-label="Code metrics">
    <header>
      <div>
        <p class="eyebrow">Measured architecture</p>
        <h2>{{ definition.label }} distribution</h2>
        <p>{{ definition.description }}</p>
      </div>
      <div class="metrics-actions">
        <button type="button" @click="collapsed = !collapsed">
          {{ collapsed ? 'Expand' : 'Minimize' }}
        </button>
        <label v-if="!collapsed">Metric
          <select :value="selected" @change="emit('change', ($event.target as HTMLSelectElement).value as MetricId)">
            <option v-for="metric in METRIC_DEFINITIONS" :key="metric.id" :value="metric.id">
              {{ metric.label }}
            </option>
          </select>
        </label>
      </div>
    </header>

    <p v-if="!collapsed && loading" class="metrics-state">Evaluating exhaustive metric evidence…</p>
    <p v-else-if="!collapsed && error" class="metrics-state metrics-error">{{ error }}</p>
    <template v-else-if="!collapsed && summary">
      <div class="metrics-strip">
        <div><strong>{{ summary.measured }}</strong><span>Measured</span></div>
        <div><strong>{{ summary.median ?? '—' }}</strong><span>Median</span></div>
        <div><strong>{{ summary.p95 ?? '—' }}</strong><span>P95</span></div>
        <div><strong>{{ summary.maximum ?? '—' }}</strong><span>Maximum</span></div>
      </div>

      <div class="metrics-body">
        <figure class="metric-histogram">
          <div class="metric-bars" :aria-label="`${definition.label} histogram`">
            <div v-for="bucket in summary.histogram" :key="bucket.label" class="metric-bucket">
              <span
                class="metric-bar"
                :style="{ height: `${Math.max(5, bucket.count / maximumBucket * 100)}%` }"
                :title="`${bucket.label}: ${bucket.count}`"
              ></span>
              <small>{{ bucket.label }}</small>
            </div>
          </div>
          <figcaption>
            {{ summary.measured }} exact values · {{ summary.unknown }} unknown
            · {{ summary.complete ? 'complete population' : 'incomplete population' }}
          </figcaption>
          <div class="metric-scale" aria-label="Metric heat scale">
            <span>Lower measured</span><i></i><span>Highest measured</span>
          </div>
          <p class="metric-note">Measurements, not quality scores.</p>
        </figure>

        <section class="metric-hotspots">
          <h3>Highest measured values</h3>
          <button
            v-for="row in summary.rows.slice(0, 10)"
            :key="row.key"
            class="metric-hotspot"
            :class="{ active: focused === row.key }"
            type="button"
            @click="select(row)"
          >
            <span>
              <strong>{{ row.label }}</strong>
              <small>{{ row.path || row.attributes.node || row.key }}</small>
            </span>
            <b>{{ value(row) }}</b>
          </button>
        </section>
      </div>

      <section v-if="selectedRow" class="metric-evidence">
        <div>
          <p class="eyebrow">Exact metric evidence</p>
          <h3>{{ selectedRow.path }}{{ selectedRow.symbol ? `:${selectedRow.symbol}` : '' }}</h3>
          <p>{{ selectedRow.message || `${definition.label} = ${value(selectedRow)}` }}</p>
        </div>
        <button
          v-if="selectedRow.path"
          type="button"
          @click="emit('query', selectedRow.path, selectedRow.symbol)"
        >Open focused context</button>
        <ul>
          <li v-for="entry in evidence(selectedRow)" :key="entry"><code>{{ entry }}</code></li>
        </ul>
      </section>
    </template>
  </section>
</template>
