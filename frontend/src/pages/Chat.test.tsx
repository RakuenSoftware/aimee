/** @vitest-environment jsdom */
import { StrictMode, useLayoutEffect } from 'react';
import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import Chat from './Chat';
import type { SessionEvent } from '../ir/session';
import { operatorCancelError } from '../ir/interruption';

const context = vi.hoisted(() => {
  const sessions = ['one', 'two'].map(id => ({
    id, name: id, aimeeSid: `web-${id}`, claudeSid: '', projectRoot: '',
    messages: [{ role: 'user' as const, text: `History ${id}` }],
  }));
  return { sessions, active: sessions[0], patchSession: vi.fn() };
});
vi.mock('../SessionContext', () => ({ useSessions: () => context }));
vi.mock('../components/ProjectPicker', () => ({ default: () => null }));

let stream: ReadableStreamDefaultController<Uint8Array>;
let fetchMock: ReturnType<typeof vi.fn>;
const encoder = new TextEncoder();
let sequence = 0;
let sources: TestEventSource[];
class TestEventSource {
  listeners = new Map<string, (e: { data: string }) => void>();
  closed = false;
  constructor(public url: string) { sources.push(this); }
  addEventListener(name: string, listener: (e: { data: string }) => void) { this.listeners.set(name, listener); }
  close() { this.closed = true; }
  emit(event: Record<string, unknown>) { this.listeners.get('ir')?.({ data: JSON.stringify(event) }); }
}
function envelope(event: SessionEvent, seq = ++sequence) {
  return { version: 1, session_id: 'web-one', sequence: String(seq), event_id: `event-${seq}`,
    turn_id: 'turn-one', actor: 'owner', event };
}
function message(id: string, text: string): SessionEvent {
  return { kind: 'message', message_id: id, payload: { role: 'assistant', blocks: [{ type: 'text', text }] } };
}
function delta(type: string, extra = {}): SessionEvent {
  return { kind: 'delta', message_id: 'answer', role: 'assistant', payload: { type, ...extra } } as SessionEvent;
}
async function ir(...events: SessionEvent[]) {
  await emit(...events.map(e => ['ir', envelope(e)] as [string, Record<string, unknown>]));
  await tick();
}

async function emit(...events: [string, Record<string, unknown>?][]) {
  await act(async () => {
    stream.enqueue(encoder.encode(events.map(([event, data = {}]) =>
      `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`).join('')));
  });
}
async function tick(ms = 100) {
  await act(async () => { await vi.advanceTimersByTimeAsync(ms); });
}
async function start() {
  const view = render(<StrictMode><Chat /></StrictMode>);
  await act(async () => {});
  const source = sources.filter(source => !source.closed).at(-1)!;
  await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
  fireEvent.change(screen.getByPlaceholderText(/^Type a message/), { target: { value: 'Please investigate' } });
  fireEvent.keyDown(screen.getByPlaceholderText(/^Type a message/), { key: 'Enter', shiftKey: false });
  await act(async () => {});
  expect(fetchMock.mock.calls.some(([url]) => url === '/api/chat/send')).toBe(true);
  return view;
}
async function close() {
  await act(async () => { stream.close(); });
}

