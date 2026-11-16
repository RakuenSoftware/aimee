/** @vitest-environment jsdom */
import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import TypedFacts from './TypedFacts';
import { apiGet, apiSend } from '../api';
vi.mock('../api', () => ({ apiGet: vi.fn(), apiSend: vi.fn() }));
function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (reason: Error) => void;
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
const preview = (id: string) => ({ commit_id: id, changes: [{ object_kind: 'fact', object_key: id, action: 'assert', before: '', after: 'candidate', detail: '' }] });
beforeEach(() => {
  vi.mocked(apiGet).mockResolvedValue({ config: {}, promotion_candidates: [], assertions: [], merges: [] });
  vi.mocked(apiSend).mockImplementation(async (_method, path) => path === '/v1/console/evidence' ? { items: [], changeset_head: '' } : {});
  vi.spyOn(window, 'confirm').mockReturnValue(true);
});
afterEach(() => { cleanup(); vi.restoreAllMocks(); vi.resetAllMocks(); });
async function mount() {
  render(<TypedFacts />);
  return screen.findByLabelText('Commit ID');
}
function rollback() { return screen.getByRole('button', { name: 'rollback previewed changes' }) as HTMLButtonElement; }
it('discards preview A arriving after the selector changed to B and only rolls back a new B preview', async () => {
  const pending = deferred<ReturnType<typeof preview>>();
  vi.mocked(apiSend).mockImplementation(async (_method, path, body) => {
    if (path === '/v1/console/evidence') return { items: [], changeset_head: '' };
    const request = body as { action: string; commit_id: string };
    if (request.action === 'preview') return request.commit_id === 'A' ? pending.promise : preview('B');
    return {};
  });
  const input = await mount();
  fireEvent.change(input, { target: { value: 'A' } });
  fireEvent.click(screen.getByRole('button', { name: 'preview diff' }));
  fireEvent.change(input, { target: { value: 'B' } });
  await act(async () => pending.resolve(preview('A')));
  expect(rollback().disabled).toBe(true);
  expect(screen.queryByText('fact:A')).toBeNull();
  fireEvent.click(rollback());
  expect(apiSend).not.toHaveBeenCalledWith('POST', '/v1/console/typed_facts/commit', { action: 'rollback', commit_id: 'B' });
  fireEvent.click(screen.getByRole('button', { name: 'preview diff' }));
  await waitFor(() => expect(rollback().disabled).toBe(false));
  fireEvent.click(rollback());
  await waitFor(() => expect(apiSend).toHaveBeenCalledWith('POST', '/v1/console/typed_facts/commit', { action: 'rollback', commit_id: 'B' }));
});
it('does not accept an old request when the selector changes away and back to the same value', async () => {
  const first = deferred<ReturnType<typeof preview>>();
  const second = deferred<ReturnType<typeof preview>>();
  let requests = 0;
  vi.mocked(apiSend).mockImplementation(async (_method, path) => path === '/v1/console/evidence' ? { items: [], changeset_head: '' } : (++requests === 1 ? first.promise : second.promise));
  const input = await mount();
  fireEvent.change(input, { target: { value: 'A' } });
  fireEvent.click(screen.getByRole('button', { name: 'preview diff' }));
  fireEvent.change(input, { target: { value: 'B' } });
  fireEvent.change(input, { target: { value: 'A' } });
  fireEvent.click(screen.getByRole('button', { name: 'preview diff' }));
  await act(async () => first.resolve(preview('A')));
  expect(rollback().disabled).toBe(true);
  expect((screen.getByRole('button', { name: 'preview diff' }) as HTMLButtonElement).disabled).toBe(true);
  await act(async () => second.resolve(preview('A')));
  expect(rollback().disabled).toBe(false);
});
it('ignores an obsolete preview error after switching to an ingest-run preview', async () => {
  const old = deferred<ReturnType<typeof preview>>();
  vi.mocked(apiSend).mockImplementation(async (_method, path, body) => {
    if (path === '/v1/console/evidence') return { items: [], changeset_head: '' };
    return (body as { commit_id?: string }).commit_id ? old.promise : { ingest_run_id: 'run:B', changes: [] };
  });
  const input = await mount();
  fireEvent.change(input, { target: { value: 'A' } });
  fireEvent.click(screen.getByRole('button', { name: 'preview diff' }));
  fireEvent.change(input, { target: { value: '' } });
  fireEvent.change(screen.getByLabelText('Ingest run ID'), { target: { value: 'run:B' } });
  fireEvent.click(screen.getByRole('button', { name: 'preview diff' }));
  await waitFor(() => expect(rollback().disabled).toBe(false));
  await act(async () => old.reject(new Error('obsolete request failed')));
  expect(screen.queryByText(/obsolete request failed/)).toBeNull();
  expect(rollback().disabled).toBe(false);
  fireEvent.click(rollback());
  await waitFor(() => expect(apiSend).toHaveBeenCalledWith('POST', '/v1/console/typed_facts/commit', { action: 'rollback', ingest_run_id: 'run:B' }));
});
