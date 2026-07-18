<template><button @click="run">Run</button></template>
<script setup>
import { add } from '../js/runtime.js'

function run() {
  return add(1, 2)
}
</script>
