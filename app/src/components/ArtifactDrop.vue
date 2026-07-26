<script setup lang="ts">
import { ref } from "vue";

defineProps<{ status: string }>();
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
    :class="{ dragging }"
    @dragenter.prevent="dragging = true"
    @dragover.prevent="dragging = true"
    @dragleave.prevent="dragging = false"
    @drop.prevent="drop"
  >
    <div class="picker-actions">
      <button type="button" @click="artifactInput?.click()">Artifact</button>
      <button type="button" @click="directoryInput?.click()">Folder</button>
      <button type="button" @click="zipInput?.click()">ZIP</button>
    </div>
    <small>{{ status }}</small>
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
