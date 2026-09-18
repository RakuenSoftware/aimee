/** @vitest-environment jsdom */
import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { StrictMode } from 'react';
import { SessionProvider, useSessions } from './SessionContext';

function SessionProbe() {
  const { sessions, active, addSession, patchSession, closeSession } = useSessions();
  return <>
    <div data-testid="sessions">{sessions.map(session => session.name).join(',')}</div>
    <div data-testid="active">{active?.name}|{active?.projectRoot}|{active?.messages.map(message => message.text).join(',')}</div>
    <button onClick={() => addSession('New chat')}>Add</button>
    <button onClick={() => active && patchSession(active.id, { projectRoot: '/work/new-project', projectName: 'new-project' })}>Project</button>
    <button onClick={() => active && closeSession(active.id)}>Close</button>
  </>;
}

function cacheSession(name: string) {
  localStorage.setItem('aimee_sessions', JSON.stringify([{
    id: 'ui-secret',
    name,
    projectRoot: '/private/project',
    projectName: 'project',
    claudeSid: 'provider-secret',
    aimeeSid: 'web-secret',
    attachId: '',
    messages: [{ role: 'user', text: 'private message' }],
  }]));
}

describe('SessionProvider bootstrap cache ownership', () => {
  beforeEach(() => {
    localStorage.clear();
  });

  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
  });

  it('does not reveal cached chats when identity verification fails', async () => {
    cacheSession('Private chat');
    localStorage.setItem('aimee_sessions_owner', 'alice');
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new Error('offline')));

    render(<SessionProvider><SessionProbe /></SessionProvider>);

    expect((await screen.findByTestId('sessions')).textContent).toBe('Session 1');
  });

  it('keeps an account-owned cache when identity succeeds but session restore is offline', async () => {
    cacheSession('Alice chat');
    localStorage.setItem('aimee_sessions_owner', 'alice');
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(new Response(JSON.stringify({ username: 'alice' }), {
        status: 200,
        headers: { 'Content-Type': 'application/json' },
      }))
      .mockRejectedValueOnce(new Error('sessions offline'));
    vi.stubGlobal('fetch', fetchMock);

    render(<SessionProvider><SessionProbe /></SessionProvider>);

    expect((await screen.findByTestId('sessions')).textContent).toBe('Alice chat');
  });
});

const json = (value: unknown, status = 200) => new Response(JSON.stringify(value), { status });
const remote = { id: 'web-secret', title: 'Alice chat', cwd: '/private/project', messages: [{ role: 'user', text: 'private message' }] };

function trustedCache() {
  cacheSession('Alice chat');
  localStorage.setItem('aimee_sessions_owner', 'alice');
  localStorage.setItem('aimee_server_sessions_authoritative_v1', 'alice');
}

