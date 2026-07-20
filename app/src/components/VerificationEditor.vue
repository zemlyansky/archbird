<script setup lang="ts">
import { computed, ref, watch } from "vue";
import type { ParsedArtifact } from "../artifacts/model";
import {
  candidateSuite,
  verificationFindings,
  waiverCandidate,
} from "../artifacts/verification";

const props = defineProps<{ artifact: ParsedArtifact }>();
const message = ref("");
const suite = ref<Record<string, unknown>>(structuredClone(props.artifact.document));
const selectedFinding = ref(0);
const owner = ref("");
const rationale = ref("");
const expiresOn = ref("");

watch(() => props.artifact, (value) => {
  suite.value = structuredClone(value.document);
  selectedFinding.value = 0;
  message.value = "";
});

const checks = computed(() => Array.isArray(suite.value.checks)
  ? suite.value.checks as Record<string, unknown>[]
  : []);
const findings = computed(() => verificationFindings(props.artifact.document));

function requirements(check: Record<string, unknown>): string {
  return Array.isArray(check.requirements) ? check.requirements.join(", ") : "";
}

function updateRequirements(check: Record<string, unknown>, value: string) {
  check.requirements = [...new Set(value.split(",").map((row) => row.trim()).filter(Boolean))].sort();
}

function requirementsInput(check: Record<string, unknown>, event: Event) {
  updateRequirements(check, (event.target as HTMLInputElement).value);
}

function textInput(check: Record<string, unknown>, field: string, event: Event) {
  check[field] = (event.target as HTMLInputElement | HTMLTextAreaElement).value;
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

function saveSuite() {
  try {
    const candidate = candidateSuite(suite.value);
    downloadJson(candidate, `${String(candidate.suite)}.candidate.verify.json`);
    message.value = "Saved an unreviewed candidate. Remove candidate=true only after review.";
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
    message.value = "Saved a waiver candidate. Add it to suite.waivers and review it.";
  } catch (error) {
    message.value = (error as Error).message;
  }
}
</script>

<template>
  <section class="contract-editor">
    <template v-if="artifact.artifact === 'verification-suite'">
      <p class="eyebrow">Asserted architecture contract</p>
      <h1>{{ suite.suite }}</h1>
      <p>
        Edit reviewed IDs and rationale here. Every download is forced back to
        <code>candidate=true</code>; Archbird will refuse to run it until review.
      </p>
      <article v-for="(check, index) in checks" :key="String(check.id || index)" class="check-editor">
        <label>Check ID
          <input :value="String(check.id || '')" @input="textInput(check, 'id', $event)" />
        </label>
        <label>Owner
          <input :value="String(check.owner || '')" @input="textInput(check, 'owner', $event)" />
        </label>
        <label>Requirement IDs
          <input :value="requirements(check)" @input="requirementsInput(check, $event)" />
        </label>
        <label>Rationale
          <textarea :value="String(check.rationale || '')" @input="textInput(check, 'rationale', $event)"></textarea>
        </label>
        <code>{{ check.assert }}</code>
      </article>
      <button type="button" @click="saveSuite">Save candidate suite</button>
    </template>
    <template v-else-if="artifact.artifact === 'verification'">
      <p class="eyebrow">Derived findings</p>
      <h1>{{ artifact.document.suite && (artifact.document.suite as Record<string, unknown>).name }}</h1>
      <p>
        Findings are derived and cannot be edited. Create an explicit, expiring
        waiver candidate for review instead.
      </p>
      <label>Finding
        <select v-model.number="selectedFinding">
          <option v-for="(finding, index) in findings" :key="finding.fingerprint" :value="index">
            {{ finding.check }} · {{ finding.comparison }} · {{ finding.key }}
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
      <pre>{{ JSON.stringify(artifact.document, null, 2) }}</pre>
    </details>
  </section>
</template>
