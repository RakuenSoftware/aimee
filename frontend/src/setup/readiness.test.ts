import { describe, it, expect } from 'vitest';
import { FIELD_HELP } from '../pages/settingsHelp';
import { completedSteps, computeReadiness, stepsRemaining, READINESS_KEYS, readinessKeysAreDocumented } from './readiness';

describe('readiness grounding', () => {
  it('every READINESS_KEYS entry is a documented config field', () => {
    for (const k of READINESS_KEYS) {
      expect(FIELD_HELP, `${k} missing from FIELD_HELP`).toHaveProperty(k);
    }
    expect(readinessKeysAreDocumented()).toBe(true);
  });
});

describe('computeReadiness (local KB path)', () => {
  it('an empty config with an unsecured login and no projects is not ready', () => {
    const r = computeReadiness({}, { accountReady: false, projectCount: 0 });
    expect(r.ready).toBe(false);
    expect(r.steps.account.ok).toBe(false);
    expect(r.steps.provider.ok).toBe(false);
    expect(r.steps.knowledge_base.ok).toBe(true); // local is the default; the fork is satisfied
    expect(r.steps.embedding.ok).toBe(false);
    // A local KB never blocks on DB2: a blank db2_url means the bundled Postgres.
    expect(r.steps.db2.ok).toBe(true);
    expect(r.steps.db2.detail).toMatch(/bundled/i);
    expect(r.steps.project.ok).toBe(false);
    expect(r.steps.connection.ok).toBe(false);
    expect(r.steps.connection.optional).toBe(true);
    expect(r.steps.git_identity.ok).toBe(false);
    // account, provider, embedding, git identity, and project are incomplete.
    expect(stepsRemaining(r)).toBe(5);
  });

  it('a db2_url reads as an existing database, still ok', () => {
    const r = computeReadiness({ db2_url: 'postgres://x' }, { accountReady: true, projectCount: 0 });
    expect(r.steps.db2.ok).toBe(true);
    expect(r.steps.db2.detail).toMatch(/existing database/i);
  });

  it('the built-in hash embedder (both keys blank) reads as not-ok, test-only', () => {
    const r = computeReadiness({ embedder_command: '', embedder_url: '   ' }, { accountReady: true, projectCount: 0 });
    expect(r.steps.embedding.ok).toBe(false);
    expect(r.steps.embedding.detail).toMatch(/hash fallback/i);
  });

  it('an embedding command OR endpoint satisfies embedding', () => {
    expect(computeReadiness({ embedder_command: 'embed.sh' }, { accountReady: true, projectCount: 0 }).steps.embedding.ok).toBe(true);
    expect(computeReadiness({ embedder_url: 'http://e' }, { accountReady: true, projectCount: 0 }).steps.embedding.ok).toBe(true);
  });

  it('a fully configured local instance with a project is ready', () => {
    const cfg = { provider: 'claude', embedder_url: 'http://e', db2_url: 'postgres://x' };
    const r = computeReadiness(cfg, { accountReady: true, projectCount: 1, gitIdentityReady: true });
    expect(r.ready).toBe(true);
    expect(stepsRemaining(r)).toBe(0);
  });

  it('a cloned project flips only the project step', () => {
    const base = { provider: 'claude', embedder_command: 'e.sh', db2_url: 'x' };
    expect(computeReadiness(base, { accountReady: true, projectCount: 0, gitIdentityReady: true }).ready).toBe(false);
    const ready = computeReadiness(base, { accountReady: true, projectCount: 19, gitIdentityReady: true });
    expect(ready.ready).toBe(true);
    expect(ready.steps.project.detail).toBe('19 projects cloned');
  });
});

