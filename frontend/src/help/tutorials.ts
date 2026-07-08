/* Per-tab tutorial content. One entry per route in App's NAV_ITEMS, keyed by the
 * exact route path. `TabTutorial` shows the entry for the active route on first
 * visit and behind a "?" re-opener after. Copy is short (a title + 3–5 lines) and
 * grounded in what each page actually does; `seeAlso` points at the sibling tab a
 * new operator most often confuses this one with. A route with no entry renders
 * nothing (the component degrades gracefully), so adding a nav tab without a
 * tutorial is not an error — it just shows no help until copy is added. */

export type Tutorial = {
  /** Heading shown at the top of the overlay. */
  title: string;
  /** Body lines, rendered as short paragraphs. Keep to 3–5. */
  body: string[];
  /** Optional "where this connects" pointer — a route in NAV_ITEMS. */
  seeAlso?: string;
};

// Keyed by route path (must match App's NAV_ITEMS routes). The test in
// tutorials.test.ts asserts every NAV_ITEMS route has an entry here.
export const TAB_TUTORIALS: Record<string, Tutorial> = {
  '/chat': {
    title: 'Chat',
    body: [
      'This is where a turn runs — your conversation with aimee for the active session.',
      'Attach files, use slash-commands and channels, and turn a reply into a proposal.',
      'The session’s bound project is what every tool here acts on.',
    ],
    seeAlso: '/projects',
  },
  '/dashboard': {
    title: 'Dashboard',
    body: [
      'Live health of the instance: Readiness, LSP health, per-agent success and tokens,',
      'latency by role, provider mix, cache efficiency, guardrail actions, and traces.',
      'Start here when something feels wrong. Cards can be reordered.',
    ],
    seeAlso: '/logs',
  },
  '/logs': {
    title: 'Logs',
    body: [
      'The audit trail: every recorded action, newest first.',
      'Click a row to expand the full-field detail modal.',
      'Answers “what did aimee do, when, and under whose identity”.',
    ],
    seeAlso: '/dashboard',
  },
  '/edit-workflows': {
    title: 'Edit Workflows',
    body: [
      'Define multi-step workflows — the design surface.',
      'Each step sets a task, a persona, a delegate, and a role.',
      'This is where the steps come from; you approve their runs next door.',
    ],
    seeAlso: '/workflow-actions',
  },
  '/workflow-actions': {
    title: 'Workflow Actions',
    body: [
      'The runtime queue: workflow items waiting on you to approve or reject.',
      'This is the human-in-the-loop gate.',
      'Edit Workflows is where these steps were defined.',
    ],
    seeAlso: '/edit-workflows',
  },
  '/agents': {
    title: 'Agents',
    body: [
      'Your delegates and their run history and stats.',
      'Edit a delegate’s persona and the roles it may use.',
      'Roles are the routing key matched between personas and agents.',
    ],
    seeAlso: '/personas',
  },
  '/personas': {
    title: 'Personas',
    body: [
      'The shared ROLE vocabulary plus the PERSONA definitions.',
      'Edit who aimee can be (identity + allowed roles) and what each role means.',
      'Agents binds these personas and roles to actual work.',
    ],
    seeAlso: '/agents',
  },
  '/projects': {
    title: 'Projects',
    body: [
      'Connect the git repositories aimee works on.',
      'Store per-host credentials (sealed vault) and run read + remote git ops.',
      'Credentials never reach the browser — they stay server-side.',
    ],
    seeAlso: '/editor',
  },
  '/graph': {
    title: 'Graph',
    body: [
      'A read-only explorer of the code-projection graph for the session’s project.',
      'Rank hubs, expand callers/callees/neighbours with provenance, spot “surprising links”.',
      'For understanding the codebase’s shape — off the agent’s hot path.',
    ],
    seeAlso: '/editor',
  },
  '/editor': {
    title: 'Editor',
    body: [
      'In-app VS Code, bound to the session’s isolated worktree.',
      'It’s the same tree the agent edits, so you see and hand-edit exactly what it sees.',
      'The editor’s port and git credentials stay server-side.',
    ],
    seeAlso: '/projects',
  },
  '/settings': {
    title: 'Settings',
    body: [
      'Every typed config option with plain-English help.',
      'Booleans are toggles, numbers are fields, strings are text; restart-sensitive keys are badged.',
      'This is the full control plane the setup wizard walks you through a slice of.',
    ],
    seeAlso: '/dashboard',
  },
};

/** Lookup helper — returns undefined for a route with no tutorial. */
export function tutorialFor(route: string): Tutorial | undefined {
  return TAB_TUTORIALS[route];
}
