<script setup lang="ts">
import type { VerificationConstraint } from "../graph/verification";

defineProps<{
  constraints: VerificationConstraint[];
  selectedId: string | null;
  unmappedFindings: number;
}>();
const emit = defineEmits<{ select: [id: string | null] }>();
</script>

<template>
  <section v-if="constraints.length" class="constraint-panel" aria-label="Verification constraints">
    <header>
      <h3>Constraints</h3>
      <button
        v-if="selectedId"
        type="button"
        @click="emit('select', null)"
      >Show all</button>
    </header>
    <button
      v-for="constraint in constraints"
      :key="constraint.id"
      class="constraint-row"
      :class="{ active: constraint.id === selectedId }"
      :data-status="constraint.status"
      type="button"
      @click="emit('select', constraint.id === selectedId ? null : constraint.id)"
    >
      <span class="constraint-status"></span>
      <span>
        <strong>{{ constraint.id }}</strong>
        <small>
          {{ constraint.assert }} · {{ constraint.findings.length }}
          {{ constraint.findings.length === 1 ? 'finding' : 'findings' }}
        </small>
      </span>
    </button>
    <p v-if="unmappedFindings" class="unmapped-findings">
      {{ unmappedFindings }}
      {{ unmappedFindings === 1 ? 'finding has' : 'findings have' }}
      no visible graph location.
    </p>
  </section>
</template>
