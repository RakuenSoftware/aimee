/* Ordered wizard step definitions + small helpers. Pure data/logic (no DOM), so
 * the ordering and restart-key classification are unit-tested. The step order is
 * the real dependency order for a working turn (provider → embedding → shared
 * store → optional KB API → project). Each step's config keys reuse the
 * plain-English copy in settingsHelp.ts (single source of truth). */

import { FIELD_HELP, RESTART_KEYS } from '../pages/settingsHelp';
import type { StepId } from './readiness';

export interface WizardStep {
  id: StepId;
  title: string;
  /** Config keys this step edits, in display order. Empty for a hand-off step. */
  keys: string[];
  /** Optional steps are skippable and never block "ready". */
  optional?: boolean;
  /** For a hand-off step (e.g. project), the route to send the operator to. */
  route?: string;
  /** One-line "what you lose if you skip", shown for optional steps. */
  skipNote?: string;
}

export const WIZARD_STEPS: WizardStep[] = [
  { id: 'provider', title: 'Primary provider', keys: ['provider', 'claude_model', 'openai_endpoint', 'openai_model', 'openai_key_cmd'] },
  { id: 'embedding', title: 'Embedding backend', keys: ['embedding_command', 'embedding_endpoint', 'embedding_model', 'embedding_dim'] },
  { id: 'db2', title: 'Shared store (DB2)', keys: ['db2_url'] },
  { id: 'kb_api', title: 'Knowledge-base API', keys: ['kb_api_http_port', 'kb_api_bearer_token'], optional: true, skipNote: 'Skipping leaves the KB REST API off — no external programmatic access to the knowledge base.' },
  { id: 'project', title: 'Connect a project', keys: [], route: '/projects', skipNote: 'Without a connected project, tools have no repository to act on.' },
];

/** True when a config key only takes effect after a server restart. */
export function isRestartKey(key: string): boolean {
  return RESTART_KEYS.has(key);
}

/** Plain-English help for a config key (blank if undocumented). */
export function helpFor(key: string): string {
  return FIELD_HELP[key] ?? '';
}
