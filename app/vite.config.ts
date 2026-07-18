import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";

export default defineConfig({
  base: "./",
  plugins: [vue()],
  resolve: {
    preserveSymlinks: true,
  },
  optimizeDeps: {
    include: ["archbird/wasm"],
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