describe('SessionProvider refresh races', () => {
  beforeEach(() => { localStorage.clear(); trustedCache(); });
  afterEach(() => { cleanup(); vi.unstubAllGlobals(); });

  it('preserves the active conversation on a transient identity failure after bootstrap', async () => {
    let offline = false;
    vi.stubGlobal('fetch', vi.fn(async (url: string) => {
      if (offline) throw new Error('offline');
      return json(url === '/api/auth/me' ? { username: 'alice' } : [remote]);
    }));
    render(<SessionProvider><SessionProbe /></SessionProvider>);
    await screen.findByText('Alice chat');
    offline = true;
    await act(async () => { window.dispatchEvent(new Event('focus')); });
    expect(screen.getByTestId('active').textContent).toBe('Alice chat|/private/project|private message');
  });

  it('still clears private history on an explicit authentication rejection', async () => {
    let rejected = false;
    vi.stubGlobal('fetch', vi.fn(async (url: string) => {
      if (rejected) return json({}, 401);
      return json(url === '/api/auth/me' ? { username: 'alice' } : [remote]);
    }));
    render(<SessionProvider><SessionProbe /></SessionProvider>);
    await screen.findByText('Alice chat');
    rejected = true;
    await act(async () => { window.dispatchEvent(new Event('focus')); });
    expect(screen.getByTestId('sessions').textContent).toBe('Session 1');
    expect(screen.getByTestId('active').textContent).not.toContain('private');
  });

  it('does not drop a newly created active chat when a GET races its pending save', async () => {
    let resolveSave!: (value: Response) => void;
    vi.stubGlobal('fetch', vi.fn((url: string, init?: RequestInit) => {
      if (init?.method === 'POST') return new Promise(resolve => { resolveSave = resolve; });
      return Promise.resolve(json(url === '/api/auth/me' ? { username: 'alice' } : [remote]));
    }));
    render(<StrictMode><SessionProvider><SessionProbe /></SessionProvider></StrictMode>);
    await screen.findByText('Alice chat');
    fireEvent.click(screen.getByRole('button', { name: 'Add' }));
    await act(async () => { window.dispatchEvent(new Event('focus')); });
    expect(screen.getByTestId('sessions').textContent).toBe('Alice chat,New chat');
    expect(screen.getByTestId('active').textContent).toBe('New chat||');
    await act(async () => { resolveSave(json({})); });
  });

  it('does not restore an old project when a refresh predates a successful local edit', async () => {
    let resolveRefresh!: (value: Response) => void;
    let delayRefresh = false;
    vi.stubGlobal('fetch', vi.fn((url: string, init?: RequestInit) => {
      if (init?.method === 'POST') return Promise.resolve(json({}));
      if (url === '/api/chat/sessions' && delayRefresh) return new Promise(resolve => { resolveRefresh = resolve; });
      return Promise.resolve(json(url === '/api/auth/me' ? { username: 'alice' } : [remote]));
    }));
    render(<SessionProvider><SessionProbe /></SessionProvider>);
    await screen.findByText('Alice chat');
    delayRefresh = true;
    await act(async () => { window.dispatchEvent(new Event('focus')); });
    fireEvent.click(screen.getByRole('button', { name: 'Project' }));
    await act(async () => {});
    await act(async () => { resolveRefresh(json([remote])); });
    expect(screen.getByTestId('active').textContent).toBe('Alice chat|/work/new-project|private message');
  });

  it('coalesces simultaneous focus and visibility refreshes', async () => {
    let resolveRefresh!: (value: Response) => void;
    let delayRefresh = false;
    vi.stubGlobal('fetch', vi.fn((url: string) => {
      if (url === '/api/chat/sessions' && delayRefresh) return new Promise(resolve => { resolveRefresh = resolve; });
      return Promise.resolve(json(url === '/api/auth/me' ? { username: 'alice' } : [remote]));
    }));
    render(<StrictMode><SessionProvider><SessionProbe /></SessionProvider></StrictMode>);
    await screen.findByText('Alice chat');
    delayRefresh = true;
    await act(async () => {
      window.dispatchEvent(new Event('focus'));
      document.dispatchEvent(new Event('visibilitychange'));
    });
    expect(vi.mocked(fetch).mock.calls.filter(([url]) => url === '/api/chat/sessions')).toHaveLength(2);
    await act(async () => { resolveRefresh(json([remote])); });
  });

  it('serializes writes and deletes after a pending create instead of resurrecting a closed chat', async () => {
    const saves: Array<(value: Response) => void> = [];
    vi.stubGlobal('fetch', vi.fn((url: string, init?: RequestInit) => {
      if (init?.method === 'POST') return new Promise(resolve => { saves.push(resolve); });
      if (init?.method === 'DELETE') return Promise.resolve(json({}));
      return Promise.resolve(json(url === '/api/auth/me' ? { username: 'alice' } : [remote]));
    }));
    render(<StrictMode><SessionProvider><SessionProbe /></SessionProvider></StrictMode>);
    await screen.findByText('Alice chat');
    fireEvent.click(screen.getByRole('button', { name: 'Add' }));
    await waitFor(() => expect(saves).toHaveLength(1));
    fireEvent.click(screen.getByRole('button', { name: 'Project' }));
    await act(async () => {});
    expect(saves).toHaveLength(1);
    await act(async () => { saves[0](json({})); });
    expect(saves).toHaveLength(2);
    fireEvent.click(screen.getByRole('button', { name: 'Close' }));
    expect(vi.mocked(fetch).mock.calls.some(([, init]) => init?.method === 'DELETE')).toBe(false);
    await act(async () => { saves[1](json({})); });
    expect(vi.mocked(fetch).mock.calls.filter(([, init]) => init?.method === 'DELETE')).toHaveLength(1);
    expect(screen.getByTestId('sessions').textContent).toBe('Alice chat');
  });
});
