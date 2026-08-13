import { afterEach, describe, expect, it, vi } from 'vitest';
import { fetchGitIdentityReady } from './setupSignals';

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
