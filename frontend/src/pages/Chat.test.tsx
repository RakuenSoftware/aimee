/** @vitest-environment jsdom */
import { StrictMode } from 'react';
import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import Chat from './Chat';
import type { SessionEvent } from '../ir/session';

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
  fetchMock = vi.fn(async (url: string) => {
    if (url === '/api/chat/send') {
      return new Response(new ReadableStream<Uint8Array>({ start(c) { stream = c; } }), {
        headers: { 'Content-Type': 'text/event-stream' },
      });
    }
    return new Response(JSON.stringify(url === '/api/chat/attach' ? { attach_id: 'attach-1' } : {}));
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