beforeEach(() => {
  vi.useFakeTimers();
  localStorage.clear();
  context.active = context.sessions[0];
  sequence = 0; sources = [];
  vi.stubGlobal('EventSource', TestEventSource);
  vi.stubGlobal('ResizeObserver', class {
    observe() {}
    unobserve() {}
    disconnect() {}
  });
  Element.prototype.scrollIntoView = vi.fn();
  fetchMock = vi.fn(async (url: string, init?: RequestInit) => {
    if (url.startsWith('/api/chat/primary?')) return new Response(JSON.stringify({ agent: '' }));
    if (url.startsWith('/api/chat/persona?')) return new Response(JSON.stringify({ name: 'engineer' }));
    if (url === '/api/chat/send') {
      return new Response(new ReadableStream<Uint8Array>({ start(c) { stream = c; } }), {
        headers: { 'Content-Type': 'text/event-stream' },
      });
    }
    return new Response(JSON.stringify(url === '/api/chat/attach' ? { session_id: JSON.parse(String(init?.body)).sid, attach_id: 'attach-1' } : {}));
  });
  vi.stubGlobal('fetch', fetchMock);
});
afterEach(() => {
  cleanup();
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('canonical browser chat', () => {
  it('still auto-titles the first explicit message in an empty conversation', async () => {
    const session = context.sessions[0];
    const originalMessages = session.messages;
    session.messages = [];
    try {
      render(<StrictMode><Chat /></StrictMode>);
      await act(async () => {});
      const source = sources.filter(item => !item.closed).at(-1)!;
      await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
      context.patchSession.mockClear();
      fireEvent.change(screen.getByPlaceholderText(/^Type a message/), { target: { value: 'First explicit question' } });
      fireEvent.keyDown(screen.getByPlaceholderText(/^Type a message/), { key: 'Enter' });
      await act(async () => {});
      expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(1);
      expect(context.patchSession.mock.calls.filter(([, patch]) => Object.hasOwn(patch, 'name')))
        .toEqual([['one', { name: 'First explicit question' }]]);
      await close();
    } finally { session.messages = originalMessages; }
  });

  it('preserves a canonical-only conversation title when sending after replay', async () => {
    const session = context.sessions[0];
    const originalMessages = session.messages;
    const originalName = session.name;
    session.messages = [];
    session.name = 'User chosen investigation title';
    context.patchSession.mockClear();
    try {
      render(<StrictMode><Chat /></StrictMode>);
      await act(async () => {});
      const source = sources.filter(item => !item.closed).at(-1)!;
      await act(async () => {
        source.emit(envelope({ kind: 'execution', payload: { status: 'running' } }));
        source.emit(envelope({ kind: 'message', message_id: 'historical-user', payload: {
          role: 'user', blocks: [{ type: 'text', text: 'Original question' }],
        } }));
        source.emit(envelope(message('historical-answer', 'Historical canonical answer')));
        source.emit(envelope({ kind: 'execution', payload: { status: 'completed' } }));
        source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: String(sequence) }) });
      });
      await tick();
      expect(screen.getByText('Historical canonical answer')).toBeTruthy();
      expect(session.messages).toHaveLength(0); // Canonical replay is the real history owner.
      context.patchSession.mockClear();
      fireEvent.change(screen.getByPlaceholderText(/^Type a message/), { target: { value: 'Next model follow-up' } });
      fireEvent.keyDown(screen.getByPlaceholderText(/^Type a message/), { key: 'Enter' });
      await act(async () => {});
      expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(1);
      expect(context.patchSession.mock.calls.filter(([, patch]) => Object.hasOwn(patch, 'name'))).toHaveLength(0);
      await close();
    } finally {
      session.messages = originalMessages;
      session.name = originalName;
    }
  });

  it('admits a fresh session before preferences or replay and reuses its surface for sending', async () => {
    let resolveAttach!: (response: Response) => void;
    const ordinary = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => url === '/api/chat/attach'
      ? new Promise<Response>(resolve => { resolveAttach = resolve; }) : ordinary(url, init));
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/attach')).toHaveLength(1);
    expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/chat/primary'))).toBe(false);
    expect(sources).toHaveLength(0);
    await act(async () => { resolveAttach(new Response(JSON.stringify({ session_id: 'web-one', attach_id: 'fresh-surface' }))); });
    expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/chat/primary?sid=web-one'))).toBe(true);
    expect(sources.filter(source => !source.closed)).toHaveLength(1);
    const admitted = sources.filter(source => !source.closed).at(-1)!;
    await act(async () => { admitted.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
    fireEvent.change(screen.getByPlaceholderText(/^Type a message/), { target: { value: 'Fresh session first turn' } });
    fireEvent.keyDown(screen.getByPlaceholderText(/^Type a message/), { key: 'Enter' });
    await act(async () => {});
    const sent = fetchMock.mock.calls.find(([url]) => url === '/api/chat/send');
    expect(JSON.parse(String(sent?.[1]?.body))).toMatchObject({ aimee_session_id: 'web-one', attach_id: 'fresh-surface' });
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/attach')).toHaveLength(1);
    await close();
  });

  it('does not revive a cached attachment after a real page remount', async () => {
    localStorage.setItem('aimee_chat_tabs', JSON.stringify([{ sessionId: 'one', aimeeSid: 'web-one', attachId: 'dead-surface', messages: [] }]));
    let view = await start();
    expect(JSON.parse(String(fetchMock.mock.calls.find(([url]) => url === '/api/chat/send')?.[1]?.body)).attach_id).toBe('attach-1');
    await close();
    view.unmount();
    await act(async () => {});
    expect(fetchMock.mock.calls.some(([url, init]) => url === '/api/chat/detach' && JSON.parse(String(init?.body)).attach_id === 'attach-1')).toBe(true);
    fetchMock.mockClear();
    view = await start();
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/attach')).toHaveLength(1);
    expect(JSON.parse(String(fetchMock.mock.calls.find(([url]) => url === '/api/chat/send')?.[1]?.body)).attach_id).toBe('attach-1');
    await close(); view.unmount();
  });

  it('releases a late attachment when its page has already closed', async () => {
    let resolveAttach!: (response: Response) => void;
    fetchMock.mockImplementation((url: string) => url === '/api/chat/attach'
      ? new Promise<Response>(resolve => { resolveAttach = resolve; }) : Promise.resolve(new Response('{}')));
    const view = render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    view.unmount();
    await act(async () => { resolveAttach(new Response(JSON.stringify({ session_id: 'web-one', attach_id: 'late-surface' }))); });
    const detaches = fetchMock.mock.calls.filter(([url]) => url === '/api/chat/detach');
    expect(detaches).toHaveLength(1);
    expect(JSON.parse(String(detaches[0][1]?.body))).toEqual({ sid: 'web-one', attach_id: 'late-surface' });
    expect(sources).toHaveLength(0);
  });

  it('refuses foreign attachment replies and retries through the admitted owner route', async () => {
    const ordinary = fetchMock.getMockImplementation()!;
    let attempts = 0;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => url === '/api/chat/attach'
      ? Promise.resolve(new Response(JSON.stringify({ session_id: ++attempts === 1 ? 'foreign-session' : 'web-one', attach_id: 'owned-surface' }))) : ordinary(url, init));
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    expect(screen.getByRole('alert').textContent).toContain('Invalid conversation attachment');
    expect(sources).toHaveLength(0);
    expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/chat/primary'))).toBe(false);
    fireEvent.click(screen.getByText('Reconnect conversation'));
    await act(async () => {});
    expect(attempts).toBe(2);
    expect(sources.filter(source => !source.closed)).toHaveLength(1);
    expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/chat/primary?sid=web-one'))).toBe(true);
  });

  it('keeps a refused send visible through replay and reacquires its surface only on explicit reconnect', async () => {
    await start();
    const source = sources.filter(source => !source.closed).at(-1)!;
    await emit(['ir', { status: 'error', message: 'invalid chat attachment', http_status: 502 }]);
    await close();
    expect(screen.getByRole('alert').textContent).toContain('invalid chat attachment');
    await act(async () => {
      source.emit(envelope(message('saved', 'A saved message arriving during reconnect')));
      source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '1' }) });
    });
    expect(screen.getByRole('alert').textContent).toContain('invalid chat attachment');
    const ordinary = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => url === '/api/chat/attach'
      ? Promise.resolve(new Response(JSON.stringify({ session_id: 'web-one', attach_id: 'replacement-surface' }))) : ordinary(url, init));
    fireEvent.click(screen.getByText('Reconnect conversation'));
    await act(async () => {});
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/attach')).toHaveLength(2);
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(1);
    expect(fetchMock.mock.calls.some(([url, init]) => url === '/api/chat/detach' && JSON.parse(String(init?.body)).attach_id === 'attach-1')).toBe(true);
    const recovered = sources.filter(source => !source.closed).at(-1)!;
    await act(async () => { recovered.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: String(sequence) }) }); });
    fireEvent.change(screen.getByPlaceholderText(/^Type a message/), { target: { value: 'An explicitly requested new turn' } });
    fireEvent.keyDown(screen.getByPlaceholderText(/^Type a message/), { key: 'Enter' });
    await act(async () => {});
    const sends = fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send');
    expect(sends).toHaveLength(2);
    expect(JSON.parse(String(sends[1][1]?.body)).attach_id).toBe('replacement-surface');
    await close();
  });

  it('keeps the live surface through BFcache navigation and detaches only on a genuine page discard', async () => {
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/attach')).toHaveLength(1);
    await act(async () => {
      window.dispatchEvent(new Event('beforeunload'));
      window.dispatchEvent(new PageTransitionEvent('pagehide', { persisted: true }));
      window.dispatchEvent(new PageTransitionEvent('pageshow', { persisted: true }));
    });
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/detach')).toHaveLength(0);
    const source = sources.filter(source => !source.closed).at(-1)!;
    await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
    fireEvent.change(screen.getByPlaceholderText(/^Type a message/), { target: { value: 'After a real Back navigation' } });
    fireEvent.keyDown(screen.getByPlaceholderText(/^Type a message/), { key: 'Enter' });
    await act(async () => {});
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/attach')).toHaveLength(1);
    expect(JSON.parse(String(fetchMock.mock.calls.find(([url]) => url === '/api/chat/send')?.[1]?.body)).attach_id).toBe('attach-1');
    await close();
    await act(async () => {
      window.dispatchEvent(new Event('beforeunload'));
      window.dispatchEvent(new PageTransitionEvent('pagehide', { persisted: false }));
    });
    const detaches = fetchMock.mock.calls.filter(([url]) => url === '/api/chat/detach');
    expect(detaches).toHaveLength(1);
    expect(JSON.parse(String(detaches[0][1]?.body))).toEqual({ sid: 'web-one', attach_id: 'attach-1' });
    expect(detaches[0][1]?.keepalive).toBe(true);
  });

  it('merges delayed channel history with repeated live frames and reconnect snapshots', async () => {
    localStorage.setItem('aimee_active_channel', 'room');
    const ordinaryFetch = fetchMock.getMockImplementation()!;
    let resolveHistory!: (response: Response) => void;
    const history = new Promise<Response>(resolve => { resolveHistory = resolve; });
    fetchMock.mockImplementation((url: string) => url === '/api/channels/room/messages' ? history.then(response => response.clone()) : ordinaryFetch(url));
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const source = sources.findLast(source => !source.closed && source.url.includes('/channels/room/events'))!;
    const second = { id: '2', sender: 'user', text: 'Live channel message' };
    await act(async () => {
      source.listeners.get('message')?.({ data: JSON.stringify(second) });
      source.listeners.get('message')?.({ data: JSON.stringify(second) });
      resolveHistory(new Response(JSON.stringify({ messages: [{ id: '1', sender: 'user', text: 'Saved channel message' }, second] })));
    });
    expect(screen.getAllByText('Live channel message')).toHaveLength(1);
    expect(screen.getAllByText('Saved channel message')).toHaveLength(1);
  });

  it('hides unsupported branching instead of offering a mutation that always fails', async () => {
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    expect(screen.queryByRole('button', { name: 'Branch', exact: true })).toBeNull();
    expect(fetchMock.mock.calls.some(([url]) => ['/api/chat/threads', '/api/chat/branch', '/api/chat/switch-thread'].includes(url))).toBe(false);
  });

  it('does not revive raw thread mutations from a legacy capability response', async () => {
    const ordinaryFetch = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => {
      if (url === '/api/chat/threads') return Promise.resolve(new Response(JSON.stringify({ capabilities: { branching: true } })));
      if (url === '/api/chat/branch' || url === '/api/chat/switch-thread') return Promise.resolve(new Response(JSON.stringify({ ok: true, messages: [{ role: 'assistant', text: 'Foreign thread transcript' }] })));
      return ordinaryFetch(url, init);
    });
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    expect(screen.queryByRole('button', { name: /Branch/ })).toBeNull();
    expect(screen.getByRole('log', { name: 'Conversation' }).textContent).not.toContain('Foreign thread transcript');
    expect(fetchMock.mock.calls.some(([url]) => ['/api/chat/threads', '/api/chat/branch', '/api/chat/switch-thread'].includes(url))).toBe(false);
  });

  it('labels an unavailable model lookup instead of displaying the default as a known pin', async () => {
    const ordinaryFetch = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => {
      if (url === '/api/models') return Promise.resolve(new Response(JSON.stringify({ models: [{ name: 'default-model' }], default_agent: 'default-model' })));
      if (url.startsWith('/api/chat/primary?')) return Promise.resolve(new Response('{}', { status: 503 }));
      return ordinaryFetch(url, init);
    });
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const selector = screen.getByRole('combobox', { name: 'Agent' }) as HTMLSelectElement;
    expect(selector.value).toBe('');
    expect(selector.selectedOptions[0].textContent).toBe('Agent unavailable');
    expect(screen.getByRole('alert').textContent).toContain('Current selection is unavailable');
  });

  it.each(['Agent', 'Persona'])('preserves the saved %s when the catalog no longer lists it', async kind => {
    const ordinaryFetch = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => {
      if (url === '/api/models') return Promise.resolve(new Response(JSON.stringify({ models: [{ name: 'available-model' }], default_agent: 'available-model' })));
      if (url === '/api/chat/personas') return Promise.resolve(new Response(JSON.stringify({ personas: [{ name: 'available-persona' }] })));
      if (url.startsWith('/api/chat/primary?')) return Promise.resolve(new Response(JSON.stringify({ agent: 'saved-model' })));
      if (url.startsWith('/api/chat/persona?')) return Promise.resolve(new Response(JSON.stringify({ name: 'saved-persona' })));
      return ordinaryFetch(url, init);
    });
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const selector = screen.getByRole('combobox', { name: kind }) as HTMLSelectElement;
    expect(selector.value).toBe(kind === 'Agent' ? 'saved-model' : 'saved-persona');
    expect(selector.selectedOptions[0].textContent).toContain('not in current catalog');
    expect(fetchMock.mock.calls.some(([, init]) => init?.method === 'POST' && /saved-/.test(String(init.body)))).toBe(false);
  });

  it('shows an explicit empty persona as the session default instead of the first named persona', async () => {
    const ordinaryFetch = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => {
      if (url === '/api/chat/personas') return Promise.resolve(new Response(JSON.stringify({ personas: [{ name: 'engineer' }] })));
      if (url.startsWith('/api/chat/persona?')) return Promise.resolve(new Response(JSON.stringify({ name: '' })));
      return ordinaryFetch(url, init);
    });
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const selector = screen.getByRole('combobox', { name: 'Persona' }) as HTMLSelectElement;
    expect(selector.value).toBe('');
    expect(selector.selectedOptions[0].textContent).toBe('Session default');
  });

  it('derives idle, running and terminal composer state from the canonical execution log', async () => {
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const source = sources.filter(candidate => !candidate.closed).at(-1)!;
    const running = envelope({ kind: 'execution', payload: { status: 'running' } });
    await act(async () => { source.emit(running); });
    await tick();
    expect(screen.getByRole('button', { name: 'Send', exact: true })).toBeTruthy();
    await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '1' }) }); });
    expect(screen.getByRole('button', { name: 'Steer', exact: true })).toBeTruthy();
    expect(screen.getByPlaceholderText('Steer the running turn…')).toBeTruthy();
    await act(async () => { source.emit(running); });
    await tick();
    expect(screen.getByRole('button', { name: 'Steer', exact: true })).toBeTruthy();
    await act(async () => { source.emit(envelope({ kind: 'execution', payload: { status: 'completed' } })); });
    await tick();
    expect(screen.getByRole('button', { name: 'Send', exact: true })).toBeTruthy();
    expect(screen.getByPlaceholderText(/^Type a message/)).toBeTruthy();
  });

  async function runningDraft(text: string) {
    const view = render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const source = sources.filter(candidate => !candidate.closed).at(-1)!;
    await act(async () => { source.emit(envelope({ kind: 'execution', payload: { status: 'running' } }));
      source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '1' }) }); });
    const box = screen.getByPlaceholderText('Steer the running turn…');
    fireEvent.change(box, { target: { value: text } });
    fireEvent.keyDown(box, { key: 'Enter', shiftKey: false });
    await act(async () => {});
    return { source, box, view };
  }

  it('stops the exact canonical turn and waits for its terminal before one continuation', async () => {
    const ordinary = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => url === '/api/chat/cancel'
      ? Promise.resolve(new Response(JSON.stringify({ status: 'ok', session_id: 'web-one', turn_id: 'turn-one', disposition: 'stopping' })))
      : ordinary(url, init));
    const { source, box } = await runningDraft('New explicit continuation');
    const stops = () => fetchMock.mock.calls.filter(([url]) => url === '/api/chat/cancel');
    const sends = () => fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send');
    expect(stops()).toHaveLength(1);
    expect(JSON.parse(String(stops()[0][1]?.body))).toEqual({ session_id: 'web-one', turn_id: 'turn-one' });
    expect(sends()).toHaveLength(0);
    expect((box as HTMLTextAreaElement).value).toBe('New explicit continuation');
    fireEvent.keyDown(box, { key: 'Enter', shiftKey: false });
    await act(async () => { source.emit(envelope({ kind: 'execution', payload: { status: 'cancelled' } })); });
    await tick(300);
    expect(stops()).toHaveLength(1);
    expect(sends()).toHaveLength(1);
    await act(async () => { source.emit(envelope({ kind: 'execution', payload: { status: 'cancelled' } })); });
    await tick(300);
    expect(sends()).toHaveLength(1);
  });

  it.each(['http', 'network', 'malformed', 'wrong-turn'])('retains the stop draft and sends nothing after %s failure', async failure => {
    const ordinary = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => {
      if (url !== '/api/chat/cancel') return ordinary(url, init);
      if (failure === 'network') return Promise.reject(new Error('Network unavailable'));
      return Promise.resolve(new Response(JSON.stringify(failure === 'wrong-turn'
        ? { session_id: 'web-one', turn_id: 'another-turn', disposition: 'stopping' } : {}), { status: failure === 'http' ? 503 : 200 }));
    });
    const { box } = await runningDraft('Keep this draft');
    await tick(300);
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(0);
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/interrupt')).toHaveLength(0);
    expect((box as HTMLTextAreaElement).value).toBe('Keep this draft');
    expect(screen.getByRole('alert').textContent).toMatch(/retained|Network unavailable/);
  });

  it('does not continue into another selected session after a successful stop', async () => {
    const ordinary = fetchMock.getMockImplementation()!;
    let acknowledge!: (response: Response) => void;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => url === '/api/chat/cancel'
      ? new Promise<Response>(resolve => { acknowledge = resolve; }) : ordinary(url, init));
    const { source, view } = await runningDraft('Original session draft');
    await act(async () => { source.emit(envelope({ kind: 'execution', payload: { status: 'cancelled' } })); });
    context.active = context.sessions[1];
    view.rerender(<StrictMode><Chat /></StrictMode>);
    await act(async () => { acknowledge(new Response(JSON.stringify({ status: 'ok', session_id: 'web-one', turn_id: 'turn-one', disposition: 'stopping' }))); });
    await tick(300);
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(0);
  });

  it('keeps the draft when a stop is acknowledged but no canonical terminal arrives', async () => {
    const ordinary = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => url === '/api/chat/cancel'
      ? Promise.resolve(new Response(JSON.stringify({ status: 'ok', session_id: 'web-one', turn_id: 'turn-one', disposition: 'stopping' }))) : ordinary(url, init));
    const { box } = await runningDraft('Wait for a real terminal');
    await tick(30100);
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(0);
    expect((box as HTMLTextAreaElement).value).toBe('Wait for a real terminal');
    expect(screen.getByRole('alert').textContent).toMatch(/still stopping/);
  });

  it.each(['exact', 'wrong-event', 'wrong-error', 'wrong-sequence'])('requires the exact committed cancellation marker before continuation: %s', async mode => {
    const ordinary = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => url === '/api/chat/cancel'
      ? Promise.resolve(new Response(JSON.stringify({ status: 'ok', session_id: 'web-one', turn_id: 'turn-one', disposition: 'cancelled', terminal_sequence: mode === 'wrong-sequence' ? '3' : '2' }))) : ordinary(url, init));
    const { source, box } = await runningDraft('Continue only after the exact marker');
    await act(async () => { source.emit({ ...envelope({ kind: 'execution', payload: { status: 'cancelled', error: mode === 'wrong-error' ? 'ordinary stop' : operatorCancelError } }), event_id: mode === 'wrong-event' ? 'unrelated-stop' : 'operator-cancel:turn-one' }); });
    await tick(300);
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(mode === 'exact' ? 1 : 0);
    if (mode !== 'exact') expect((box as HTMLTextAreaElement).value).toBe('Continue only after the exact marker');
  });

  it('waits for the selected model to be saved before sending the retained draft', async () => {
    const ordinaryFetch = fetchMock.getMockImplementation()!;
    let save!: (response: Response) => void;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => {
      if (url === '/api/models') return Promise.resolve(new Response(JSON.stringify({ models: [{ name: 'old' }, { name: 'new' }], default_agent: 'old' })));
      if (url.startsWith('/api/chat/primary?')) return Promise.resolve(new Response(JSON.stringify({ agent: 'old' })));
      if (url === '/api/chat/primary' && init?.method === 'POST') return new Promise(resolve => { save = resolve; });
      return ordinaryFetch(url, init);
    });
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    fireEvent.change(screen.getByRole('combobox', { name: 'Agent' }), { target: { value: 'new' } });
    await act(async () => {});
    const source = sources.filter(source => !source.closed).at(-1)!;
    await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
    const input = screen.getByPlaceholderText(/^Type a message/);
    fireEvent.change(input, { target: { value: 'Use the selected model' } });
    fireEvent.keyDown(input, { key: 'Enter' });
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(0);
    expect(input).toHaveProperty('value', 'Use the selected model');
    await act(async () => { save(new Response('{}')); });
    fireEvent.keyDown(input, { key: 'Enter' });
    await act(async () => {});
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(1);
    await close();
  });

  it.each([false, true])('shows a failed history connection and reconnects without resubmitting (previously ready: %s)', async (previouslyReady) => {
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const source = sources.filter(candidate => !candidate.closed).at(-1)!;
    await act(async () => {
      source.emit(envelope(message('saved-history', 'Previously admitted history')));
      source.emit(envelope({ kind: 'control_request', request_id: 'request', control_kind: 'permission', payload: {} }));
      if (previouslyReady) source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '2' }) });
    });
    await tick();
    expect(!!screen.queryByRole('button', { name: 'Allow once' })).toBe(previouslyReady);
    const input = screen.getByPlaceholderText(/^Type a message/);
    fireEvent.change(input, { target: { value: 'Retain this unsent draft' } });
    // Browsers expose HTTP 502/refused EventSource responses through this error
    // event without making the status or response body available to JavaScript.
    await act(async () => { source.listeners.get('error')?.({ data: '' }); });
    expect(screen.getByRole('alert').textContent).toContain('Conversation connection failed');
    expect(screen.queryByText('Synchronizing conversation…')).toBeNull();
    expect(screen.queryByRole('button', { name: 'Allow once' })).toBeNull();
    expect(source.closed).toBe(true);
    expect(screen.getByText('Previously admitted history')).toBeTruthy();
    await act(async () => {
      source.emit(envelope(message('late', 'Failed stream content')));
      source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '2' }) });
    });
    await tick(15000);
    expect(screen.queryByText('Failed stream content')).toBeNull();
    expect(screen.queryByRole('button', { name: 'Allow once' })).toBeNull();
    expect(sources.filter(candidate => !candidate.closed)).toHaveLength(0);
    fireEvent.click(screen.getByRole('button', { name: 'Reconnect conversation' }));
    await tick();
    const recovered = sources.filter(candidate => !candidate.closed).at(-1)!;
    expect(recovered).not.toBe(source);
    expect(recovered.url).toContain('cursor=2');
    expect(screen.queryByRole('button', { name: 'Allow once' })).toBeNull();
    await act(async () => { recovered.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '2' }) }); });
    expect(screen.queryByRole('alert')).toBeNull();
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeTruthy();
    // A late error from the retired generation must not disable the new stream.
    await act(async () => { source.listeners.get('error')?.({ data: '' }); });
    expect(screen.queryByRole('alert')).toBeNull();
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeTruthy();
    expect(input).toHaveProperty('value', 'Retain this unsent draft');
    expect(fetchMock.mock.calls.filter(([url, init]) => init?.method === 'POST' && /\/api\/chat\/(send|reply|control-reply|cancel|interrupt)$/.test(String(url)))).toHaveLength(0);
  });

  it('does not dispatch before initial replay reveals an existing active turn', async () => {
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const source = sources.filter(candidate => !candidate.closed).at(-1)!;
    const input = screen.getByPlaceholderText(/^Type a message/);
    fireEvent.change(input, { target: { value: 'Review the current turn before continuing' } });
    fireEvent.keyDown(input, { key: 'Enter' });
    await act(async () => {});
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send' || url === '/api/chat/cancel')).toHaveLength(0);
    expect(input).toHaveProperty('value', 'Review the current turn before continuing');
    await act(async () => { source.emit(envelope({ kind: 'execution', payload: { status: 'running' } })); });
    fireEvent.keyDown(input, { key: 'Enter' });
    await act(async () => {});
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send' || url === '/api/chat/cancel')).toHaveLength(0);
    await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '1' }) }); });
    expect(screen.getByRole('button', { name: 'Steer', exact: true })).toBeTruthy();
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(0);
  });

  it('binds a same-commit session switch and Enter to the selected owner rather than old index refs', async () => {
    const roots = context.sessions.map(session => session.projectRoot);
    context.sessions[0].projectRoot = '/work/one'; context.sessions[1].projectRoot = '/work/two';
    function LayoutEnter({ switched }: { switched: boolean }) {
      useLayoutEffect(() => { if (switched) fireEvent.keyDown(screen.getByPlaceholderText(/^Type a message/), { key: 'Enter' }); }, [switched]);
      return <Chat />;
    }
    try {
      const view = render(<LayoutEnter switched={false} />);
      await act(async () => {});
      const first = sources.filter(candidate => !candidate.closed).at(-1)!;
      await act(async () => { first.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
      fireEvent.change(screen.getByPlaceholderText(/^Type a message/), { target: { value: 'Only the selected workspace' } });
      context.active = context.sessions[1];
      view.rerender(<LayoutEnter switched />);
      await act(async () => {});
      expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toEqual([]);
      expect(screen.getByPlaceholderText(/^Type a message/)).toHaveProperty('value', 'Only the selected workspace');
      const second = sources.filter(candidate => !candidate.closed).at(-1)!;
      await act(async () => { second.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-two', cursor: '0' }) }); });
      fireEvent.keyDown(screen.getByPlaceholderText(/^Type a message/), { key: 'Enter' });
      await act(async () => {});
      const sends = fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send');
      expect(sends).toHaveLength(1);
      expect(JSON.parse(String(sends[0][1]?.body))).toMatchObject({ aimee_session_id: 'web-two', cwd: '/work/two' });
      await close();
    } finally { context.sessions.forEach((session, i) => { session.projectRoot = roots[i]; }); }
  });

  it('rejects a same-tick stale control click before latching and allows one deliberate reply after replay', async () => {
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const source = sources.filter(candidate => !candidate.closed).at(-1)!;
    await act(async () => { source.emit(envelope({ kind: 'execution', payload: { status: 'running' } }));
      source.emit(envelope({ kind: 'control_request', request_id: 'pending', control_kind: 'permission', payload: {} }));
      source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '2' }) }); });
    const stale = screen.getByRole('button', { name: 'Allow once' });
    await act(async () => { source.listeners.get('error')?.({ data: '' }); fireEvent.click(stale); });
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/control-reply')).toHaveLength(0);
    fireEvent.click(screen.getByRole('button', { name: 'Reconnect conversation' }));
    await act(async () => {});
    const recovered = sources.filter(candidate => !candidate.closed).at(-1)!;
    await act(async () => { recovered.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '2' }) }); });
    fireEvent.click(screen.getByRole('button', { name: 'Allow once' }));
    await act(async () => {});
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/control-reply')).toHaveLength(1);
  });

  it('rejects a stale primary control whose canonical terminal arrived before the render flush', async () => {
    render(<StrictMode><Chat /></StrictMode>); await act(async () => {});
    const source = sources.filter(candidate => !candidate.closed).at(-1)!;
    await act(async () => { source.emit(envelope({ kind: 'execution', payload: { status: 'running' } }));
      source.emit(envelope({ kind: 'control_request', request_id: 'pending', control_kind: 'permission', payload: {} }));
      source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '2' }) }); });
    const stale = screen.getByRole('button', { name: 'Allow once' });
    await act(async () => { source.emit(envelope({ kind: 'execution', payload: { status: 'cancelled' } })); fireEvent.click(stale); });
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/control-reply')).toHaveLength(0);
  });

  it('does not classify an uncertain post-dispatch network failure as an unsent draft', async () => {
    const ordinary = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => url === '/api/chat/send'
      ? Promise.reject(new Error('Connection lost after dispatch began')) : ordinary(url, init));
    render(<StrictMode><Chat /></StrictMode>); await act(async () => {});
    const source = sources.filter(candidate => !candidate.closed).at(-1)!;
    await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
    const input = screen.getByPlaceholderText(/^Type a message/);
    fireEvent.change(input, { target: { value: 'Do not automatically repeat this operation' } });
    fireEvent.keyDown(input, { key: 'Enter' }); await act(async () => {});
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(1);
    expect(input).toHaveProperty('value', '');
    expect(screen.queryByRole('button', { name: 'Restore unsent message' })).toBeNull();
  });

  it('retains a new draft instead of dispatching while failed history awaits explicit canonical reconnect', async () => {
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const source = sources.filter(candidate => !candidate.closed).at(-1)!;
    await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
    await act(async () => { source.listeners.get('error')?.({ data: '' }); });
    const input = screen.getByPlaceholderText(/^Type a message/);
    fireEvent.change(input, { target: { value: 'Ask the real host question once' } });
    fireEvent.keyDown(input, { key: 'Enter' });
    await act(async () => {});
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(0);
    expect(input).toHaveProperty('value', 'Ask the real host question once');
    fireEvent.click(screen.getByRole('button', { name: 'Reconnect conversation' }));
    await act(async () => {});
    const recovered = sources.filter(candidate => !candidate.closed).at(-1)!;
    fireEvent.keyDown(input, { key: 'Enter' });
    await act(async () => {});
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(0);
    expect(input).toHaveProperty('value', 'Ask the real host question once');
    await act(async () => { recovered.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(0);
    fireEvent.keyDown(input, { key: 'Enter' });
    await act(async () => {});
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(1);
    await close();
  });

  it('retains an enqueued draft when history fails during attachment admission without dispatching it', async () => {
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const source = sources.filter(candidate => !candidate.closed).at(-1)!;
    await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
    const input = screen.getByPlaceholderText(/^Type a message/);
    fireEvent.change(input, { target: { value: 'First explicitly queued message' } });
    await act(async () => {
      fireEvent.keyDown(input, { key: 'Enter' });
      fireEvent.change(input, { target: { value: 'A different unsent draft' } });
      source.listeners.get('error')?.({ data: '' });
    });
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(0);
    expect(input).toHaveProperty('value', 'A different unsent draft');
    expect(screen.getByText('First explicitly queued message')).toBeTruthy();
    expect(screen.getByRole('log', { name: 'Conversation' }).textContent).not.toContain('First explicitly queued message');
    fireEvent.click(screen.getByRole('button', { name: 'Restore unsent message' }));
    expect(input).toHaveProperty('value', 'First explicitly queued message\n\nA different unsent draft');
    fireEvent.click(screen.getByRole('button', { name: 'Reconnect conversation' }));
    await act(async () => {});
    const recovered = sources.filter(candidate => !candidate.closed).at(-1)!;
    await act(async () => { recovered.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(0);
  });

  it('does not submit a waiting Steer continuation after its connection failed and canonical replay recovered', async () => {
    const ordinary = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, init?: RequestInit) => url === '/api/chat/cancel'
      ? Promise.resolve(new Response(JSON.stringify({ status: 'ok', session_id: 'web-one', turn_id: 'turn-one', disposition: 'stopping' })))
      : ordinary(url, init));
    const { source, box } = await runningDraft('Retain this interrupted continuation');
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/cancel')).toHaveLength(1);
    await act(async () => { source.listeners.get('error')?.({ data: '' }); });
    fireEvent.click(screen.getByRole('button', { name: 'Reconnect conversation' }));
    await act(async () => {});
    const recovered = sources.filter(candidate => !candidate.closed).at(-1)!;
    await act(async () => { recovered.emit(envelope({ kind: 'execution', payload: { status: 'cancelled' } }));
      recovered.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '2' }) }); });
    await tick(300);
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(0);
    expect(box).toHaveProperty('value', 'Retain this interrupted continuation');
    expect(screen.getByRole('alert').textContent).toContain('retained');
  });

  it('withdraws controls after a malformed live frame and ignores queued frames from the closed stream', async () => {
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const source = sources.filter(source => !source.closed).at(-1)!;
    await act(async () => { source.emit(envelope({ kind: 'control_request', request_id: 'request', control_kind: 'permission', payload: {} })); });
    await tick();
    await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '1' }) }); });
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeTruthy();
    await act(async () => { source.emit({ version: 1, session_id: 'foreign', sequence: '2' }); });
    expect(source.closed).toBe(true);
    expect(screen.queryByRole('button', { name: 'Allow once' })).toBeNull();
    await act(async () => {
      source.emit(envelope(message('late', 'Closed stream content')));
      source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '1' }) });
    });
    await tick();
    expect(screen.queryByText('Closed stream content')).toBeNull();
    expect(screen.queryByRole('button', { name: 'Allow once' })).toBeNull();
    expect(screen.getByText('Invalid canonical session envelope')).toBeTruthy();
    fireEvent.click(screen.getByRole('button', { name: 'Reconnect conversation' }));
    await tick();
    const recovered = sources.filter(candidate => !candidate.closed).at(-1)!;
    expect(recovered).not.toBe(source);
    expect(recovered.url).toContain('cursor=1');
    expect(screen.queryByRole('button', { name: 'Allow once' })).toBeNull();
    await act(async () => { recovered.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '1' }) }); });
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeTruthy();
    expect(screen.queryByRole('button', { name: 'Reconnect conversation' })).toBeNull();
  });

  it('waits for replay before offering controls and does not retrigger replies on duplicate delivery', async () => {
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const source = sources.filter(source => !source.closed).at(-1)!;
    const old = envelope({ kind: 'control_request', request_id: 'old', control_kind: 'permission', payload: { title: 'Historical request' } });
    await act(async () => { source.emit(old); });
    await tick();
    expect(screen.queryByRole('button', { name: 'Allow once' })).toBeNull();
    const resolved = envelope({ kind: 'control_response', request_id: 'old', control_kind: 'permission', payload: { decision: 'allow' } });
    const current = envelope({ kind: 'control_request', request_id: 'current', control_kind: 'permission', payload: { title: 'Current request' } });
    await act(async () => { source.emit(resolved); source.emit(current); });
    await tick();
    expect(screen.queryByRole('button', { name: 'Allow once' })).toBeNull();
    await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: String(sequence) }) }); });
    const allow = screen.getByRole('button', { name: 'Allow once' });
    fireEvent.click(allow);
    fireEvent.click(allow);
    await act(async () => { source.emit(current); });
    await tick();
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/control-reply')).toHaveLength(1);
    expect(screen.getAllByText('Current request')).toHaveLength(1);
    expect(screen.getByRole('button', { name: 'Allow once' }).hasAttribute('disabled')).toBe(true);
  });

  it('renders typed tool arguments and paired results', async () => {
    await start();
    await ir({ kind: 'message', message_id: 'use', payload: { role: 'assistant', blocks: [
      { type: 'tool_use', tool_id: 'opaque:1', name: 'delegate', input: { task: 'inspect' } },
    ] } });
    expect(screen.getByText('Running: delegate')).toBeTruthy();
    expect(screen.getByText(/"task": "inspect"/)).toBeTruthy();
    await ir({ kind: 'message', message_id: 'result', payload: { role: 'tool', blocks: [
      { type: 'tool_result', tool_id: 'opaque:1', content: 'Inspected the files' },
    ] } }, message('final', 'Finished'));
    await close();
    expect(screen.queryByText('Running: delegate')).toBeNull();
    expect(screen.getByText('Inspected the files')).toBeTruthy();
    expect(screen.getByText('Finished')).toBeTruthy();
  });

  it('renders text deltas before completion and keeps every message', async () => {
    await start();
    await ir(delta('TURN_START'), delta('BLOCK_START', { block_id: 0, kind: 'text' }),
      delta('BLOCK_DELTA', { block_id: 0, kind: 'text', text_delta: 'Checking' }));
    expect(screen.getByText('Checking')).toBeTruthy();
    await ir(delta('BLOCK_DELTA', { block_id: 0, kind: 'text', text_delta: ' files' }),
      delta('BLOCK_STOP', { block_id: 0, kind: 'text' }), delta('TURN_STOP'),
      message('second', 'Second update'), message('last', 'Final answer'));
    await close();
    expect(screen.getAllByText('Checking files')).toHaveLength(1);
    expect(screen.getByText('Second update')).toBeTruthy();
    expect(screen.getByText('Final answer')).toBeTruthy();
    expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/chat/live'))).toBe(false);
  });

  it('preserves thinking, partial content and canonical failures', async () => {
    await start();
    await ir({ kind: 'execution', payload: { status: 'running' } },
      { kind: 'message', message_id: 'thinking', payload: { role: 'assistant', blocks: [
        { type: 'thinking', text: 'Inspecting files', signature: 'opaque-signature' },
      ] } }, message('partial', 'Partial update'),
      { kind: 'execution', payload: { status: 'failed', error: 'Provider disconnected' } });
    await close();
    expect(screen.getByText('Inspecting files')).toBeTruthy();
    expect(screen.getByText('Partial update')).toBeTruthy();
    expect(screen.getByText('Provider disconnected')).toBeTruthy();
  });

  it('routes later messages to the originating tab', async () => {
    const view = await start();
    await ir(message('first', 'First update'));
    context.active = context.sessions[1];
    view.rerender(<StrictMode><Chat /></StrictMode>);
    await tick();
    await ir(message('last', 'Finished elsewhere'));
    await close();
    expect(screen.queryByText('Finished elsewhere')).toBeNull();
    context.active = context.sessions[0];
    view.rerender(<StrictMode><Chat /></StrictMode>);
    expect(screen.getByText('First update')).toBeTruthy();
    expect(screen.getByText('Finished elsewhere')).toBeTruthy();
  });

  it('replays from zero on a fresh browser and deduplicates overlapping POST delivery', async () => {
    const view = await start();
    const source = sources.filter(s => !s.closed).slice(-1)[0]!;
    expect(source.url).toContain('cursor=0');
    const first = envelope(message('first', 'Replayed answer'));
    const second = envelope(message('second', 'Next answer'));
    await emit(['ir', second]); // POST can race ahead of replay
    await act(async () => { source.emit(first); source.emit(second); });
    await tick();
    expect(screen.getAllByText('Replayed answer')).toHaveLength(1);
    expect(screen.getAllByText('Next answer')).toHaveLength(1);
    await close();
    view.unmount();
    render(<StrictMode><Chat /></StrictMode>);
    await act(async () => {});
    const fresh = sources.filter(s => !s.closed).slice(-1)[0]!;
    expect(fresh.url).toContain('cursor=0');
    await act(async () => { fresh.emit(first); fresh.emit(second); });
    await tick();
    expect(screen.getAllByText('Replayed answer')).toHaveLength(1);
    expect(screen.getAllByText('Next answer')).toHaveLength(1);
  });

  it('reports malformed transport events without dropping existing content', async () => {
    await start();
    await ir(message('first', 'Kept content'));
    await emit(['ir', { version: 2, session_id: 'web-one', sequence: '2' }]);
    await tick();
    expect(screen.getByText('Kept content')).toBeTruthy();
    expect(screen.getByRole('alert')).toBeTruthy();
  });
});

