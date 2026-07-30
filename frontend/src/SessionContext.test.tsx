/** @vitest-environment jsdom */
import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { SessionProvider, useSessions } from './SessionContext';

function SessionProbe() {
  const { sessions } = useSessions();
  return <div data-testid="sessions">{sessions.map(session => session.name).join(',')}</div>;
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
