/* The left-nav tab registry — the single source of truth for what tools the GUI
 * exposes. App.tsx renders these as NavLinks/Routes; the per-tab tutorial content
 * (help/tutorials.ts) is keyed on these routes, and tutorials.test.ts imports this
 * list so a tab added here without a tutorial entry fails the coverage test rather
 * than silently shipping with no help. Kept in its own module (not App.tsx) so that
 * test can import it without pulling in the whole app/router tree. */

export type Tab = { label: string; icon: string; route: string };

export const NAV_ITEMS: Tab[] = [
  { label: 'Chat', icon: '💬', route: '/chat' },
  { label: 'Dashboard', icon: '📊', route: '/dashboard' },
  { label: 'Logs', icon: '📜', route: '/logs' },
  { label: 'Edit Workflows', icon: '🔀', route: '/edit-workflows' },
  { label: 'Workflow Actions', icon: '📝', route: '/workflow-actions' },
  { label: 'Agents', icon: '🤝', route: '/agents' },
  { label: 'Personas', icon: '🎭', route: '/personas' },
  { label: 'Roles', icon: '🎬', route: '/roles' },
  { label: 'Roundtable', icon: '⚖️', route: '/roundtable' },
  { label: 'Projects', icon: '📁', route: '/projects' },
  { label: 'Graph', icon: '🕸️', route: '/graph' },
  { label: 'Pipeline', icon: '🧩', route: '/pipeline' },
  { label: 'Editor', icon: '🖥️', route: '/editor' },
  { label: 'Settings', icon: '⚙️', route: '/settings' },
];
