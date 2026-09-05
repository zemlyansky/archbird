<script setup lang="ts">
import { computed, ref, watchEffect } from "vue";
import MetricBoxPlot from "./MetricBoxPlot.vue";
import type { GraphView } from "../artifacts/model";
import type { ProjectionResult } from "../artifacts/projection";
import {
  METRIC_DEFINITIONS,
  summarizeMetricScope,
  type MetricDefinition,
  type MetricId,
  type MetricRow,
  type MetricScopeMode,
  type ScopedMetricSummary,
} from "../graph/metrics";

const props = defineProps<{
  errors: ReadonlyMap<MetricId, string>;
  graph: GraphView;
  loading: ReadonlySet<MetricId>;
  overlay: MetricId | null;
  projections: ReadonlyMap<MetricId, ProjectionResult>;
  scope: MetricScopeMode;
  selectedId: string | null;
}>();
const emit = defineEmits<{
  load: [metric: MetricId];
  overlay: [metric: MetricId | null];
  query: [path: string, symbol: string | null];
  scope: [scope: MetricScopeMode];
}>();

const focused = ref<ReadonlyMap<MetricId, string>>(new Map());
const expanded = ref<ReadonlySet<MetricId>>(new Set());
watchEffect(() => {
  // Only open metrics request exhaustive evidence; cache resets follow a new Map.
  for (const metric of expanded.value) {
    if (!props.projections.has(metric) && !props.loading.has(metric) && !props.errors.has(metric)) {
      emit("load", metric);
    }
  }
});
const summaries = computed(() => new Map(
  METRIC_DEFINITIONS.flatMap((definition) => {
    const projection = props.projections.get(definition.id);
    return projection
      ? [[definition.id, summarizeMetricScope(
        projection,
        props.graph,
        props.selectedId,
        props.scope,
      )] as const]
      : [];
  }),
));

function summary(metric: MetricId): ScopedMetricSummary | null {
  return summaries.value.get(metric) || null;
}

function selectedRow(metric: MetricId): MetricRow | null {
  const key = focused.value.get(metric);
  return summary(metric)?.summary.rows.find((row) => row.key === key) || null;
}

function value(row: MetricRow): string {
  if (row.value !== null) return String(row.value);
  return row.lowerBound !== null ? `≥${row.lowerBound}` : "unknown";
}

function headline(metric: MetricId): string {
  const metricSummary = summary(metric)?.summary;
  if (!metricSummary) {
    if (props.loading.has(metric)) return "loading";
    return props.errors.has(metric) ? "error" : "Expand to measure";
  }
  if (!metricSummary.population) return "no values";
  if (metricSummary.knownMaximumLowerBound !== null) {
    return `≥${metricSummary.knownMaximumLowerBound}`;
  }
  return `${metricSummary.unknown} unknown`;
}

function hasExactDistribution(metric: MetricId): boolean {
  const metricSummary = summary(metric)?.summary;
  return Boolean(metricSummary?.complete && metricSummary.maximum !== null);
}

function distributionLabel(metric: MetricId): string {
  const metricSummary = summary(metric)?.summary;
  if (!metricSummary || metricSummary.median === null || metricSummary.maximum === null
    || metricSummary.q1 === null || metricSummary.q3 === null) {
    return "No measured values";
  }
  return `Q1 ${metricSummary.q1}; median ${metricSummary.median}; Q3 ${metricSummary.q3}; p95 ${metricSummary.p95}; p99 ${metricSummary.p99}; maximum ${metricSummary.maximum}; ${metricSummary.tailPoints.length} above p99`;
}

function evidence(row: MetricRow): string[] {
  return row.evidence.map((entry) => {
    const path = typeof entry.path === "string" ? entry.path : row.path || "source";
    const line = typeof entry.line === "number" ? `:${entry.line}` : "";
    const detail = typeof entry.detail === "string"
      ? entry.detail
      : JSON.stringify(entry);
    return `${path}${line} · ${detail}`;
  });
}

