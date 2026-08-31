<script setup lang="ts">
import { computed } from "vue";
import type { MetricRow, MetricSummary } from "../graph/metrics";

const props = withDefaults(defineProps<{
  selectedKey?: string | null;
  summary: MetricSummary;
}>(), { selectedKey: null });
const emit = defineEmits<{ select: [row: MetricRow] }>();

const logarithmic = computed(() => {
  const { maximum, minimum, q3 } = props.summary;
  return maximum !== null && minimum !== null && q3 !== null
    && maximum - minimum > 4 * Math.max(1, q3 - minimum);
});

function position(value: number | null): number {
  const { maximum, minimum } = props.summary;
  if (value === null || maximum === null || minimum === null) return 0;
  if (maximum === minimum) return 50;
  const distance = value - minimum;
  const extent = maximum - minimum;
  const ratio = logarithmic.value
    ? Math.log1p(distance) / Math.log1p(extent)
    : distance / extent;
  return Math.max(0, Math.min(100, ratio * 100));
}

function pointStyle(row: MetricRow, index: number): Record<string, string> {
  return {
    "--metric-dot-row": String(index % 3),
    left: `${position(row.value)}%`,
  };
}

function pointLabel(row: MetricRow): string {
  const identity = row.path
    ? `${row.path}${row.symbol ? `:${row.symbol}` : ""}`
    : row.label;
  return `${identity}: ${row.value}; above p99`;
}

const plotLabel = computed(() => {
  const summary = props.summary;
  return [
    `Distribution from ${summary.minimum} to ${summary.maximum}`,
    `first quartile ${summary.q1}`,
    `median ${summary.median}`,
    `third quartile ${summary.q3}`,
    `p95 ${summary.p95}`,
    `p99 ${summary.p99}`,
    `${summary.tailPoints.length} values above p99`,
    logarithmic.value ? "logarithmic position scale" : "linear position scale",
  ].join("; ");
});
</script>

<template>
  <div
    class="metric-box-plot"
    :class="{ logarithmic }"
    role="img"
    :aria-label="plotLabel"
    :title="plotLabel"
  >
    <span class="metric-plot-axis"></span>
    <span
      class="metric-plot-whisker"
      :style="{
        left: `${position(summary.lowerWhisker)}%`,
        width: `${position(summary.upperWhisker) - position(summary.lowerWhisker)}%`,
      }"
    ></span>
    <span
      class="metric-plot-box"
      :style="{
        left: `${position(summary.q1)}%`,
        width: `${Math.max(1, position(summary.q3) - position(summary.q1))}%`,
      }"
    ></span>
    <span
      class="metric-plot-median"
      :style="{ left: `${position(summary.median)}%` }"
    ></span>
    <span
      class="metric-plot-p99"
      :style="{ left: `${position(summary.p99)}%` }"
    ></span>
    <button
      v-for="(row, index) in summary.tailPoints"
      :key="row.key"
      type="button"
      class="metric-plot-point extreme"
      :class="{ active: selectedKey === row.key }"
      :style="pointStyle(row, index)"
      :aria-label="pointLabel(row)"
      :title="pointLabel(row)"
      @click="emit('select', row)"
    ></button>
  </div>
</template>
