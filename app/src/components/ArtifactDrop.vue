<script setup lang="ts">
import { ref } from "vue";

const emit = defineEmits<{
  directory: [files: File[]];
  select: [file: File];
  zip: [file: File];
}>();
const dragging = ref(false);
const artifactInput = ref<HTMLInputElement | null>(null);
const directoryInput = ref<HTMLInputElement | null>(null);
const zipInput = ref<HTMLInputElement | null>(null);

function select(files: FileList | null) {
  if (files?.length) emit("select", files[0]);
}

function drop(event: DragEvent) {
  dragging.value = false;
  const files = event.dataTransfer?.files || null;
  if (!files?.length) return;
  const file = files[0];
  if (file.name.toLocaleLowerCase().endsWith(".zip")) emit("zip", file);
  else emit("select", file);
}

function selectDirectory(files: FileList | null) {
  if (files?.length) emit("directory", [...files]);
}

function selectZip(files: FileList | null) {
  if (files?.length) emit("zip", files[0]);
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
    <small>Saved evidence or local source · nothing is uploaded</small>
    <input
      ref="artifactInput"
      type="file"
      accept="application/json,.json"
      hidden
      @change="select(($event.target as HTMLInputElement).files)"
    />
    <input
      ref="directoryInput"
      type="file"
      webkitdirectory
      multiple
      hidden
      @change="selectDirectory(($event.target as HTMLInputElement).files)"
    />
    <input
      ref="zipInput"
      type="file"
      accept="application/zip,.zip"
      hidden
      @change="selectZip(($event.target as HTMLInputElement).files)"
    />
  </div>
</template>