describe('canonical-only active history', () => {
  it('never renders a flat server transcript as an empty canonical conversation', async () => {
    render(<Chat />);
    await tick();
    const source = sources.filter(item => !item.closed).at(-1)!;
    await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
    expect(screen.getByRole('log', { name: 'Conversation' }).textContent).not.toContain('History one');
    expect(context.patchSession.mock.calls.filter(([, patch]) => Object.hasOwn(patch, 'messages'))).toEqual([]);
  });

  it('keeps flat history out of the active transcript after replay refusal and remount', async () => {
    const view = render(<Chat />);
    await tick();
    const source = sources.filter(item => !item.closed).at(-1)!;
    await act(async () => { source.listeners.get('error')?.({ data: '' }); });
    expect(screen.getByRole('log', { name: 'Conversation' }).textContent).not.toContain('History one');
    view.unmount();
    render(<Chat />);
    await tick();
    expect(screen.getByRole('log', { name: 'Conversation' }).textContent).not.toContain('History one');
    expect(fetchMock.mock.calls.filter(([url]) => url === '/api/chat/send')).toHaveLength(0);
  });
});

describe('flat history is an archive, never authority', () => {
  it('sends only the new user input after canonical empty replay, without cached transcript data', async () => {
    await start();
    const request = fetchMock.mock.calls.find(([url]) => url === '/api/chat/send')!;
    expect(JSON.parse(String(request[1]?.body))).toEqual({ message: 'Please investigate', aimee_session_id: 'web-one', attach_id: 'attach-1', cwd: '' });
    expect(context.patchSession.mock.calls.some(([, patch]) => Object.hasOwn(patch, 'messages'))).toBe(false);
    await close();
  });

  it('preserves archived thread metadata and messages without exposing thread mutations', async () => {
    const threads = [{ id: 1, label: 'Root', msg_count: 0, parent_id: -1, branch_msg_index: 0, active: false }, { id: 2, label: 'Alternative', msg_count: 1, parent_id: 1, branch_msg_index: 0, active: true }];
    const messages = [{ role: 'user', text: 'History one' }, { role: 'assistant', text: 'Archived original answer' }];
    localStorage.setItem('aimee_chat_tabs', JSON.stringify([{ sessionId: 'one', title: 'one', aimeeSid: 'web-one', messages, threads, activeThreadId: 2 }]));
    const original = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation(async (url: string, init?: RequestInit) => {
      if (url === '/api/chat/threads') return new Response(JSON.stringify({ capabilities: { branching: true }, threads, active_id: 2 }));
      if (url === '/api/chat/switch-thread') return new Response(JSON.stringify({ ok: true, messages: [{ role: 'assistant', text: 'Foreign legacy branch answer' }] }));
      return original(url, init);
    });
    render(<Chat />);
    await tick();
    expect(screen.queryByRole('button', { name: /Alternative|Branch/ })).toBeNull();
    expect(screen.getByLabelText('Legacy transcript archive').textContent).toContain('Archived original answer');
    expect(screen.getByRole('log', { name: 'Conversation' }).textContent).not.toContain('Archived original answer');
    expect(screen.getByRole('log', { name: 'Conversation' }).textContent).not.toContain('Foreign legacy branch answer');
    const cached = JSON.parse(localStorage.getItem('aimee_chat_tabs')!)[0];
    expect(cached.threads).toEqual(threads);
    expect(cached.activeThreadId).toBe(2);
    expect(cached.messages).toEqual(messages);
    expect(fetchMock.mock.calls.some(([url]) => ['/api/chat/threads', '/api/chat/branch', '/api/chat/switch-thread', '/api/chat/send'].includes(url))).toBe(false);
  });
});

