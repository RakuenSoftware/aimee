/* The left-nav tab registry — the single source of truth for what tools the GUI
 * exposes. App.tsx renders these as NavLinks/Routes; the per-tab tutorial content
 * (help/tutorials.ts) is keyed on these routes, and tutorials.test.ts imports this
 * list so a tab added here without a tutorial entry fails the coverage test rather
 * than silently shipping with no help. Kept in its own module (not App.tsx) so that
 * test can import it without pulling in the whole app/router tree. */

/* `hint` is a one-line tooltip (rendered as the NavLink `title` in App.tsx) — a
 * quick "what is this tab" on hover, distinct from the fuller TabTutorial card. */
export type Tab = { label: string; icon: string; route: string; hint: string };

export const NAV_ITEMS: Tab[] = [
  { label: 'Chat', icon: '💬', route: '/chat', hint: 'Talk to Aimee for the active session — attach files, run slash-commands, turn a reply into a proposal.' },
  { label: 'Dashboard', icon: '📊', route: '/dashboard', hint: 'Live instance health: readiness, LSP, per-agent success and tokens, latency, provider mix, traces.' },
  { label: 'Logs', icon: '📜', route: '/logs', hint: 'The audit trail — every recorded action, newest first. Click a row for full detail.' },
  { label: 'Edit Workflows', icon: '🔀', route: '/edit-workflows', hint: 'Design multi-step workflows — each step sets a task, persona, delegate, and role.' },
  { label: 'Workflows', icon: '📝', route: '/workflow-actions', hint: 'The runtime queue — workflow items waiting on you to approve or reject.' },
  { label: 'Agents', icon: '🤝', route: '/agents', hint: 'Your delegates, their run history and stats — bind each to a persona and allowed roles.' },
  { label: 'Personas', icon: '🎭', route: '/personas', hint: 'Edit who Aimee can be — each persona is an identity plus the roles it may use.' },
  { label: 'Roles', icon: '🎬', route: '/roles', hint: 'The shared role vocabulary — each role’s prompt template and per-role turn cap.' },
  { label: 'Roundtable', icon: '⚖️', route: '/roundtable', hint: 'Configure the multi-model review panels — seats, aggregator, and loop knobs.' },
  { label: 'Projects', icon: '📁', route: '/projects', hint: 'Connect the git repositories Aimee works on and store sealed per-host credentials.' },
  { label: 'Graph', icon: '🕸️', route: '/graph', hint: 'Read-only explorer of the code-projection graph for the session’s project.' },
  { label: 'Pipeline', icon: '🧩', route: '/pipeline', hint: 'The curator pipeline — toggle each stage’s enable flag by resource lane.' },
  { label: 'Editor', icon: '🖥️', route: '/editor', hint: 'In-app VS Code bound to the session’s isolated worktree — the same tree the agent edits.' },
  { label: 'Settings', icon: '⚙️', route: '/settings', hint: 'Every typed config option with plain-English help; restart-sensitive keys are badged.' },
];
