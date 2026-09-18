/** @vitest-environment jsdom */
import { StrictMode } from 'react';
import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Session } from '../SessionContext';
import Chat from './Chat';

const context = vi.hoisted(() => ({ sessions: [] as Session[], active: null as Session | null, patchSession: vi.fn() }));
vi.mock('../SessionContext', () => ({ useSessions: () => context }));
vi.mock('./chat/ChatPrimitives', () => ({
  Message: ({ role, text }: { role: string; text: string }) => <div data-testid={role}>{text}</div>,
  BootstrapBanner: () => null, DiffBlock: () => null, RewindMarker: () => null,
  ThinkingBlock: () => null, ToolBlock: () => null, TurnSummaryCard: () => null,
}));

function session(id: string, text = ''): Session {
  return { id, aimeeSid: id, name: id, projectRoot: `/work/${id}`, projectName: id,
    claudeSid: '', attachId: '', messages: text ? [{ role: 'user', text }] : [] };
}
const json = (body: unknown, status = 200) => new Response(JSON.stringify(body), { status });
let stream: ReadableStreamDefaultController<Uint8Array>;
const emit = (event: string, data = {}) => stream.enqueue(new TextEncoder().encode(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`));
const tick = async (ms = 0) => { await act(async () => { await vi.advanceTimersByTimeAsync(ms); }); };

beforeEach(() => {
  vi.useFakeTimers();
  localStorage.clear();
  context.sessions = [session('one')];
  context.active = context.sessions[0];
  context.patchSession.mockReset();
  Element.prototype.scrollIntoView = vi.fn();
  vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} });
  vi.stubGlobal('EventSource', class { addEventListener() {} close() {} });
  vi.stubGlobal('fetch', vi.fn(async (url: string) => {
    if (url === '/api/chat/send') return new Response(new ReadableStream({ start(controller) { stream = controller; } }));
    if (url === '/api/chat/attach') return json({ attach_id: 'attachment' });
    if (url === '/api/git/projects') return json({ root: '/work', projects: ['one', 'two'] });
    if (url.startsWith('/api/chat/bootstrap-status')) return json({ has_rules: true });
    if (url.startsWith('/api/sessions/workflows')) return json({}, 404);
    return json({});
  }));
});
afterEach(() => { cleanup(); vi.useRealTimers(); vi.unstubAllGlobals(); });

function send(text: string) {
  const input = screen.getByPlaceholderText(/Type a message/);
  fireEvent.change(input, { target: { value: text } });
  fireEvent.keyDown(input, { key: 'Enter' });
}

describe('chat transcript ownership', () => {
  it.each([false, true])('keeps one model reply when turn_end flushes the final text batch (StrictMode=%s)', async strict => {
    render(strict ? <StrictMode><Chat /></StrictMode> : <Chat />);
    await tick();
    send('hello');
    await tick();
    await act(async () => { emit('turn_start'); });
    await act(async () => { emit('text', { content: 'The answer' }); });
    await tick(500);
    expect(screen.getAllByTestId('assistant').map(node => node.textContent)).toEqual(['The answer']);
    await act(async () => { emit('text', { content: ' is complete' }); });
    await act(async () => { emit('turn_end'); stream.close(); });
    await tick(500);
    expect(screen.getAllByTestId('assistant').map(node => node.textContent)).toEqual(['The answer is complete']);
    expect(screen.getAllByTestId('user').map(node => node.textContent)).toEqual(['hello']);
  });

  it('does not erase a live reply when metadata refreshes with an empty transcript', async () => {
    const view = render(<Chat />);
    await tick();
    send('hello');
    await tick();
    await act(async () => { emit('turn_start'); });
    await act(async () => { emit('text', { content: 'Still writing' }); });
    await tick(500);
    context.active = { ...context.active!, messages: [] };
    context.sessions = [context.active];
    view.rerender(<Chat />);
    await tick();
    expect(screen.getByTestId('assistant').textContent).toBe('Still writing');
    expect(screen.getByTestId('user').textContent).toBe('hello');
    await act(async () => { emit('turn_end'); stream.close(); });
    await tick(500);
  });

  it('does not duplicate a live reply when a server snapshot arrives ahead of the stream', async () => {
    const view = render(<Chat />);
    await tick();
    send('hello');
    await tick();
    await act(async () => { emit('turn_start'); });
    await act(async () => { emit('text', { content: 'Partial' }); });
    await tick(500);
    context.active = { ...context.active!, messages: [
      { role: 'user', text: 'hello' }, { role: 'assistant', text: 'Partial answer' },
    ] };
    context.sessions = [context.active];
    view.rerender(<Chat />);
    await tick();
    await act(async () => { emit('text', { content: ' answer complete' }); });
    await act(async () => { emit('turn_end'); stream.close(); });
    await tick(500);
    expect(screen.getAllByTestId('assistant').map(node => node.textContent)).toEqual(['Partial answer complete']);
  });

  it('keeps the active conversation when the session list is reordered', async () => {
    const one = session('one', 'First conversation');
    const two = session('two', 'Second conversation');
    context.sessions = [one, two]; context.active = one;
    const view = render(<Chat />);
    await tick();
    expect(screen.getByTestId('user').textContent).toBe('First conversation');
    context.sessions = [two, one];
    view.rerender(<Chat />);
    await tick();
    expect(screen.getByTestId('user').textContent).toBe('First conversation');
    context.active = two;
    view.rerender(<Chat />);
    await tick();
    expect(screen.getByTestId('user').textContent).toBe('Second conversation');
    context.active = one;
    view.rerender(<Chat />);
    await tick();
    expect(screen.getByTestId('user').textContent).toBe('First conversation');
  });

  it('flushes a new message to the account cache when navigating before the debounce fires', async () => {
    const view = render(<Chat />);
    await tick();
    send('Do not lose this message');
    await tick();
    view.unmount();
    expect(context.patchSession).toHaveBeenCalledWith('one', {
      messages: [{ role: 'user', text: 'Do not lose this message' }],
    });
    await act(async () => { stream.close(); });
    await tick(500);
  });

  it('hydrates newer server history without changing the active session', async () => {
    context.active = session('one', 'First message');
    context.sessions = [context.active];
    const view = render(<Chat />);
    await tick();
    context.active = { ...context.active, messages: [
      ...context.active.messages, { role: 'assistant', text: 'Reply from another device' },
    ] };
    context.sessions = [context.active];
    view.rerender(<Chat />);
    await tick();
    expect(screen.getByTestId('assistant').textContent).toBe('Reply from another device');
  });

  it('sends to the originating project when the user switches sessions while attachment is pending', async () => {
    let attached!: (response: Response) => void;
    const originalFetch = vi.mocked(fetch).getMockImplementation()!;
    vi.mocked(fetch).mockImplementation((url, init) => String(url) === '/api/chat/attach'
      ? new Promise(resolve => { attached = resolve; }) : originalFetch(url, init));
    context.sessions = [session('one'), session('two')]; context.active = context.sessions[0];
    const view = render(<Chat />);
    await tick();
    send('work on project one');
    await tick();
    context.active = context.sessions[1];
    view.rerender(<Chat />);
    await tick();
    await act(async () => { attached(json({ attach_id: 'attachment' })); });
    const request = vi.mocked(fetch).mock.calls.find(([url]) => url === '/api/chat/send');
    expect(JSON.parse(String(request?.[1]?.body))).toMatchObject({ aimee_session_id: 'one', cwd: '/work/one' });
    await act(async () => {
      emit('turn_start'); emit('text', { content: 'Finished project one' }); emit('turn_end'); stream.close();
    });
    await tick(500);
    expect(screen.queryByTestId('assistant')).toBeNull();
    context.active = context.sessions[0];
    view.rerender(<Chat />);
    await tick();
    expect(screen.getByTestId('assistant').textContent).toBe('Finished project one');
  });
});
