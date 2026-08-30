<script setup lang="ts">
import { ref } from "vue";

withDefaults(defineProps<{
  prominent?: boolean;
  status: string;
}>(), {
  prominent: false,
});
const emit = defineEmits<{
  directory: [files: File[]];
  select: [file: File];
  zip: [file: File];
}>();
const dragging = ref(false);
const artifactInput = ref<HTMLInputElement | null>(null);
const directoryInput = ref<HTMLInputElement | null>(null);
const zipInput = ref<HTMLInputElement | null>(null);

function select(input: HTMLInputElement) {
  if (input.files?.length) emit("select", input.files[0]);
  input.value = "";
}

function drop(event: DragEvent) {
  dragging.value = false;
  const files = event.dataTransfer?.files || null;
  if (!files?.length) return;
  const file = files[0];
  if (file.name.toLocaleLowerCase().endsWith(".zip")) emit("zip", file);
  else emit("select", file);
}

function selectDirectory(input: HTMLInputElement) {
  if (input.files?.length) emit("directory", [...input.files]);
  input.value = "";
}

function selectZip(input: HTMLInputElement) {
  if (input.files?.length) emit("zip", input.files[0]);
  input.value = "";
}
</script>

<template>
  <div
    class="artifact-drop"
    :class="{ dragging, prominent }"
    @dragenter.prevent="dragging = true"
    @dragover.prevent="dragging = true"
    @dragleave.prevent="dragging = false"
    @drop.prevent="drop"
  >
    <div v-if="prominent" class="source-intro">
      <p class="eyebrow">Local source</p>
      <h2>Choose what to explore</h2>
      <p>Map source in this browser, or reopen canonical Archbird evidence.</p>
    </div>
    <div class="picker-actions">
      <button type="button" @click="directoryInput?.click()">
        <strong>{{ prominent ? 'Project folder' : 'Folder' }}</strong>
        <small v-if="prominent">Analyze a local directory snapshot</small>
      </button>
      <button type="button" @click="zipInput?.click()">
        <strong>{{ prominent ? 'Source ZIP' : 'ZIP' }}</strong>
        <small v-if="prominent">Analyze an exported repository</small>
      </button>
      <button type="button" @click="artifactInput?.click()">
        <strong>{{ prominent ? 'Saved artifact' : 'Artifact' }}</strong>
        <small v-if="prominent">Review an existing Map or result</small>
      </button>
    </div>
    <small class="source-status">{{ status }}</small>
    <p v-if="prominent" class="local-trust">
      Nothing is sent to a remote service. Folder and ZIP analysis runs locally.
    </p>
    <input
      ref="artifactInput"
      type="file"
      accept="application/json,.json"
      hidden
      @change="select($event.target as HTMLInputElement)"
    />
    <input
      ref="directoryInput"
      type="file"
      webkitdirectory
      multiple
      hidden
      @change="selectDirectory($event.target as HTMLInputElement)"
    />
    <input
      ref="zipInput"
      type="file"
      accept="application/zip,.zip"
      hidden
      @change="selectZip($event.target as HTMLInputElement)"
    />
  </div>
</template>
