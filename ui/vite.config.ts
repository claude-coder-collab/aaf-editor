import { svelte } from "@sveltejs/vite-plugin-svelte";
import { defineConfig } from "vitest/config";
import { viteSingleFile } from "vite-plugin-singlefile";

export default defineConfig({
  plugins: [svelte(), viteSingleFile()],
  build: { outDir: "dist", emptyOutDir: true, assetsInlineLimit: 100_000_000 },
  test: { environment: "jsdom", include: ["src/**/*.test.ts"] },
});
