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
  vi.stubGlobal('fetch', vi.fn(async (path, init) => {
    const body = JSON.parse(String(init?.body));
    return path === '/v1/memory/review' && body.store === 'kb'
      ? response([row('shared fixture', 'rejected')]) : response();
  }));
  render(<Memory />);
  await waitFor(() => expect(fetch).toHaveBeenCalledTimes(1));
  fireEvent.change(screen.getByLabelText('Memory store'), { target: { value: 'kb' } });
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'all' } });
  expect(await screen.findByText('shared fixture')).toBeTruthy();
  fireEvent.click(screen.getByRole('button', { name: 'Restore' }));
  await waitFor(() => expect(vi.mocked(fetch).mock.calls.some(([path]) => path === '/v1/memory/restore')).toBe(true));
  const restore = vi.mocked(fetch).mock.calls.find(([path]) => path === '/v1/memory/restore')!;
  expect(JSON.parse(String(restore[1]?.body)))
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

it('discards personal proposal responses after switching to the KB', async () => {
  let resolvePrivate!: (value: unknown) => void;
  vi.stubGlobal('fetch', vi.fn((path, init) => {
    const body = JSON.parse(String(init?.body));
    if (path === '/v1/memory/correction_proposals' && body.store === 'user') return new Promise((resolve) => { resolvePrivate = resolve; });
    return Promise.resolve({ ok: true, json: async () => ({ status: 'ok', memories: [], proposals: [] }) });
  }));
  render(<Memory />);
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'corrections' } });
  await waitFor(() => expect(resolvePrivate).toBeDefined());
  fireEvent.change(screen.getByLabelText('Memory store'), { target: { value: 'kb' } });
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'corrections' } });
  resolvePrivate({ ok: true, json: async () => ({ status: 'ok', proposals: [proposal] }) });
  await waitFor(() => expect(screen.queryByRole('button', { name: 'Inspect proposal-a' })).toBeNull());
});


it('displays exact proposed validity and processing before sending the unchanged decision', async () => {
  const metadata = {
    native_validity: { valid_from: '1792929600000000001', valid_until: '9223372036854775807' },
    native_required_jobs: {
      producer: { extraction: '<script>pipeline</script>', synthesis_model: 'model-reviewed', embedding_generation: 'generation-exact' },
      baseline: { entity: 'baseline-entity', field: 'document', selector: '',
        version: { commit: 'baseline-commit', operation: '9007199254740993', belief: '18446744073709551615' } },
    },
    native_inputs: [{ commit: 'input-commit', operation: '9007199254740995', belief: '18446744073709551614' }],
  };
  const detailed = { ...proposal, draft: { ...proposal.draft, ...metadata } };
  vi.stubGlobal('fetch', vi.fn(async (path) => ({ ok: true, json: async () => {
    if (path === '/v1/memory/correction_proposals') return { status: 'ok', proposals: [detailed] };
    if (path === '/v1/memory/get') return { status: 'ok', memory: { content: 'protected original', version } };
    return { status: 'ok', memories: [] };
  } })));
  render(<Memory />);
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'corrections' } });
  fireEvent.click(await screen.findByRole('button', { name: 'Inspect proposal-a' }));
  await screen.findByText('protected original');
  expect(screen.getByText(metadata.native_validity.valid_from)).toBeTruthy();
  expect(screen.getByText(metadata.native_validity.valid_until)).toBeTruthy();
  expect(screen.getByText('<script>pipeline</script>')).toBeTruthy();
  expect(document.querySelector('script')).toBeNull();
  const panel = screen.getByRole('region', { name: 'Proposed validity and processing' });
  expect(panel.textContent).toContain('18446744073709551615');
  expect(panel.textContent).toContain('9007199254740993');
  expect(panel.textContent).toContain('18446744073709551614');
  fireEvent.click(screen.getByRole('button', { name: 'Approve correction' }));
  await waitFor(() => expect(vi.mocked(fetch).mock.calls.some(([path]) => path === '/v1/memory/review_correction')).toBe(true));
  const decision = vi.mocked(fetch).mock.calls.find(([path]) => path === '/v1/memory/review_correction')!;
  expect(JSON.parse(String(decision[1]?.body))).toEqual({ store: 'user', proposal_id: proposal.proposal_id,
    payload_digest: proposal.payload_digest, expected_version: version, action: 'approve' });
});

it('distinguishes explicit unbounded/no-job plans from unavailable legacy details', async () => {
  const detailed = { ...proposal, draft: { ...proposal.draft, native_validity: null, native_required_jobs: null, native_inputs: [] } };
  vi.stubGlobal('fetch', vi.fn(async (path) => ({ ok: true, json: async () => {
    if (path === '/v1/memory/correction_proposals') return { status: 'ok', proposals: [detailed] };
    if (path === '/v1/memory/get') return { status: 'ok', memory: { content: 'protected original', version } };
    return { status: 'ok', memories: [] };
  } })));
  render(<Memory />);
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'corrections' } });
  fireEvent.click(await screen.findByRole('button', { name: 'Inspect proposal-a' }));
  await screen.findByText('protected original');
  expect(screen.getByText('No logical validity bounds.')).toBeTruthy();
  expect(screen.getByText('No required processing jobs.')).toBeTruthy();
  expect(screen.queryByText('This proposal format does not supply validity details.')).toBeNull();
});

