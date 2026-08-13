/* Setup readiness — a PURE function over the values GET /api/config returns
 * (config.show), plus a couple of session/runtime signals passed in. It classifies
 * the minimum steps needed for a working turn so the header chip and the wizard
 * share one definition of "ready". No network, no DOM: unit-tested under vitest's
 * node env.
 *
 * The wizard forks on kb_mode: connecting to a REMOTE aimee-kb (kb_mode='remote')
 * deploys nothing locally, so the embedder + shared store (DB2) requirements fall
 * away — only the remote KB URL matters. A LOCAL knowledge base still needs a real
 * embedder and a DB2 store. The `connection` (git host) step is optional: public
 * repos clone without it.
 *
 * MVP scope: readiness is inferred client-side from config values, whether the
 * the workspace's cloned-project count, and how many git hosts are connected. A
 * server-side GET /api/setup/state that can additionally ping DB2/the provider is
 * a documented follow-up. */

import { FIELD_HELP } from '../pages/settingsHelp';

export type StepId = 'account' | 'provider' | 'knowledge_base' | 'embedding' | 'db2' | 'git_identity' | 'connection' | 'project';

/* The config keys readiness inspects. Exported so a test can assert each one is a
 * real, documented config field (a key rename in settingsHelp.ts that we miss
 * would otherwise silently break a rule). `connection` reads from the git-host
 * count, `project` from GET /api/git/projects, and `account` from
 * GET /api/setup/account — none has a config key, so they are absent here. */
export const READINESS_KEYS = [
  'provider',
  'embedder_command',
  'embedder_url',
  'embedder_model',
  'db2_url',
  'kb_mode',
  'kb_client_url',
] as const;

export interface StepStatus {
  ok: boolean;
  detail: string;
  /** Optional steps do not block overall readiness. */
  optional?: boolean;
}

export interface Readiness {
  ready: boolean;
  steps: Record<StepId, StepStatus>;
}

function asStr(cfg: Record<string, unknown>, key: string): string {
  const v = cfg[key];
  if (typeof v === 'string') return v.trim();
  if (v == null) return '';
  return String(v).trim();
}

export interface ReadinessSignals {
  accountReady: boolean;
  projectCount: number;
  hostsConnected?: number;
  gitIdentityReady?: boolean;
}

/** Classify the setup steps. Project readiness comes from the user's cloned
 * project inventory, not whichever chat tab happens to be active. */