describe('computeReadiness (remote KB path)', () => {
  it('remote KB satisfies embedding + db2 automatically; only the KB URL matters', () => {
    const cfg = { provider: 'claude', kb_mode: 'remote', kb_client_url: 'https://kb.example' };
    const r = computeReadiness(cfg, { accountReady: true, projectCount: 1, gitIdentityReady: true });
    expect(r.steps.knowledge_base.ok).toBe(true);
    expect(r.steps.embedding.ok).toBe(true);
    expect(r.steps.embedding.detail).toMatch(/n\/a/i);
    expect(r.steps.db2.ok).toBe(true);
    expect(r.ready).toBe(true);
  });

  it('remote with no KB URL blocks readiness on the knowledge_base step', () => {
    const cfg = { provider: 'claude', kb_mode: 'remote' };
    const r = computeReadiness(cfg, { accountReady: true, projectCount: 1, gitIdentityReady: true });
    expect(r.steps.knowledge_base.ok).toBe(false);
    expect(r.ready).toBe(false);
    expect(stepsRemaining(r)).toBe(1); // only knowledge_base is required-incomplete
  });
});

describe('computeReadiness (connection step)', () => {
  it('is optional and reflects the connected-host count without blocking ready', () => {
    const cfg = { provider: 'claude', embedder_command: 'e.sh', db2_url: 'x' };
    const none = computeReadiness(cfg, { accountReady: true, projectCount: 1, hostsConnected: 0, gitIdentityReady: true });
    expect(none.steps.connection.ok).toBe(false);
    expect(none.ready).toBe(true); // optional never blocks
    const two = computeReadiness(cfg, { accountReady: true, projectCount: 1, hostsConnected: 2, gitIdentityReady: true });
    expect(two.steps.connection.ok).toBe(true);
    expect(two.steps.connection.detail).toMatch(/2 hosts/);
  });
});

describe('completedSteps (affirmative completion — hides wizard sections on reopen)', () => {
  it('a fresh install has completed NOTHING, even steps readiness marks ok-by-default', () => {
    const done = completedSteps({}, { accountReady: false, projectCount: 0, hostsConnected: 0 });
    expect(done.size).toBe(0);
    // Contrast: readiness says knowledge_base + db2 are ok on the same input.
    const r = computeReadiness({}, { accountReady: false, projectCount: 0, hostsConnected: 0 });
    expect(r.steps.knowledge_base.ok).toBe(true);
    expect(r.steps.db2.ok).toBe(true);
  });

  it('each step completes on its affirmative signal', () => {
    expect(completedSteps({}, { accountReady: true, projectCount: 0 }).has('account')).toBe(true);
    expect(completedSteps({ provider: 'claude' }, { accountReady: false, projectCount: 0 }).has('provider')).toBe(true);
    expect(completedSteps({ kb_mode: 'local' }, { accountReady: false, projectCount: 0 }).has('knowledge_base')).toBe(true);
    expect(completedSteps({ embedder_model: 'bekko-a25m' }, { accountReady: false, projectCount: 0 }).has('embedding')).toBe(true);
    expect(completedSteps({}, { accountReady: false, projectCount: 0, hostsConnected: 1 }).has('connection')).toBe(true);
    expect(completedSteps({}, { accountReady: false, projectCount: 0, gitIdentityReady: true }).has('git_identity')).toBe(true);
    expect(completedSteps({}, { accountReady: false, projectCount: 19 }).has('project')).toBe(true);
  });

  it('remote KB completes only once the URL makes the choice real', () => {
    expect(completedSteps({ kb_mode: 'remote' }, { accountReady: false, projectCount: 0 }).has('knowledge_base')).toBe(false);
    expect(completedSteps({ kb_mode: 'remote', kb_client_url: 'https://kb' }, { accountReady: false, projectCount: 0 }).has('knowledge_base')).toBe(true);
  });

  it('db2 completes via an explicit URL or the deploy walk (embed role placed)', () => {
    expect(completedSteps({}, { accountReady: false, projectCount: 0 }).has('db2')).toBe(false);
    expect(completedSteps({ db2_url: 'postgres://x' }, { accountReady: false, projectCount: 0 }).has('db2')).toBe(true);
    expect(completedSteps({ embedder_url: 'https://emb.x' }, { accountReady: false, projectCount: 0 }).has('db2')).toBe(true);
  });

  it('a fully set-up instance completes every step', () => {
    const done = completedSteps(
      { provider: 'claude', kb_mode: 'local', embedder_model: 'bekko-a25m', db2_url: '' },
      { accountReady: true, projectCount: 19, hostsConnected: 1, gitIdentityReady: true },
    );
    expect(done.size).toBe(8);
  });
});