it('does not invent an unbounded validity or empty processing plan for legacy proposals', async () => {
  vi.stubGlobal('fetch', proposalFetch());
  render(<Memory />);
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'corrections' } });
  fireEvent.click(await screen.findByRole('button', { name: 'Inspect proposal-a' }));
  await screen.findByText('protected original');
  expect(screen.getByText('This proposal format does not supply validity details.')).toBeTruthy();
  expect(screen.getByText('This proposal format does not supply processing details.')).toBeTruthy();
  expect(screen.queryByText('No logical validity bounds.')).toBeNull();
  expect(screen.queryByText('No required processing jobs.')).toBeNull();
});


it('preserves native wide row IDs and does not invent an update timestamp', async () => {
  const nativeRow = { ...row('exact native wide ID'), id: '9007199254740993', updated_at: undefined };
  vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(response([nativeRow])).mockResolvedValue(response()));
  render(<Memory />);
  expect(await screen.findByText('exact native wide ID')).toBeTruthy();
  expect(screen.getByText('user:memory:9007199254740993')).toBeTruthy();
  expect(screen.queryByText(/undefined|Invalid Date/)).toBeNull();
  fireEvent.click(screen.getByRole('button', { name: 'Retire' }));
  await waitFor(() => expect(fetch).toHaveBeenCalledTimes(3));
  const request = vi.mocked(fetch).mock.calls.find(([path]) => path === '/v1/memory/delete')!;
  expect(JSON.parse(String(request[1]?.body))).toMatchObject({ store: 'user', id: '9007199254740993' });
});


it('requests native eligibility before the page limit and refetches when changing views', async () => {
  const expired = Array.from({ length: 64 }, (_, index) => ({ ...row(`expired-${index}`, 'retained'), id: index + 100 }));
  const fetcher = vi.fn(async (_path, init) => {
    const body = JSON.parse(String(init?.body));
    return response(body.state === 'active' ? [row('older eligible fact')]
      : body.state === 'retired' ? [row('retired fact', 'retired')] : expired);
  });
  vi.stubGlobal('fetch', fetcher);
  render(<Memory />);
  expect(await screen.findByText('older eligible fact')).toBeTruthy();
  expect(JSON.parse(String(fetcher.mock.calls[0][1]?.body))).toMatchObject({ state: 'active', limit: 64, store: 'user' });
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'all' } });
  expect(await screen.findByText('expired-63')).toBeTruthy();
  expect(JSON.parse(String(fetcher.mock.calls[fetcher.mock.calls.length - 1]?.[1]?.body))).not.toHaveProperty('state');
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'retired' } });
  expect(await screen.findByText('retired fact')).toBeTruthy();
  expect(JSON.parse(String(fetcher.mock.calls[fetcher.mock.calls.length - 1]?.[1]?.body))).toMatchObject({ state: 'retired' });
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'active' } });
  expect(await screen.findByText('older eligible fact')).toBeTruthy();
  expect(screen.queryByText('retired fact')).toBeNull();
});


it('refreshes the current view after a mutation finishes in an older view', async () => {
  let deleted = false;
  let finishDelete!: () => void;
  const fetcher = vi.fn(async (path, init) => {
    const body = JSON.parse(String(init?.body));
    if (path === '/v1/memory/delete') return new Promise<ReturnType<typeof response>>((resolve) => {
      finishDelete = () => { deleted = true; resolve(response()); };
    });
    return response(body.state === 'retired'
      ? (deleted ? [row('retired after pending mutation', 'retired')] : [])
      : (deleted ? [] : [row('active before pending mutation')]));
  });
  vi.stubGlobal('fetch', fetcher);
  render(<Memory />);
  expect(await screen.findByText('active before pending mutation')).toBeTruthy();
  fireEvent.click(screen.getByRole('button', { name: 'Retire' }));
  await waitFor(() => expect(finishDelete).toBeDefined());
  fireEvent.change(screen.getByLabelText('Memory view'), { target: { value: 'retired' } });
  await waitFor(() => expect(fetcher.mock.calls.some(([path, init]) => path === '/v1/memory/review'
    && JSON.parse(String(init?.body)).state === 'retired')).toBe(true));
  finishDelete();
  expect(await screen.findByText('retired after pending mutation')).toBeTruthy();
  expect(JSON.parse(String(fetcher.mock.calls[fetcher.mock.calls.length - 1][1]?.body))).toMatchObject({ state: 'retired' });
});
