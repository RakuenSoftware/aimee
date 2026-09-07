/* Single-user setup. Model services belong to Server; KB connection is optional settings. */

import { FIELD_HELP, RESTART_KEYS } from '../pages/settingsHelp';
import type { StepId } from './readiness';

/** The knowledge-base mode that drives conditional step visibility. */
export type WizardKbMode = 'none' | 'local' | 'remote';

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
   * 'account' = replacement login, 'chooser' = primary chooser, 'kb' = knowledge-base fork, 'deploy' = deploy
   * topology (LLM placement), 'db2' = shared-store (bundled vs existing Postgres),
   * 'git_identity' = vaulted commit author, 'connection' = git-host auth,
   * 'workspace' = org enumerate + bulk clone.
   * Rendered specially by SetupWizard. */
  kind?: 'account' | 'chooser' | 'deploy' | 'git_identity' | 'connection' | 'workspace';
  /** When present, the step is only shown for the kb modes it returns true for.
   * Absent ⇒ always shown. */
  showWhen?: (kbMode: WizardKbMode) => boolean;
}

/** Bespoke steps render and own their primary action. Keeping this exhaustive
 * next to the kind union prevents SetupWizard from accidentally adding a
 * generic Next button that bypasses a newly-added required step. */
export function ownsPrimaryAction(step: WizardStep): boolean {
  return step.kind !== undefined;
}

export const WIZARD_STEPS: WizardStep[] = [
  { id: 'account', title: 'Secure your account', keys: [], kind: 'account' },
  { id: 'provider', title: 'Primary provider', keys: [], kind: 'chooser' },
  { id: 'embedding', title: 'Local memory models', keys: [], kind: 'deploy' },
  { id: 'git_identity', title: 'Git commit identity', keys: [], kind: 'git_identity', skipNote: 'Without it, every commit is refused rather than attributed to an invented author.' },
  // Always: authenticate to git hosts (OAuth / token / SSH). Optional — public
  // repos clone without it.
  { id: 'connection', title: 'Connection', keys: [], kind: 'connection', optional: true, skipNote: 'Skipping leaves no git host connected — you can still clone public repos and connect private hosts later.' },
  // Always: point at an owner/org, list its repos, and bulk-clone into the workspace.
  { id: 'project', title: 'Workspaces & projects', keys: [], kind: 'workspace', skipNote: 'Without a connected repo, tools have no repository to act on.' },
];

/** Models remain configurable even on a prebuilt appliance. */
export const APPLIANCE_HIDDEN_STEPS: ReadonlySet<StepId> = new Set<StepId>();

/** KB connection never changes the single-user setup path. */
export function visibleSteps(_kbMode: WizardKbMode = 'none', _appliance = false): WizardStep[] {
  return WIZARD_STEPS;
}

/** True when a config key only takes effect after a server restart. */
export function isRestartKey(key: string): boolean {
  return RESTART_KEYS.has(key);
}

/** Plain-English help for a config key (blank if undocumented). */
export function helpFor(key: string): string {
  return FIELD_HELP[key] ?? '';
}
