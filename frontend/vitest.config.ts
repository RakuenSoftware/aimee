import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

// Vitest runs the dashboard data-contract tests. The React plugin transforms
// the TSX modules under test; the environment is `node` because the tests
// exercise the pure `toDashData` transform, not the DOM.
export default defineConfig({
  plugins: [react()],
  test: {
    environment: 'node',
    include: ['src/**/*.test.ts', 'src/**/*.test.tsx'],
  },
});