export function computeReadiness(
  cfg: Record<string, unknown>,
  { accountReady, projectCount, hostsConnected = 0, gitIdentityReady = false }: ReadinessSignals,
): Readiness {
  const provider = asStr(cfg, 'provider');
  const remote = asStr(cfg, 'kb_mode') === 'remote';
  const kbUrl = asStr(cfg, 'kb_client_url');

  const embCmd = asStr(cfg, 'embedder_command');
  const embUrl = asStr(cfg, 'embedder_url');
  /* A bundled embedder is configured by NAMING it: the model is baked into the
   * image, so there is no endpoint to point at. This read llm_embed_backend ===
   * 'local', a selector that could disagree with the fields it gated. */
  const embModel = asStr(cfg, 'embedder_model');
  // The deploy-topology page places the embedder as a role (local container or
  // external), which also configures a real embedder.
  const embConfigured = embCmd !== '' || embUrl !== '' || embModel !== '';
  const db2 = asStr(cfg, 'db2_url');

  const steps: Record<StepId, StepStatus> = {
    account: {
      ok: accountReady,
      detail: accountReady ? 'login secured' : 'replace the development login',
    },
    provider: {
      ok: provider !== '',
      detail: provider !== '' ? `primary: ${provider}` : 'no primary provider set',
    },
    knowledge_base: remote
      ? {
          ok: kbUrl !== '',
          detail: kbUrl !== '' ? 'remote KB connected' : 'no remote KB URL set',
        }
      : { ok: true, detail: 'local knowledge base' },
    embedding: remote
      ? { ok: true, detail: 'n/a (remote KB)' }
      : {
          // A real embedder is a command, an endpoint, or a placed embed role; blank
          // means the built-in 384-dim hash fallback, which only works in a test setup.
          ok: embConfigured,
          detail: embConfigured ? 'embedder configured' : 'built-in hash fallback (test-only)',
        },
    // A local KB always has a Postgres store: the deploy stack spawns a bundled
    // one automatically (blank db2_url), or the operator points at an existing
    // database (db2_url set). Either way the step is satisfied — spawning your own
    // KB never requires a URL.
    db2: remote
      ? { ok: true, detail: 'n/a (remote KB)' }
      : {
          ok: true,
          detail: db2 !== '' ? 'existing database' : 'bundled Postgres',
        },
    git_identity: {
      ok: gitIdentityReady,
      detail: gitIdentityReady ? 'commit author stored in the Vault' : 'commits will be refused',
    },
    connection: {
      ok: hostsConnected > 0,
      detail:
        hostsConnected > 0
          ? `${hostsConnected} host${hostsConnected > 1 ? 's' : ''} connected`
          : 'no git host connected',
      optional: true,
    },
    project: {
      ok: projectCount > 0,
      detail: projectCount > 0
        ? `${projectCount} project${projectCount === 1 ? '' : 's'} cloned`
        : 'no projects cloned',
    },
  };

  const ready = (Object.values(steps) as StepStatus[]).every((s) => s.ok || s.optional);
  return { ready, steps };
}

/** How many REQUIRED steps are still incomplete (optional steps excluded). Drives
 * the "Setup — N left" chip; 0 ⇒ chip hidden. */
export function stepsRemaining(r: Readiness): number {
  return (Object.values(r.steps) as StepStatus[]).filter((s) => !s.ok && !s.optional).length;
}

/** The steps the operator has AFFIRMATIVELY completed — used by the wizard to
 * hide already-done sections on reopen. Deliberately stricter than
 * computeReadiness: a step that is merely satisfied-by-default (the local-KB
 * fork never visited, the bundled Postgres never chosen) is NOT completed, so a
 * first run still walks every step. */
export function completedSteps(
  cfg: Record<string, unknown>,
  { accountReady, projectCount, hostsConnected = 0, gitIdentityReady = false }: ReadinessSignals,
): Set<StepId> {
  const done = new Set<StepId>();
  if (accountReady) done.add('account');
  if (asStr(cfg, 'provider') !== '') done.add('provider');

  // The KB fork is complete once a mode was explicitly recorded ('' = never
  // visited); remote additionally needs the URL that makes the choice real.
  const kbMode = asStr(cfg, 'kb_mode');
  if (kbMode === 'local' || (kbMode === 'remote' && asStr(cfg, 'kb_client_url') !== '')) {
    done.add('knowledge_base');
  }

  const embConfigured =
    asStr(cfg, 'embedder_command') !== '' ||
    asStr(cfg, 'embedder_url') !== '' ||
    asStr(cfg, 'embedder_model') !== '';
  if (embConfigured) done.add('embedding');

  // A blank db2_url is ALSO the completed "bundled Postgres" choice, but blank
  // is equally the never-visited default. Treat the step as walked once the
  // local-deploy walk demonstrably happened: an explicit URL, or the embed role
  // placed in the step right before it.
  if (asStr(cfg, 'db2_url') !== '' || embConfigured) done.add('db2');

  if (hostsConnected > 0) done.add('connection');
  if (gitIdentityReady) done.add('git_identity');
  if (projectCount > 0) done.add('project');
  return done;
}

/** Guard used by the grounding test: every READINESS_KEYS entry must be a real
 * documented field. Kept here so the invariant lives next to the keys. */
export function readinessKeysAreDocumented(): boolean {
  return READINESS_KEYS.every((k) => k in FIELD_HELP);
}
