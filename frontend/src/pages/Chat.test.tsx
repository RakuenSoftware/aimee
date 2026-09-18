/** @vitest-environment jsdom */
import { StrictMode } from 'react';
import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import Chat from './Chat';

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
  vi.stubGlobal('EventSource', class {
    addEventListener() {}
    close() {}
  });
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

describe('webchat intermediate messages', () => {
  it('shows text before completion and batches subsequent tokens', async () => {
    await start();
    await emit(['turn_start'], ['text', { content: 'Checking' }]);
    expect(screen.getByText('Checking')).toBeTruthy();
    await emit(['text', { content: ' the' }], ['text', { content: ' files' }]);
    expect(screen.queryByText('Checking the files')).toBeNull();
    await tick();
    expect(screen.getByText('Checking the files')).toBeTruthy();
    expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/chat/live'))).toBe(false);
    await emit(['turn_end'], ['done']);
    await close();
    expect(screen.getAllByText('Checking the files')).toHaveLength(1);
  });

  it('preserves intermediate messages when the next message starts in the same read', async () => {
    await start();
    await emit(
      ['turn_start'], ['text', { content: 'First' }], ['text', { content: ' update' }],
      ['turn_start'], ['text', { content: 'Second' }], ['text', { content: ' update' }],
      ['turn_start'], ['text', { content: 'Final' }], ['text', { content: ' answer' }],
      ['turn_end'], ['done'],
    );
    await close();
    expect(screen.getAllByText(/^(First update|Second update|Final answer)$/).map(el => el.textContent))
      .toEqual(['First update', 'Second update', 'Final answer']);
  });

  it('displays thinking and flushes the last text batch on EOF without done', async () => {
    await start();
    await emit(['turn_start'], ['thinking', { content: 'Inspecting' }], ['thinking', { content: ' files' }]);
    await tick();
    expect(screen.getByText('Thinking…')).toBeTruthy();
    expect(screen.getByText('Inspecting files')).toBeTruthy();
    await emit(['text', { content: 'Found' }], ['text', { content: ' the issue' }]);
    await close();
    expect(screen.getByText('Found the issue')).toBeTruthy();
    expect(screen.getByPlaceholderText(/^Type a message/).getAttribute('placeholder')).not.toMatch(/steer/i);
  });

  it('keeps partial content when an error arrives before the flush timer', async () => {
    await start();
    await emit(['turn_start'], ['text', { content: 'Partial' }], ['text', { content: ' update' }],
      ['error', { message: 'Provider disconnected' }]);
    await close();
    expect(screen.getByText(/Partial update/)).toBeTruthy();
    expect(screen.getByText(/Provider disconnected/)).toBeTruthy();
  });

  it('routes queued deltas and later messages to the originating tab', async () => {
    const view = await start();
    await emit(['turn_start'], ['text', { content: 'First' }], ['text', { content: ' update' }]);
    context.active = context.sessions[1];
    view.rerender(<StrictMode><Chat /></StrictMode>);
    await tick();
    await emit(['turn_start'], ['text', { content: 'Finished elsewhere' }], ['turn_end'], ['done']);
    await close();
    expect(screen.queryByText('First update')).toBeNull();
    expect(screen.queryByText('Finished elsewhere')).toBeNull();
    context.active = context.sessions[0];
    view.rerender(<StrictMode><Chat /></StrictMode>);
    expect(screen.getByText('First update')).toBeTruthy();
    expect(screen.getByText('Finished elsewhere')).toBeTruthy();
  });
});
