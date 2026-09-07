/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import Memory from './Memory';

vi.mock('../SessionContext', () => ({ useSessions: () => ({ active: { projectRoot: '/work/project-a' } }) }));
const row = (content: string, lifecycle = 'active') => ({
  id: 9, tier: 'L2', kind: 'fact', key: 'fixture', content, confidence: 1,
  lifecycle, review_reason: '', scope_type: 'user', scope_value: '_user', updated_at: '2026-09-06',
});
const response = (memories: unknown[] = []) => ({ ok: true, json: async () => ({ status: 'ok', memories }) });
beforeEach(() => { window._csrf = 'csrf-test'; });
afterEach(() => { cleanup(); vi.unstubAllGlobals(); });

it('keeps default review and retirement in the local user store', async () => {
  vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(response([row('private fixture')])).mockResolvedValue(response()));
  render(<Memory />);
  expect(await screen.findByText('private fixture')).toBeTruthy();
  fireEvent.click(screen.getByRole('button', { name: 'Retire' }));
  await waitFor(() => expect(fetch).toHaveBeenCalledTimes(3));
  const calls = vi.mocked(fetch).mock.calls;
  expect(JSON.parse(String(calls[0][1]?.body))).toMatchObject({ store: 'user' });
  expect(JSON.parse(String(calls[0][1]?.body))).not.toHaveProperty('cwd');
  expect(calls[1][0]).toBe('/v1/memory/delete');
  expect(JSON.parse(String(calls[1][1]?.body))).toMatchObject({ store: 'user', id: 9 });
});

it('requires explicit KB selection and carries it through restoration', async () => {
  vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(response())
    .mockResolvedValueOnce(response([row('shared fixture', 'rejected')])).mockResolvedValue(response()));
  render(<Memory />);
  await waitFor(() => expect(fetch).toHaveBeenCalledTimes(1));
  fireEvent.change(screen.getByLabelText('Memory store'), { target: { value: 'kb' } });
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'all' } });
  expect(await screen.findByText('shared fixture')).toBeTruthy();
  fireEvent.click(screen.getByRole('button', { name: 'Restore' }));
  await waitFor(() => expect(fetch).toHaveBeenCalledTimes(4));
  expect(JSON.parse(String(vi.mocked(fetch).mock.calls[2][1]?.body)))
    .toMatchObject({ store: 'kb', cwd: '/work/project-a', id: 9 });
});

it('discards an old response when switching stores with colliding IDs', async () => {
  let resolveOld!: (value: unknown) => void;
  vi.stubGlobal('fetch', vi.fn().mockReturnValueOnce(new Promise((resolve) => { resolveOld = resolve; }))
    .mockResolvedValue(response([row('shared fixture')])));
  render(<Memory />);
  fireEvent.change(screen.getByLabelText('Memory store'), { target: { value: 'kb' } });
  expect(await screen.findByText('shared fixture')).toBeTruthy();
  resolveOld(response([row('private fixture')]));
  await waitFor(() => expect(screen.queryByText('private fixture')).toBeNull());
  expect(screen.getByText('shared fixture')).toBeTruthy();
});
