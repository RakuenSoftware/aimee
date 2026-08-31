/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import Memory from './Memory';

vi.mock('../SessionContext', () => ({
  useSessions: () => ({ active: { projectRoot: '/work/project-a' } }),
}));

const rejected = {
  id: 9, tier: 'L2', kind: 'fact', key: 'release:day', content: 'release on Friday',
  confidence: 0, lifecycle: 'rejected', review_reason: 'stale', scope_type: 'project',
  scope_value: 'project-a', updated_at: '2026-01-02',
};

beforeEach(() => {
  window._csrf = 'csrf-test';
  vi.stubGlobal('fetch', vi.fn()
    .mockResolvedValueOnce({ ok: true, json: async () => ({ memories: [rejected] }) })
    .mockResolvedValueOnce({ ok: true, json: async () => ({ status: 'ok' }) })
    .mockResolvedValueOnce({ ok: true, json: async () => ({ memories: [] }) }));
});
afterEach(() => { cleanup(); vi.unstubAllGlobals(); });

describe('project memory center', () => {
  it('scopes review and restore requests to the active project', async () => {
    render(<Memory />);
    expect(await screen.findByText('release on Friday')).toBeTruthy();
    fireEvent.click(screen.getByRole('button', { name: 'Restore' }));
    await waitFor(() => expect(fetch).toHaveBeenCalledTimes(3));
    const reviewBody = JSON.parse(String(vi.mocked(fetch).mock.calls[0][1]?.body));
    const restoreBody = JSON.parse(String(vi.mocked(fetch).mock.calls[1][1]?.body));
    expect(reviewBody.cwd).toBe('/work/project-a');
    expect(restoreBody).toMatchObject({ cwd: '/work/project-a', id: 9 });
  });
});
