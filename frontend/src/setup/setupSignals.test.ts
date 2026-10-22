import { afterEach, describe, expect, it, vi } from 'vitest';
import { fetchGitIdentityReady, fetchSetupAccountStatus } from './setupSignals';

afterEach(() => vi.unstubAllGlobals());

function vaultList(credentials: unknown[]) {
  vi.stubGlobal('fetch', vi.fn(async () => ({
    ok: true,
    json: async () => ({ status: 'ok', credentials }),
  })));
}

describe('Git identity readiness signal', () => {
  it('is ready only when both canonical Git credential names are present', async () => {
    vaultList([
      { agent: 'git', cred: 'author_name' },
      { agent: 'git', cred: 'author_email' },
    ]);
    expect(await fetchGitIdentityReady()).toBe(true);
  });

  it('rejects a partial identity', async () => {
    vaultList([{ agent: 'git', cred: 'author_name' }]);
    expect(await fetchGitIdentityReady()).toBe(false);
  });

  it('ignores absent and non-Git credential pairs', async () => {
    vaultList([
      { agent: 'claude', cred: 'author_name' },
      { agent: 'git', cred: 'api_key' },
    ]);
    expect(await fetchGitIdentityReady()).toBe(false);
  });
});

it('keeps authenticated account readiness independent of its dismissal scope', async () => {
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, json: async () => ({ complete: false, dismissal_scope: 'a'.repeat(64) }) })));
  expect(await fetchSetupAccountStatus()).toEqual({ complete: false, dismissal_scope: 'a'.repeat(64) });
});
it('ignores malformed or unauthenticated dismissal scopes without inventing completion', async () => {
  for (const ok of [false, true]) for (const scope of ['a'.repeat(63), 'A'.repeat(64), 1, null]) {
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok, json: async () => ({ complete: true, dismissal_scope: scope }) })));
    expect(await fetchSetupAccountStatus()).toEqual({ complete: ok, dismissal_scope: undefined });
  }
});
