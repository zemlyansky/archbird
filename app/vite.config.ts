import vue from "@vitejs/plugin-vue";
import { defineConfig } from "vitest/config";

export default defineConfig({
  base: "./",
  plugins: [vue()],
  resolve: {
    preserveSymlinks: true,
  },
  optimizeDeps: {
    include: ["archbird/wasm"],
  },
  test: {
    include: ["test/**/*.spec.ts"],
  },
  build: {
    chunkSizeWarningLimit: 1500,
    target: "es2022",
    sourcemap: false,
    commonjsOptions: {
      include: [/node_modules/],
    },
  },
  worker: {
    format: "es",
  },
});
