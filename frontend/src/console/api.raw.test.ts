/** @vitest-environment jsdom */
import { afterEach, describe, expect, it, vi } from 'vitest';
import { apiSendRaw, setCsrf } from './api';
afterEach(() => vi.unstubAllGlobals());
describe('raw native administration transport', () => {
  it('retains session/CSRF protection and never serializes native number tokens', async () => {
    const body = '{"owner":"owner","request":{"operation":18446744073709551615}}';
    const text = '{"version":18446744073709551615}';
    const fetcher = vi.fn().mockResolvedValue({ status: 409, ok: false, text: async () => text });
    vi.stubGlobal('fetch', fetcher);
    setCsrf('session-csrf');
    expect(await apiSendRaw('POST', '/v1/console/memory', body)).toEqual({ status: 409, ok: false, text });
    expect(fetcher).toHaveBeenCalledWith('/api/v1/console/memory', {
      method: 'POST', credentials: 'same-origin',
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': 'session-csrf' }, body,
    });
  });
});
