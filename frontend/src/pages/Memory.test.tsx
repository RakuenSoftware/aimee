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

const version = { schema_version: 1, owner_id: 'owner', record_id: '9007199254740993', record_revision: '9007199254740995' };
const proposal = { proposal_id: 'proposal-a', target_version: version, payload_digest: 'digest-a', state: 'pending', proposer: 'model',
  draft: { content: '<script>proposed</script>', tier: 'L2', confidence: 0.5, use_cases: 'testing', epistemic_kind: 'fact' } };
function proposalFetch(currentVersion = version, failReview = false) {
  return vi.fn(async (path: RequestInfo | URL) => ({ ok: !(failReview && path === '/v1/memory/review_correction'), json: async () => {
    if (path === '/v1/memory/correction_proposals') return { status: 'ok', proposals: [proposal] };
    if (path === '/v1/memory/get') return { status: 'ok', memory: { content: 'protected original', version: currentVersion } };
    if (failReview && path === '/v1/memory/review_correction') return { status: 'error', message: 'revision conflict' };
    return { status: 'ok', memories: [] };
  } }));
}

it.each(['user', 'kb'])('reviews corrections with exact versions and explicit %s placement', async (store) => {
  vi.stubGlobal('fetch', proposalFetch());
  render(<Memory />);
  if (store === 'kb') fireEvent.change(screen.getByLabelText('Memory store'), { target: { value: store } });
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'corrections' } });
  fireEvent.click(await screen.findByRole('button', { name: 'Inspect proposal-a' }));
  expect(await screen.findByText('protected original')).toBeTruthy();
  expect(screen.getByText('<script>proposed</script>')).toBeTruthy();
  fireEvent.click(screen.getByRole('button', { name: 'Approve correction' }));
  await waitFor(() => expect(vi.mocked(fetch).mock.calls.some(([p]) => p === '/v1/memory/review_correction')).toBe(true));
  const call = vi.mocked(fetch).mock.calls.find(([p]) => p === '/v1/memory/review_correction')!;
  expect(JSON.parse(String(call[1]?.body))).toEqual({ store, ...(store === 'kb' ? { cwd: '/work/project-a' } : {}),
    proposal_id: 'proposal-a', payload_digest: 'digest-a', expected_version: version, action: 'approve' });
  expect(call[1]?.headers).toMatchObject({ 'X-CSRF-Token': 'csrf-test' });
  const read = vi.mocked(fetch).mock.calls.find(([p]) => p === '/v1/memory/get')!;
  expect(JSON.parse(String(read[1]?.body)).id).toBe(version.record_id);
});

it('disables decisions on stale proposals', async () => {
  vi.stubGlobal('fetch', proposalFetch({ ...version, record_revision: '9007199254740996' }));
  render(<Memory />);
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'corrections' } });
  fireEvent.click(await screen.findByRole('button', { name: 'Inspect proposal-a' }));
  await screen.findByText('protected original');
  expect((screen.getByRole('button', { name: 'Approve correction' }) as HTMLButtonElement).disabled).toBe(true);
  expect(screen.getByRole('alert').textContent).toContain('source revision has changed');
});

it('shows backend conflicts without claiming approval', async () => {
  vi.stubGlobal('fetch', proposalFetch(version, true));
  render(<Memory />);
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'corrections' } });
  fireEvent.click(await screen.findByRole('button', { name: 'Inspect proposal-a' }));
  await screen.findByText('protected original');
  fireEvent.click(screen.getByRole('button', { name: 'Approve correction' }));
  expect((await screen.findByRole('alert')).textContent).toContain('revision conflict');
  expect(screen.getByText('protected original')).toBeTruthy();
});