function select(metric: MetricId, row: MetricRow): void {
  if (row.path) {
    emit("query", row.path, row.symbol);
    return;
  }
  const next = new Map(focused.value);
  if (next.get(metric) === row.key) next.delete(metric);
  else next.set(metric, row.key);
  focused.value = next;
  expanded.value = new Set(expanded.value).add(metric);
}

function toggle(metric: MetricId): void {
  const next = new Set(expanded.value);
  if (next.has(metric)) next.delete(metric);
  else next.add(metric);
  expanded.value = next;
}

function metricRows(metric: MetricId): MetricRow[] {
  return summary(metric)?.summary.rows.slice(0, 8) || [];
}

function hotspotHeading(metric: MetricId): string {
  const metricSummary = summary(metric)?.summary;
  if (!metricSummary?.measured && metricSummary?.lowerBounds) {
    return "Highest known lower bounds";
  }
  return metricSummary?.complete ? "Highest measured values" : "Highest known values";
}

function evidenceHeading(row: MetricRow): string {
  if (row.value !== null) return "Exact metric evidence";
  return row.lowerBound !== null ? "Known lower-bound evidence" : "Unknown metric evidence";
}

function unit(definition: MetricDefinition, count: number): string {
  return `${definition.unit}${count === 1 ? "" : "s"}`;
}
</script>

