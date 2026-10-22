/** @vitest-environment jsdom */
import { act, cleanup, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import Editor from './Editor';
const state = vi.hoisted(() => ({ active: { id: 'tab', aimeeSid: 'web-one', projectRoot: '/work/octocat/Hello-World', projectName: 'Hello-World' }, patchSession: vi.fn() }));
vi.mock('../SessionContext', () => ({ useSessions: () => state }));
const registry = { root: '/work', projects: ['octocat/Hello-World', 'another/Hello-World'] };
const json = (data: unknown, status = 200) => new Response(JSON.stringify(data), { status });
beforeEach(() => { state.active = { id: 'tab', aimeeSid: 'web-one', projectRoot: '/work/octocat/Hello-World', projectName: 'Hello-World' }; state.patchSession.mockClear(); });
afterEach(() => { cleanup(); vi.unstubAllGlobals(); });
describe('owner-resolved editor workspace', () => {
 it('resolves the scoped registry identity for a hydrated short name and opens only the returned session directory', async () => {
  let finish!: (response: Response) => void;
  const lookup = new Promise<Response>(resolve => { finish = resolve; });
  const fetcher = vi.fn((path: string, _init?: RequestInit) => Promise.resolve(path === '/api/git/projects' ? json(registry) : lookup));
  vi.stubGlobal('fetch', fetcher);
  render(<Editor />);
  await waitFor(() => expect(fetcher.mock.calls.some(([path]) => path === '/api/git/session-dir')).toBe(true));
  const call = fetcher.mock.calls.find(([path]) => path === '/api/git/session-dir') as unknown as [string, RequestInit];
  expect(JSON.parse(call[1].body as string)).toEqual({ project: 'octocat/Hello-World', session_id: 'web-one' });
  expect(screen.queryByTitle('VSCode')).toBeNull();
  expect((screen.getByRole('combobox') as HTMLSelectElement).value).toBe('/work/octocat/Hello-World');
  finish(json({ dir: '/work/octocat/Hello-World/.aimee/worktrees/owned/main' }));
  await waitFor(() => expect(screen.getByTitle('VSCode').getAttribute('src')).toBe('/vscode/?folder=%2Fwork%2Foctocat%2FHello-World%2F.aimee%2Fworktrees%2Fowned%2Fmain'));
  expect(state.patchSession).not.toHaveBeenCalled();
 });
 it('does not open the checkout when the owner refuses session resolution', async () => {
  vi.stubGlobal('fetch', vi.fn((path: string) => Promise.resolve(path === '/api/git/projects' ? json(registry) : json({ error: 'private backend detail' }, 400))));
  render(<Editor />);
  await waitFor(() => expect(screen.getByRole('alert').textContent).toBe('Could not resolve this session’s editor workspace.'));
  expect(screen.queryByTitle('VSCode')).toBeNull();
  expect(document.body.textContent).not.toContain('private backend detail');
 });
 it('never submits an unregistered root or guesses the same basename from another owner', async () => {
  state.active.projectRoot = '/unowned/octocat/Hello-World';
  const fetcher = vi.fn((_path: string) => Promise.resolve(json(registry))); vi.stubGlobal('fetch', fetcher);
  render(<Editor />);
  await screen.findByRole('alert');
  expect(fetcher.mock.calls.every(([path]) => path === '/api/git/projects')).toBe(true);
  expect(screen.queryByTitle('VSCode')).toBeNull();
 });
 it('ignores a late directory response from the previously selected session', async () => {
  let old!: (response: Response) => void; const oldReply = new Promise<Response>(resolve => { old = resolve; });
  const fetcher = vi.fn((path: string, init?: RequestInit) => path === '/api/git/projects' ? Promise.resolve(json(registry)) : JSON.parse(init!.body as string).session_id === 'web-one' ? oldReply : Promise.resolve(json({ dir: '/work/second-session/main' })));
  vi.stubGlobal('fetch', fetcher); const view = render(<Editor />);
  await waitFor(() => expect(fetcher.mock.calls.some(([path]) => path === '/api/git/session-dir')).toBe(true));
  state.active = { ...state.active, aimeeSid: 'web-two' }; view.rerender(<Editor />);
  await waitFor(() => expect(screen.getByTitle('VSCode').getAttribute('src')).toContain('second-session'));
  await act(async () => { old(json({ dir: '/work/stale-first-session/main' })); await oldReply; });
  expect(screen.getByTitle('VSCode').getAttribute('src')).toContain('second-session');
 });
 it('opens a registry-matched checkout before the session has an id without requesting a worktree', async () => {
  state.active.aimeeSid = '';
  const fetcher = vi.fn((_path: string) => Promise.resolve(json(registry))); vi.stubGlobal('fetch', fetcher);
  render(<Editor />);
  await waitFor(() => expect(screen.getByTitle('VSCode').getAttribute('src')).toBe('/vscode/?folder=%2Fwork%2Foctocat%2FHello-World'));
  expect(fetcher.mock.calls.every(([path]) => path === '/api/git/projects')).toBe(true);
  expect(state.patchSession).not.toHaveBeenCalled();
 });

});
