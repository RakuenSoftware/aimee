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
 * active session has a project bound, and how many git hosts are connected. A
 * server-side GET /api/setup/state that can additionally ping DB2/the provider is
 * a documented follow-up. */

import { FIELD_HELP } from '../pages/settingsHelp';

export type StepId = 'provider' | 'knowledge_base' | 'embedding' | 'db2' | 'connection' | 'project';

/* The config keys readiness inspects. Exported so a test can assert each one is a
 * real, documented config field (a key rename in settingsHelp.ts that we miss
 * would otherwise silently break a rule). `connection` reads from the git-host
 * count and `project` from the session bundle — neither has a config key, so both
 * are deliberately absent here. */
export const READINESS_KEYS = [
  'provider',
  'embedding_command',
  'embedding_endpoint',
  'llm_embed_backend',
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

/** Classify the setup steps. `hasProject` comes from the active session (the
 * config has no project field); `hostsConnected` is how many git hosts have a
 * stored credential (GET /api/git/credentials). When kb_mode is 'remote', the
 * local embedder + DB2 steps are satisfied by connecting the remote KB. */
export function computeReadiness(
  cfg: Record<string, unknown>,
  hasProject: boolean,
  hostsConnected = 0,
): Readiness {
  const provider = asStr(cfg, 'provider');
  const remote = asStr(cfg, 'kb_mode') === 'remote';
  const kbUrl = asStr(cfg, 'kb_client_url');

  const embCmd = asStr(cfg, 'embedding_command');
  const embEndpoint = asStr(cfg, 'embedding_endpoint');
  // The deploy-topology page places the embedder as a role (local container or
  // external), which also configures a real embedder.
  const embBackend = asStr(cfg, 'llm_embed_backend');
  const embConfigured = embCmd !== '' || embEndpoint !== '' || embBackend === 'local' || embBackend === 'external';
  const db2 = asStr(cfg, 'db2_url');

  const steps: Record<StepId, StepStatus> = {
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
    connection: {
      ok: hostsConnected > 0,
      detail:
        hostsConnected > 0
          ? `${hostsConnected} host${hostsConnected > 1 ? 's' : ''} connected`
          : 'no git host connected',
      optional: true,
    },
    project: {
      ok: hasProject,
      detail: hasProject ? 'project connected' : 'no project connected',
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
  hasProject: boolean,
  hostsConnected = 0,
): Set<StepId> {
  const done = new Set<StepId>();
  if (asStr(cfg, 'provider') !== '') done.add('provider');

  // The KB fork is complete once a mode was explicitly recorded ('' = never
  // visited); remote additionally needs the URL that makes the choice real.
  const kbMode = asStr(cfg, 'kb_mode');
  if (kbMode === 'local' || (kbMode === 'remote' && asStr(cfg, 'kb_client_url') !== '')) {
    done.add('knowledge_base');
  }

  const embConfigured =
    asStr(cfg, 'embedding_command') !== '' ||
    asStr(cfg, 'embedding_endpoint') !== '' ||
    asStr(cfg, 'llm_embed_backend') === 'local' ||
    asStr(cfg, 'llm_embed_backend') === 'external';
  if (embConfigured) done.add('embedding');

  // A blank db2_url is ALSO the completed "bundled Postgres" choice, but blank
  // is equally the never-visited default. Treat the step as walked once the
  // local-deploy walk demonstrably happened: an explicit URL, or the embed role
  // placed in the step right before it.
  if (asStr(cfg, 'db2_url') !== '' || embConfigured) done.add('db2');

  if (hostsConnected > 0) done.add('connection');
  if (hasProject) done.add('project');
  return done;
}

/** Guard used by the grounding test: every READINESS_KEYS entry must be a real
 * documented field. Kept here so the invariant lives next to the keys. */
export function readinessKeysAreDocumented(): boolean {
  return READINESS_KEYS.every((k) => k in FIELD_HELP);
}