it('keeps the idle placeholder short and retains the keyboard hint without sending Shift+Enter', async () => {
  render(<StrictMode><Chat /></StrictMode>);
  await act(async () => {});
  const source = sources.filter(candidate => !candidate.closed).at(-1)!;
  await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
  const input = screen.getByPlaceholderText(/^Type a message/);
  expect(input.getAttribute('placeholder')).toBe('Type a message…');
  expect(input.getAttribute('title')).toBe('Shift+Enter for newline');
  fireEvent.change(input, { target: { value: 'Unsent draft' } });
  fireEvent.keyDown(input, { key: 'Enter', shiftKey: true });
  expect(input).toHaveProperty('value', 'Unsent draft');
  expect(fetchMock.mock.calls.some(([url]) => url === '/api/chat/send')).toBe(false);
});

it('does not reuse a previous model usage observation for a later unknown turn', async () => {
  render(<StrictMode><Chat /></StrictMode>);
  await act(async () => {});
  const source = sources.filter(candidate => !candidate.closed).at(-1)!;
  await act(async () => { source.listeners.get('replay_complete')?.({ data: JSON.stringify({ session_id: 'web-one', cursor: '0' }) }); });
  const finish = async (turn: string, usage?: { usage_in: number; usage_out: number }) => {
    const send = (event: SessionEvent) => source.emit({ ...envelope(event), turn_id: turn });
    await act(async () => {
      send({ kind: 'execution', payload: { status: 'running' } });
      send({ kind: 'delta', message_id: turn, role: 'assistant', payload: { type: 'TURN_START' } });
      send({ kind: 'delta', message_id: turn, role: 'assistant', payload: { type: 'BLOCK_START', block_id: 0, kind: 'text' } });
      send({ kind: 'delta', message_id: turn, role: 'assistant', payload: { type: 'BLOCK_DELTA', block_id: 0, kind: 'text', text_delta: `Answer ${turn}` } });
      send({ kind: 'delta', message_id: turn, role: 'assistant', payload: { type: 'BLOCK_STOP', block_id: 0, kind: 'text' } });
      send({ kind: 'delta', message_id: turn, role: 'assistant', payload: { type: 'TURN_STOP', stop_reason: 'end_turn', ...usage } });
      send({ kind: 'execution', payload: { status: 'completed' } });
    });
    await tick();
  };
  await finish('known', { usage_in: 6, usage_out: 4 });
  expect(screen.getByText('6 in / 4 out')).toBeTruthy();
  await finish('unknown');
  expect(screen.queryByText('6 in / 4 out')).toBeNull();
  await finish('observed-zero', { usage_in: 0, usage_out: 0 });
  expect(screen.getByText('0 in / 0 out')).toBeTruthy();
  expect(fetchMock.mock.calls.some(([url]) => url === '/api/chat/send')).toBe(false);
});