<template>
  <section class="metrics-panel" aria-label="Code metrics">
    <header class="metrics-header">
      <div>
        <p class="eyebrow">Measured architecture</p>
        <h2>Metrics</h2>
      </div>
      <div class="metric-scope" aria-label="Metric scope">
        <button
          type="button"
          :class="{ active: scope === 'selection' && selectedId }"
          :disabled="!selectedId"
          @click="emit('scope', 'selection')"
        >Selection</button>
        <button
          type="button"
          :class="{ active: scope === 'project' || !selectedId }"
          @click="emit('scope', 'project')"
        >Project</button>
      </div>
    </header>
    <p class="metrics-intro">
      Expand a metric to load its distribution, contributors and evidence.
    </p>

    <section
      v-for="definition in METRIC_DEFINITIONS"
      :key="definition.id"
      class="metric-accordion"
      :class="{ open: expanded.has(definition.id) }"
      :data-metric="definition.id"
    >
      <button
        type="button"
        class="metric-summary"
        :aria-expanded="expanded.has(definition.id)"
        @click="toggle(definition.id)"
      >
        <span class="metric-summary-copy">
          <strong>{{ definition.label }}</strong>
        </span>
        <span
          v-if="hasExactDistribution(definition.id)"
          class="metric-summary-value"
          :title="distributionLabel(definition.id)"
          :aria-label="distributionLabel(definition.id)"
        >
          <strong>{{ summary(definition.id)?.summary.median }}</strong><small> median / {{ summary(definition.id)?.summary.maximum }} max</small>
        </span>
        <span v-else class="metric-summary-value">{{ headline(definition.id) }}</span>
      </button>
      <MetricBoxPlot
        v-if="hasExactDistribution(definition.id)"
        :selected-key="selectedRow(definition.id)?.key"
        :summary="summary(definition.id)!.summary"
        @select="select(definition.id, $event)"
      />

      <div v-if="expanded.has(definition.id)" class="metric-detail">
        <p>{{ definition.description }}</p>
        <p v-if="loading.has(definition.id)" class="metrics-state">
          Evaluating exhaustive metric evidence…
        </p>
        <p v-else-if="errors.get(definition.id)" class="metrics-state metrics-error">
          {{ errors.get(definition.id) }}
          <button type="button" @click="emit('load', definition.id)">Retry</button>
        </p>
        <template v-else-if="summary(definition.id)">
          <dl class="metric-facts">
            <template v-if="hasExactDistribution(definition.id)">
              <dt>Distribution</dt>
              <dd>
                Q1 {{ summary(definition.id)?.summary.q1 }}
                · median {{ summary(definition.id)?.summary.median }}
                · Q3 {{ summary(definition.id)?.summary.q3 }}
                · p95 {{ summary(definition.id)?.summary.p95 }}
                · p99 {{ summary(definition.id)?.summary.p99 }}
                · max {{ summary(definition.id)?.summary.maximum }}
              </dd>
            </template>
            <template v-else>
              <dt>Known maximum</dt>
              <dd>
                {{ summary(definition.id)?.summary.knownMaximumLowerBound === null ? 'unknown' : `≥${summary(definition.id)?.summary.knownMaximumLowerBound}` }}
                {{ unit(definition, summary(definition.id)?.summary.knownMaximumLowerBound ?? 0) }}
              </dd>
            </template>
            <dt>Coverage</dt>
            <dd>
              <template v-if="hasExactDistribution(definition.id)">
                {{ summary(definition.id)?.summary.measured }}/{{ summary(definition.id)?.summary.population }} · complete
              </template>
              <template v-else>
                {{ summary(definition.id)?.summary.measured }} exact
                · {{ summary(definition.id)?.summary.lowerBounds }} lower-bound
                · {{ summary(definition.id)?.summary.population }} total
                · incomplete
              </template>
            </dd>
          </dl>

          <section
            v-if="hasExactDistribution(definition.id)"
            class="metric-distribution"
            :aria-label="`${definition.label} distribution`"
          >
            <div class="metric-distribution-labels">
              <span>min {{ summary(definition.id)?.summary.minimum }}</span>
              <span>Q1 {{ summary(definition.id)?.summary.q1 }}</span>
              <span>median {{ summary(definition.id)?.summary.median }}</span>
              <span>Q3 {{ summary(definition.id)?.summary.q3 }}</span>
              <span>p95 {{ summary(definition.id)?.summary.p95 }}</span>
              <span>p99 {{ summary(definition.id)?.summary.p99 }}</span>
              <span>max {{ summary(definition.id)?.summary.maximum }}</span>
            </div>
            <p>
              {{ summary(definition.id)?.summary.tailPoints.length }} values above p99
            </p>
          </section>

          <div class="metric-actions">
            <button
              type="button"
              :class="{ active: overlay === definition.id }"
              @click="emit('overlay', overlay === definition.id ? null : definition.id)"
            >{{ overlay === definition.id ? 'Remove graph overlay' : 'Show on graph' }}</button>
          </div>

          <section v-if="metricRows(definition.id).length" class="metric-hotspots">
            <h3>{{ hotspotHeading(definition.id) }}</h3>
            <button
              v-for="row in metricRows(definition.id)"
              :key="row.key"
              class="metric-hotspot"
              :class="{ active: selectedRow(definition.id)?.key === row.key }"
              type="button"
              @click="select(definition.id, row)"
            >
              <span>
                <strong>{{ row.label }}</strong>
                <small>{{ row.path || row.attributes.node || row.key }}</small>
              </span>
              <b>{{ value(row) }}</b>
            </button>
          </section>

          <section v-if="selectedRow(definition.id)" class="metric-evidence">
            <div>
              <p class="eyebrow">{{ evidenceHeading(selectedRow(definition.id)!) }}</p>
              <h3>{{ selectedRow(definition.id)?.path }}{{ selectedRow(definition.id)?.symbol ? `:${selectedRow(definition.id)?.symbol}` : '' }}</h3>
              <p>{{ selectedRow(definition.id)?.message || `${definition.label} = ${value(selectedRow(definition.id)!)}` }}</p>
            </div>
            <button
              v-if="selectedRow(definition.id)?.path"
              type="button"
              @click="emit('query', selectedRow(definition.id)!.path!, selectedRow(definition.id)!.symbol)"
            >Open focused context</button>
            <ul v-if="selectedRow(definition.id)?.evidence.length">
              <li v-for="entry in evidence(selectedRow(definition.id)!)" :key="entry">
                <code>{{ entry }}</code>
              </li>
            </ul>
            <p v-else>No finer evidence was emitted for this value.</p>
          </section>
        </template>
        <p v-else class="metrics-state">Metric evidence is unavailable.</p>
        <p class="metric-note">Measurements are not quality scores.</p>
      </div>
    </section>
  </section>
</template>
