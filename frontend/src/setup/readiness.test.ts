import { describe, it, expect } from 'vitest';
import { FIELD_HELP } from '../pages/settingsHelp';
import { computeReadiness, stepsRemaining, READINESS_KEYS, readinessKeysAreDocumented } from './readiness';

describe('readiness grounding', () => {
  it('every READINESS_KEYS entry is a documented config field', () => {
    for (const k of READINESS_KEYS) {
      expect(FIELD_HELP, `${k} missing from FIELD_HELP`).toHaveProperty(k);
    }
    expect(readinessKeysAreDocumented()).toBe(true);
  });
});

describe('computeReadiness (local KB path)', () => {
  it('an empty config with no project → provider/embedding/project red, not ready', () => {
    const r = computeReadiness({}, false);
    expect(r.ready).toBe(false);
    expect(r.steps.provider.ok).toBe(false);
    expect(r.steps.knowledge_base.ok).toBe(true); // local is the default; the fork is satisfied
    expect(r.steps.embedding.ok).toBe(false);
    // A local KB never blocks on DB2: a blank db2_url means the bundled Postgres.
    expect(r.steps.db2.ok).toBe(true);
    expect(r.steps.db2.detail).toMatch(/bundled/i);
    expect(r.steps.project.ok).toBe(false);
    expect(r.steps.connection.ok).toBe(false);
    expect(r.steps.connection.optional).toBe(true);
    // provider, embedding, project are the 3 required-incomplete steps (knowledge_base
    // + db2 are ok, connection is optional).
    expect(stepsRemaining(r)).toBe(3);
  });

  it('a db2_url reads as an existing database, still ok', () => {
    const r = computeReadiness({ db2_url: 'postgres://x' }, false);
    expect(r.steps.db2.ok).toBe(true);
    expect(r.steps.db2.detail).toMatch(/existing database/i);
  });

  it('the built-in hash embedder (both keys blank) reads as not-ok, test-only', () => {
    const r = computeReadiness({ embedding_command: '', embedding_endpoint: '   ' }, false);
    expect(r.steps.embedding.ok).toBe(false);
    expect(r.steps.embedding.detail).toMatch(/hash fallback/i);
  });

  it('an embedding command OR endpoint satisfies embedding', () => {
    expect(computeReadiness({ embedding_command: 'embed.sh' }, false).steps.embedding.ok).toBe(true);
    expect(computeReadiness({ embedding_endpoint: 'http://e' }, false).steps.embedding.ok).toBe(true);
  });

  it('a fully configured local instance with a project is ready', () => {
    const cfg = { provider: 'claude', embedding_endpoint: 'http://e', db2_url: 'postgres://x' };
    const r = computeReadiness(cfg, true);
    expect(r.ready).toBe(true);
    expect(stepsRemaining(r)).toBe(0);
  });

  it('a connected project flips only the project step', () => {
    const base = { provider: 'claude', embedding_command: 'e.sh', db2_url: 'x' };
    expect(computeReadiness(base, false).ready).toBe(false);
    expect(computeReadiness(base, true).ready).toBe(true);
  });
});

describe('computeReadiness (remote KB path)', () => {
  it('remote KB satisfies embedding + db2 automatically; only the KB URL matters', () => {
    const cfg = { provider: 'claude', kb_mode: 'remote', kb_client_url: 'https://kb.example' };
    const r = computeReadiness(cfg, true);
    expect(r.steps.knowledge_base.ok).toBe(true);
    expect(r.steps.embedding.ok).toBe(true);
    expect(r.steps.embedding.detail).toMatch(/n\/a/i);
    expect(r.steps.db2.ok).toBe(true);
    expect(r.ready).toBe(true);
  });

  it('remote with no KB URL blocks readiness on the knowledge_base step', () => {
    const cfg = { provider: 'claude', kb_mode: 'remote' };
    const r = computeReadiness(cfg, true);
    expect(r.steps.knowledge_base.ok).toBe(false);
    expect(r.ready).toBe(false);
    expect(stepsRemaining(r)).toBe(1); // only knowledge_base is required-incomplete
  });
});

describe('computeReadiness (connection step)', () => {
  it('is optional and reflects the connected-host count without blocking ready', () => {
    const cfg = { provider: 'claude', embedding_command: 'e.sh', db2_url: 'x' };
    const none = computeReadiness(cfg, true, 0);
    expect(none.steps.connection.ok).toBe(false);
    expect(none.ready).toBe(true); // optional never blocks
    const two = computeReadiness(cfg, true, 2);
    expect(two.steps.connection.ok).toBe(true);
    expect(two.steps.connection.detail).toMatch(/2 hosts/);
  });
});
