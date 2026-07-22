<script setup lang="ts">
import { computed, ref, watch } from "vue";
import type { ParsedArtifact } from "../artifacts/model";
import {
  reviewedProjectConfiguration,
  verificationFindings,
  waiverCandidate,
} from "../artifacts/verification";

const props = defineProps<{ artifact: ParsedArtifact }>();
const message = ref("");
const projectConfiguration = ref<Record<string, unknown>>(editableDocument(props.artifact));
const selectedFinding = ref(0);
const owner = ref("");
const rationale = ref("");
const expiresOn = ref("");

watch(() => props.artifact, (value) => {
  projectConfiguration.value = editableDocument(value);
  selectedFinding.value = 0;
  message.value = "";
});

const constraints = computed(() => Array.isArray(projectConfiguration.value.constraints)
  ? projectConfiguration.value.constraints as Record<string, unknown>[]
  : []);
const findings = computed(() => verificationFindings(props.artifact.document));

function editableDocument(artifact: ParsedArtifact): Record<string, unknown> {
  const result = structuredClone(artifact.document);
  if (artifact.artifact === "project-configuration" &&
      result.constraints && !Array.isArray(result.constraints) &&
      typeof result.constraints === "object") {
    result.constraints = Object.entries(result.constraints as Record<string, unknown>)
      .map(([id, definition]) => ({
        id,
        ...(definition as Record<string, unknown>),
      }));
  }
  return result;
}

function requirements(constraint: Record<string, unknown>): string {
  if (Array.isArray(constraint.requirement)) return constraint.requirement.join(", ");
  return typeof constraint.requirement === "string" ? constraint.requirement : "";
}

function updateRequirements(constraint: Record<string, unknown>, value: string) {
  constraint.requirement = [...new Set(value.split(",").map((row) => row.trim()).filter(Boolean))].sort();
}

function requirementsInput(constraint: Record<string, unknown>, event: Event) {
  updateRequirements(constraint, (event.target as HTMLInputElement).value);
}

function textInput(constraint: Record<string, unknown>, field: string, event: Event) {
  constraint[field] = (event.target as HTMLInputElement | HTMLTextAreaElement).value;
}

function downloadJson(payload: Record<string, unknown>, name: string) {
  const bytes = `${JSON.stringify(payload, null, 2)}\n`;
  const url = URL.createObjectURL(new Blob([bytes], { type: "application/json" }));
  const anchor = documentElement("a");
  anchor.href = url;
  anchor.download = name;
  anchor.hidden = true;
  document.body.append(anchor);
  anchor.click();
  window.setTimeout(() => {
    anchor.remove();
    URL.revokeObjectURL(url);
  }, 60_000);
}

function documentElement(tag: string): HTMLAnchorElement {
  return document.createElement(tag) as HTMLAnchorElement;
}

function saveProjectConfiguration() {
  try {
    const configuration = reviewedProjectConfiguration(projectConfiguration.value);
    downloadJson(configuration, "archbird.json");
    message.value = "Saved the schema-2 project configuration for review.";
  } catch (error) {
    message.value = (error as Error).message;
  }
}

function saveWaiver() {
  try {
    const finding = findings.value[selectedFinding.value];
    if (!finding) throw new Error("select a finding first");
    const waiver = waiverCandidate(finding, {
      owner: owner.value,
      rationale: rationale.value,
      expiresOn: expiresOn.value,
    });
    downloadJson(waiver, `${String(waiver.id).toLowerCase()}.waiver.json`);
    message.value = `Saved a waiver entry. Review it under constraints.${finding.constraint}.waivers.`;
  } catch (error) {
    message.value = (error as Error).message;
  }
}
</script>

<template>
  <section class="contract-editor">
    <template v-if="artifact.artifact === 'project-configuration'">
      <p class="eyebrow">Project architecture policy</p>
      <h1>{{ projectConfiguration.project }}</h1>
      <p>
        Edit constraint ownership, requirements, and rationale in the same
        project configuration used by Map, Query, and Verify.
      </p>
      <article v-for="(constraint, index) in constraints" :key="String(constraint.id || index)" class="check-editor">
        <label>Constraint ID
          <input :value="String(constraint.id || '')" @input="textInput(constraint, 'id', $event)" />
        </label>
        <label>Owner
          <input :value="String(constraint.owner || '')" @input="textInput(constraint, 'owner', $event)" />
        </label>
        <label>Requirement IDs
          <input :value="requirements(constraint)" @input="requirementsInput(constraint, $event)" />
        </label>
        <label>Rationale
          <textarea :value="String(constraint.rationale || '')" @input="textInput(constraint, 'rationale', $event)"></textarea>
        </label>
        <code>{{ constraint.kind || constraint.assert }}</code>
      </article>
      <button type="button" @click="saveProjectConfiguration">Save project configuration</button>
    </template>
    <template v-else-if="artifact.artifact === 'verification'">
      <p class="eyebrow">Derived findings</p>
      <h1>{{ artifact.document.policy && (artifact.document.policy as Record<string, unknown>).project }}</h1>
      <p>
        Findings are derived and cannot be edited. Create an explicit, expiring
        waiver candidate for review instead.
      </p>
      <label>Finding
        <select v-model.number="selectedFinding">
          <option v-for="(finding, index) in findings" :key="finding.fingerprint" :value="index">
            {{ finding.constraint }} · {{ finding.comparison }} · {{ finding.key }}
          </option>
        </select>
      </label>
      <p v-if="findings[selectedFinding]" class="finding-message">
        {{ findings[selectedFinding].message }}
      </p>
      <label>Owner <input v-model="owner" /></label>
      <label>Rationale <textarea v-model="rationale"></textarea></label>
      <label>Expires on <input v-model="expiresOn" type="date" /></label>
      <button type="button" :disabled="!findings.length" @click="saveWaiver">Save waiver candidate</button>
    </template>
    <p v-if="message" class="editor-message">{{ message }}</p>
    <details>
      <summary>Canonical input</summary>
      <pre>{{ JSON.stringify(projectConfiguration, null, 2) }}</pre>
    </details>
  </section>
</template>
