<script setup lang="ts">
import { computed } from "vue";
import type { ParsedArtifact } from "../artifacts/model";

const props = defineProps<{ artifact: ParsedArtifact }>();

interface DiffSection {
  added: unknown[];
  changed: unknown[];
  removed: unknown[];
}

function rows(value: unknown): Record<string, unknown>[] {
  return Array.isArray(value)
    ? value.filter((row): row is Record<string, unknown> =>
      Boolean(row) && typeof row === "object" && !Array.isArray(row))
    : [];
}

const diffSections = computed(() => {
  const sections = props.artifact.document.sections;
  if (props.artifact.artifact !== "diff" || !sections ||
      typeof sections !== "object" || Array.isArray(sections)) return [];
  return Object.entries(sections as Record<string, unknown>).flatMap(([name, raw]) => {
    if (!raw || typeof raw !== "object" || Array.isArray(raw)) return [];
    const section = raw as Record<string, unknown>;
    const value: DiffSection = {
      added: Array.isArray(section.added) ? section.added : [],
      changed: Array.isArray(section.changed) ? section.changed : [],
      removed: Array.isArray(section.removed) ? section.removed : [],
    };
    const total = value.added.length + value.changed.length + value.removed.length;
    return total ? [{ name, total, ...value }] : [];
  });
});

const planItems = computed(() => rows(props.artifact.document.items));
const planUnknowns = computed(() => rows(props.artifact.document.unknowns));
const preservedConstraints = computed(() =>
  Array.isArray(props.artifact.document.preserved_constraints)
    ? props.artifact.document.preserved_constraints.length
    : 0);
const actChanges = computed(() => rows(props.artifact.document.changes));
const actAcceptance = computed(() => {
  const value = props.artifact.document.acceptance;
  return value && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {};
});

function label(value: unknown): string {
  if (typeof value === "string") return value;
  if (!value || typeof value !== "object" || Array.isArray(value)) return JSON.stringify(value);
  const row = value as Record<string, unknown>;
  return String(row.name || row.id || row.key || row.path || row.kind || JSON.stringify(row));
}
</script>

<template>
  <section class="document-workspace">
    <header>
      <p class="eyebrow">{{ artifact.artifact }}</p>
      <h1 v-if="artifact.artifact === 'diff'">Structural comparison</h1>
      <h1 v-else-if="artifact.artifact === 'plan'">Structural change plan</h1>
      <h1 v-else-if="artifact.artifact === 'act-result'">Act result</h1>
      <h1 v-else>Structured evidence</h1>
    </header>

    <template v-if="artifact.artifact === 'diff'">
      <div class="metric-strip">
        <div><strong>{{ diffSections.length }}</strong><span>changed sections</span></div>
        <div>
          <strong>{{ diffSections.reduce((sum, section) => sum + section.added.length, 0) }}</strong>
          <span>added</span>
        </div>
        <div>
          <strong>{{ diffSections.reduce((sum, section) => sum + section.changed.length, 0) }}</strong>
          <span>changed</span>
        </div>
        <div>
          <strong>{{ diffSections.reduce((sum, section) => sum + section.removed.length, 0) }}</strong>
          <span>removed</span>
        </div>
      </div>
      <p v-if="!diffSections.length" class="empty-state">The selected Maps are structurally identical.</p>
      <article v-for="section in diffSections" :key="section.name" class="document-row">
        <h2>{{ section.name.replaceAll('_', ' ') }}</h2>
        <p>{{ section.added.length }} added · {{ section.changed.length }} changed · {{ section.removed.length }} removed</p>
        <details>
          <summary>Changed identities</summary>
          <ul>
            <li v-for="item in section.added" :key="`a:${label(item)}`"><ins>+ {{ label(item) }}</ins></li>
            <li v-for="item in section.changed" :key="`c:${label(item)}`">~ {{ label(item) }}</li>
            <li v-for="item in section.removed" :key="`r:${label(item)}`"><del>- {{ label(item) }}</del></li>
          </ul>
        </details>
      </article>
    </template>

    <template v-else-if="artifact.artifact === 'plan'">
      <div class="metric-strip">
        <div><strong>{{ planItems.length }}</strong><span>plan items</span></div>
        <div><strong>{{ planItems.filter((item) => item.executable).length }}</strong><span>executable</span></div>
        <div><strong>{{ preservedConstraints }}</strong><span>preserved</span></div>
        <div><strong>{{ planUnknowns.length }}</strong><span>unknowns</span></div>
      </div>
      <p class="document-lead">{{ artifact.document.objective }}</p>
      <article v-for="item in planItems" :key="String(item.id)" class="document-row">
        <p class="eyebrow">{{ (item.operation as Record<string, unknown>)?.action || 'manual' }}</p>
        <h2>{{ item.statement || item.id }}</h2>
        <p>{{ item.executable ? 'Ready for deterministic preview.' : 'Requires reviewed transformation input.' }}</p>
        <ul v-if="(item.non_executable_reasons as unknown[])?.length">
          <li v-for="reason in item.non_executable_reasons as unknown[]" :key="String(reason)">
            {{ reason }}
          </li>
        </ul>
      </article>
      <article v-for="unknown in planUnknowns" :key="String(unknown.id)" class="document-row warning-row">
        <p class="eyebrow">Unknown frontier</p>
        <h2>{{ unknown.id }}</h2>
        <p>{{ unknown.statement }}</p>
      </article>
    </template>

    <template v-else-if="artifact.artifact === 'act-result'">
      <div class="result-status" :data-status="artifact.document.status">
        <span>Act</span><strong>{{ artifact.document.status }}</strong>
      </div>
      <dl class="definition-list">
        <dt>Acceptance</dt><dd>{{ actAcceptance.status || 'not evaluated' }}</dd>
        <dt>Plan</dt><dd>{{ artifact.document.plan_sha256 }}</dd>
        <dt>Files changed</dt><dd>{{ actChanges.length }}</dd>
      </dl>
      <article v-for="change in actChanges" :key="String(change.path)" class="document-row">
        <p class="eyebrow">{{ change.kind }}</p>
        <h2>{{ change.path }}</h2>
        <p>{{ (change.item_ids as unknown[])?.length || 0 }} Plan item(s)</p>
        <details>
          <summary>Patch</summary>
          <pre>{{ change.unified_diff }}</pre>
        </details>
      </article>
    </template>

    <p v-else class="empty-state">
      No task-oriented renderer is registered for this artifact type.
    </p>

    <details class="canonical-input">
      <summary>Canonical input</summary>
      <pre>{{ JSON.stringify(artifact.document, null, 2) }}</pre>
    </details>
  </section>
</template>
