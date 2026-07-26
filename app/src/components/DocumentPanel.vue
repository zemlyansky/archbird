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

const proposalCandidates = computed(() => rows(props.artifact.document.candidates));
const proposalUnknowns = computed(() => rows(props.artifact.document.unknowns));
const resultOutcomes = computed(() => rows(props.artifact.document.outcomes));

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
      <h1 v-else-if="artifact.artifact === 'change-proposal'">Change proposal</h1>
      <h1 v-else-if="artifact.artifact === 'change-contract'">Reviewed change contract</h1>
      <h1 v-else-if="artifact.artifact === 'change-result'">Change result</h1>
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

    <template v-else-if="artifact.artifact === 'change-proposal'">
      <div class="metric-strip">
        <div><strong>{{ proposalCandidates.length }}</strong><span>candidate sites</span></div>
        <div><strong>{{ rows(artifact.document.postconditions).length }}</strong><span>postconditions</span></div>
        <div><strong>{{ rows(artifact.document.preserved_invariants).length }}</strong><span>preserved</span></div>
        <div><strong>{{ proposalUnknowns.length }}</strong><span>unknowns</span></div>
      </div>
      <article v-for="candidate in proposalCandidates" :key="String(candidate.id)" class="document-row">
        <p class="eyebrow">{{ candidate.kind }}</p>
        <h2>{{ candidate.path || candidate.id }}</h2>
        <p>{{ candidate.reason }}</p>
      </article>
      <article v-for="unknown in proposalUnknowns" :key="String(unknown.id)" class="document-row warning-row">
        <p class="eyebrow">Unknown frontier</p>
        <h2>{{ unknown.code || unknown.id }}</h2>
        <p>{{ unknown.message }}</p>
      </article>
    </template>

    <template v-else-if="artifact.artifact === 'change-contract'">
      <dl class="definition-list">
        <dt>Objective</dt><dd>{{ artifact.document.objective }}</dd>
        <dt>Owner</dt><dd>{{ artifact.document.owner }}</dd>
        <dt>Rationale</dt><dd>{{ artifact.document.rationale }}</dd>
        <dt>Selected candidates</dt><dd>{{ (artifact.document.selected_candidates as unknown[])?.length || 0 }}</dd>
        <dt>Postconditions</dt><dd>{{ (artifact.document.postconditions as unknown[])?.length || 0 }}</dd>
        <dt>Preserved constraints</dt><dd>{{ (artifact.document.preserved_constraints as unknown[])?.length || 0 }}</dd>
      </dl>
    </template>

    <template v-else-if="artifact.artifact === 'change-result'">
      <div class="result-status" :data-status="artifact.document.status">
        <span>Overall result</span><strong>{{ artifact.document.status }}</strong>
      </div>
      <article v-for="outcome in resultOutcomes" :key="String(outcome.id)" class="document-row">
        <p class="eyebrow">{{ outcome.kind }}</p>
        <h2>{{ outcome.id }}</h2>
        <p><strong>{{ outcome.status }}</strong> · {{ outcome.message }}</p>
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
