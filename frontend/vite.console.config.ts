import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { resolve } from 'path';

// Second SPA build for the aimee-kb web console. Reuses the same tooling and the
// vendored @rakuensoftware/smoothgui as the main webchat SPA, but builds the
// console.html entry into a single inlined file under dist-console/.
export default defineConfig({
  plugins: [react(), viteSingleFile()],
  build: {
    outDir: 'dist-console',
    emptyOutDir: true,
    assetsInlineLimit: 100000000,
    cssCodeSplit: false,
    rollupOptions: {
      input: resolve(__dirname, 'console.html'),
    },
  },
});
