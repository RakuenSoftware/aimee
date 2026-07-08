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

describe('computeReadiness', () => {
  it('an empty config with no project → all required steps red, not ready', () => {
    const r = computeReadiness({}, false);
    expect(r.ready).toBe(false);
    expect(r.steps.provider.ok).toBe(false);
    expect(r.steps.embedding.ok).toBe(false);
    expect(r.steps.db2.ok).toBe(false);
    expect(r.steps.project.ok).toBe(false);
    expect(r.steps.kb_api.ok).toBe(false);
    expect(r.steps.kb_api.optional).toBe(true);
    // provider, embedding, db2, project are the 4 required-incomplete steps.
    expect(stepsRemaining(r)).toBe(4);
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

  it('kb_api is optional: unset does not block readiness', () => {
    const cfg = { provider: 'claude', embedding_command: 'e.sh', db2_url: 'postgres://x' };
    const r = computeReadiness(cfg, true); // kb_api unset (port 0)
    expect(r.steps.kb_api.ok).toBe(false);
    expect(r.ready).toBe(true);
    expect(stepsRemaining(r)).toBe(0);
  });

  it('a fully configured instance with a project is ready', () => {
    const cfg = { provider: 'claude', embedding_endpoint: 'http://e', db2_url: 'postgres://x', kb_api_http_port: 8741 };
    const r = computeReadiness(cfg, true);
    expect(r.ready).toBe(true);
    expect(r.steps.kb_api.ok).toBe(true);
    expect(stepsRemaining(r)).toBe(0);
  });

  it('kb_api_http_port coerces from a string and 0 stays disabled', () => {
    expect(computeReadiness({ kb_api_http_port: '8741' }, false).steps.kb_api.ok).toBe(true);
    expect(computeReadiness({ kb_api_http_port: '0' }, false).steps.kb_api.ok).toBe(false);
  });

  it('a connected project flips only the project step', () => {
    const base = { provider: 'claude', embedding_command: 'e.sh', db2_url: 'x' };
    expect(computeReadiness(base, false).ready).toBe(false);
    expect(computeReadiness(base, true).ready).toBe(true);
  });
});
