/* Ordered wizard step definitions + small helpers. Pure data/logic (no DOM), so
 * the ordering and restart-key classification are unit-tested.
 *
 * The wizard forks on the knowledge-base choice (step 2): connecting to a REMOTE
 * aimee-kb deploys nothing locally, so the deploy-topology + shared-store (DB2)
 * steps are hidden. A step's `showWhen` predicate decides whether it appears for
 * the current kb_mode; a step with no predicate always shows. The tail is always
 * Connection (git hosts, optional) → Workspaces & projects. Each keyed step's
 * config keys reuse the plain-English copy in settingsHelp.ts (single source of
 * truth). */

import { FIELD_HELP, RESTART_KEYS } from '../pages/settingsHelp';
import type { StepId } from './readiness';

/** The knowledge-base mode that drives conditional step visibility. */
export type WizardKbMode = 'local' | 'remote';

export interface WizardStep {
  id: StepId;
  title: string;
  /** Config keys this step edits, in display order. Empty for a bespoke step. */
  keys: string[];
  /** Optional steps are skippable and never block "ready". */
  optional?: boolean;
  /** One-line "what you lose if you skip", shown for optional/hand-off steps. */
  skipNote?: string;
  /** A step whose body is a bespoke component rather than the generic key inputs:
   * 'chooser' = primary chooser, 'kb' = knowledge-base fork, 'deploy' = deploy
   * topology (LLM placement), 'db2' = shared-store (bundled vs existing Postgres),
   * 'connection' = git-host auth, 'workspace' = org enumerate + bulk clone.
   * Rendered specially by SetupWizard. */
  kind?: 'chooser' | 'kb' | 'deploy' | 'db2' | 'connection' | 'workspace';
  /** When present, the step is only shown for the kb modes it returns true for.
   * Absent ⇒ always shown. */
  showWhen?: (kbMode: WizardKbMode) => boolean;
}

export const WIZARD_STEPS: WizardStep[] = [
  { id: 'provider', title: 'Primary provider', keys: [], kind: 'chooser' },
  // Step 2 — the knowledge-base fork. Local deploys an aimee-kb here (needs the
  // deploy-topology + DB2 steps below); remote connects to an existing one and
  // skips all local infra.
  { id: 'knowledge_base', title: 'Knowledge base', keys: [], kind: 'kb' },
  // Local-only: LLM role placement for the deployed knowledge base.
  { id: 'embedding', title: 'Deploy topology', keys: [], kind: 'deploy', showWhen: (m) => m === 'local' },
  // Local-only: the shared Postgres (DB2) store the local KB writes to. A bespoke
  // step: spawning your own KB deploys a bundled Postgres automatically (no URL),
  // so db2_url is asked for only when pointing at an existing database.
  { id: 'db2', title: 'Shared store (DB2)', keys: [], kind: 'db2', showWhen: (m) => m === 'local' },
  // Always: authenticate to git hosts (OAuth / token / SSH). Optional — public
  // repos clone without it.
  { id: 'connection', title: 'Connection', keys: [], kind: 'connection', optional: true, skipNote: 'Skipping leaves no git host connected — you can still clone public repos and connect private hosts later.' },
  // Always: point at an owner/org, list its repos, and bulk-clone into the workspace.
  { id: 'project', title: 'Workspaces & projects', keys: [], kind: 'workspace', skipNote: 'Without a connected repo, tools have no repository to act on.' },
];

/** Infra steps the all-in-one appliance bakes (KB + LLM + shared store), so its
 * wizard hides them and only asks for the provider, git connection, and
 * workspaces. */
export const APPLIANCE_HIDDEN_STEPS: ReadonlySet<StepId> = new Set<StepId>([
  'knowledge_base',
  'embedding',
  'db2',
]);

/** The steps visible for the given kb mode, in order (drives the wizard's Step
 * N of M and next/back navigation). In `appliance` mode the baked-infra steps are
 * hidden regardless of kb mode. */
export function visibleSteps(kbMode: WizardKbMode, appliance = false): WizardStep[] {
  return WIZARD_STEPS.filter((s) => {
    if (appliance && APPLIANCE_HIDDEN_STEPS.has(s.id)) return false;
    return !s.showWhen || s.showWhen(kbMode);
  });
}

/** True when a config key only takes effect after a server restart. */
export function isRestartKey(key: string): boolean {
  return RESTART_KEYS.has(key);
}

/** Plain-English help for a config key (blank if undocumented). */
export function helpFor(key: string): string {
  return FIELD_HELP[key] ?? '';
}
