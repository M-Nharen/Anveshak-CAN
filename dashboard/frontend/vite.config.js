import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';

// `npm run dev` proxies the API and the WebSocket to a backend on :8080.
export default defineConfig({
  plugins: [svelte()],
  server: {
    proxy: {
      '/api': 'http://localhost:8080',
      '/ws': { target: 'ws://localhost:8080', ws: true },
    },
  },
  build: { outDir: 'dist', emptyOutDir: true },
});
