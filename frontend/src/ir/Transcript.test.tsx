import { parseJSON } from './json';
// @vitest-environment jsdom
import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
afterEach(() => { cleanup(); vi.unstubAllGlobals(); });
import IRTranscript from './Transcript';
import { nativeOwner } from './interruption.fixture';
import { emptySession, reduceSession, type Envelope } from './session';
it.each([false, true])('presents rejected steering as a safe quoted diagnostic without changing canonical state (shortened=%s)', shortened => {
  const preview = '<img src=x onerror="alert(1)">\nKeep this as submitted text';
  const reason = 'Steering continuation was not dispatched; no model or tool executed. Send it again explicitly.';
  const error = JSON.stringify({ reason, dispatched: false, submitted_input_sha256: 'a'.repeat(64),
    submitted_input_bytes: new TextEncoder().encode(preview).length + (shortened ? 100 : 0),
    submitted_input_preview: preview, submitted_input_truncated: shortened });
  const state = { ...emptySession('s'), errors: [error] };
  const view = render(<IRTranscript state={state} />);
  expect(screen.getByText(reason)).toBeTruthy();
  expect(screen.getByText(shortened ? 'Submitted text (shortened):' : 'Submitted text:')).toBeTruthy();
  expect(view.container.querySelector('blockquote')?.textContent).toBe(preview);
  expect(view.container.querySelector('img')).toBeNull();
  expect(view.container.textContent).not.toContain('submitted_input_sha256');
  expect(view.container.textContent).not.toContain('a'.repeat(64));
  expect(state.errors).toEqual([error]);
  expect(state.messages).toEqual([]);
});
it('keeps ordinary and malformed steering diagnostics as ordinary error text', () => {
  const malformed = JSON.stringify({ reason: 'Steering continuation was not dispatched', dispatched: false });
  render(<IRTranscript state={{ ...emptySession('s'), errors: ['Ordinary failure', malformed] }} />);
  expect(screen.getByText('Ordinary failure')).toBeTruthy();
  expect(screen.getByText(malformed)).toBeTruthy();
});
it('displays authored message groups without changing persisted text fragments', () => {
  const envelope: Envelope = { version: 1, session_id: 's', sequence: '1', event_id: 'e', turn_id: 't', actor: 'alice',
    event: { kind: 'message', message_id: 'a', payload: { role: 'assistant', blocks: [
      { type: 'text', text: 'P', message_group: 1 }, { type: 'text', text: 'R.', message_group: 1 },
      { type: 'text', text: 'GitHub', message_group: 2 },
    ] } } };
  const state = reduceSession(emptySession('s'), envelope);
  render(<IRTranscript state={state} />);
  expect(screen.getByText('PR.')).toBeTruthy();
  expect(screen.getByText('GitHub')).toBeTruthy();
  expect(state.messages[0].blocks.map(block => block.text)).toEqual(['P', 'R.', 'GitHub']);
});

it('reports output-boundary loss while retaining the canonical reasoning', () => {
  const before = reduceSession(emptySession('s'), { version: 1, session_id: 's', sequence: '1', event_id: 'a', turn_id: 't', actor: 'alice',
    event: { kind: 'message', message_id: 'a', payload: { role: 'assistant', blocks: [{ type: 'thinking', text: 'retained reasoning', signature: 'opaque' }] } } });
  const envelope: Envelope = { version: 1, session_id: 's', sequence: '2', event_id: 'p', turn_id: 't', actor: 'alice',
    event: { kind: 'projection', payload: { model: 'model-b', boundary: 'request', omitted_reasoning_blocks: 1, omitted_opaque_state_blocks: 1 } } };
  const state = reduceSession(before, envelope);
  expect(state.messages).toBe(before.messages);
  expect(reduceSession(JSON.parse(JSON.stringify(before)), JSON.parse(JSON.stringify(envelope)))).toEqual(state);
  render(<IRTranscript state={state} />);
  expect(screen.getByRole('status').textContent).toContain("1 reasoning block remains in history and was omitted from model-b's input");
  expect(state.messages[0].blocks[0].signature).toBe('opaque');
});

it('shows cache projection independently of reasoning without changing history', () => {
  const initial = emptySession('s');
  const envelope: Envelope = { version: 1, session_id: 's', sequence: '1', event_id: 'cache', turn_id: 't', actor: 'alice',
    event: { kind: 'projection', payload: { model: 'model-b', boundary: 'request', omitted_reasoning_blocks: 0,
      omitted_opaque_state_blocks: 0, omitted_cache_controls: 3 } } };
  const state = reduceSession(initial, envelope);
  expect(state.messages).toBe(initial.messages);
  expect(reduceSession(emptySession('s'), JSON.parse(JSON.stringify(envelope)))).toEqual(state);
  render(<IRTranscript state={state} />);
  expect(screen.getByRole('status').textContent).toContain('3 requested cache settings were not applied for model-b');
  expect(screen.getByRole('status').textContent).not.toContain('0 reasoning');
  for (const count of [-1, 0, 0.5, Number.MAX_SAFE_INTEGER + 1]) {
    const invalid = { ...envelope, event: { ...envelope.event, payload: { ...envelope.event.payload, omitted_cache_controls: count } } };
    expect(() => reduceSession(emptySession('s'), invalid as Envelope)).toThrow('Invalid projection report');
  }
});

it('loads an expanded delegate from its private canonical log without adding it to parent messages', () => {
  class Stream {
    static instances: Stream[] = [];
    listener?: (event: MessageEvent) => void;
    onerror?: () => void;
    close = vi.fn();
    constructor(public url: string) { Stream.instances.push(this); }
    addEventListener(type: string, listener: (event: MessageEvent) => void) { if (type === 'ir') this.listener = listener; }
    emit(envelope: Envelope) { this.listener?.(new MessageEvent('ir', { data: JSON.stringify(envelope) })); }
  }
  vi.stubGlobal('EventSource', Stream);
  const parent = reduceSession(emptySession('parent'), { version: 1, session_id: 'parent', sequence: '1', event_id: 'ref', turn_id: 'parent-run', actor: 'alice',
    event: { kind: 'child_execution', payload: { session_id: 'private-child', turn_id: 'child-run', role: 'review', status: 'completed' } } });
  const view = render(<IRTranscript state={parent} />);
  expect(Stream.instances).toHaveLength(0);
  const details = screen.getByText('Delegate: review — completed').closest('details')!;
  details.open = true;
  fireEvent(details, new Event('toggle'));
  expect(Stream.instances).toHaveLength(1);
  const stream = Stream.instances[0];
  expect(stream.url).toContain('sid=private-child&cursor=0');
  const childEnvelope = (sequence: string, event: Envelope['event']): Envelope => ({ version: 1, session_id: 'private-child', sequence, event_id: `child-${sequence}`, turn_id: 'child-run', actor: 'alice', event });
  act(() => {
    stream.emit(childEnvelope('1', { kind: 'execution', payload: { status: 'running' } }));
    stream.emit(childEnvelope('2', { kind: 'message', message_id: 'child-answer', payload: { role: 'assistant', blocks: [{ type: 'text', text: 'Private delegate answer' }] } }));
    stream.emit(childEnvelope('3', { kind: 'execution', payload: { status: 'completed' } }));
  });
  expect(screen.getByText('Private delegate answer')).toBeTruthy();
  expect(parent.messages).toEqual([]);
  expect(stream.close).not.toHaveBeenCalled();
  view.unmount();
  expect(stream.close).toHaveBeenCalledTimes(1);
});

it('waits for child replay, routes its question to the child and retains a pending reply after collapse', async () => {
  class Stream {
    static instances: Stream[] = [];
    listeners = new Map<string, (event: MessageEvent) => void>();
    close = vi.fn();
    constructor(public url: string) { Stream.instances.push(this); }
    addEventListener(type: string, listener: (event: MessageEvent) => void) { this.listeners.set(type, listener); }
    emit(type: string, value: unknown) { this.listeners.get(type)?.(new MessageEvent(type, { data: JSON.stringify(value) })); }
  }
  vi.stubGlobal('EventSource', Stream);
  const onReply = vi.fn().mockResolvedValue(undefined);
  const parent = reduceSession(emptySession('parent'), { version: 1, session_id: 'parent', sequence: '1', event_id: 'ref', turn_id: 'p', actor: 'alice',
    event: { kind: 'child_execution', payload: { session_id: 'child', turn_id: 'c', role: 'review', status: 'running' } } });
  const view = render(<IRTranscript state={parent} onReply={onReply} />);
  const details = screen.getByText('Delegate: review — running').closest('details')!;
  act(() => { details.open = true; fireEvent(details, new Event('toggle')); });
  const envelope = (sequence: string, event: Envelope['event']): Envelope => ({ version: 1, session_id: 'child', sequence, event_id: `child-${sequence}`, turn_id: 'c', actor: 'alice', event });
  act(() => {
    Stream.instances[0].emit('ir', envelope('1', { kind: 'execution', payload: { status: 'running' } }));
    Stream.instances[0].emit('ir', envelope('2', { kind: 'control_request', request_id: 'c:q', control_kind: 'question', payload: { questions: [
      { id: 'answer', question: 'Child choice?', multiple: false, options: [] },
    ] } }));
  });
  expect(screen.queryByRole('button', { name: 'Send answers' })).toBeNull();
  act(() => Stream.instances[0].emit('replay_complete', { session_id: 'child', cursor: '2' }));
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Child answer' } });
  fireEvent.click(screen.getByRole('button', { name: 'Send answers' }));
  await waitFor(() => expect(onReply).toHaveBeenCalledWith('c:q', { decision: 'allow', answers: { answer: ['Child answer'] } }, 'child'));
  view.rerender(<IRTranscript state={parent} onReply={onReply} pendingRepliesBySession={{ child: new Set(['c:q']) }} />);
  act(() => { details.open = false; fireEvent(details, new Event('toggle')); });
  act(() => { details.open = true; fireEvent(details, new Event('toggle')); });
  expect(Stream.instances).toHaveLength(2);
  act(() => Stream.instances[1].emit('replay_complete', { session_id: 'child', cursor: '2' }));
  expect(screen.getByRole('button', { name: 'Send answers' })).toHaveProperty('disabled', true);
  act(() => {
    Stream.instances[1].emit('ir', envelope('3', { kind: 'control_response', request_id: 'c:q', control_kind: 'question', payload: { decision: 'allow', answers: { answer: ['Child answer'] } } }));
    Stream.instances[1].emit('ir', envelope('4', { kind: 'execution', payload: { status: 'completed' } }));
  });
  expect(screen.queryByRole('button')).toBeNull();
  expect(screen.getByText('question: Resolved')).toBeTruthy();
  expect(onReply).toHaveBeenCalledTimes(1);
  expect(parent.messages).toEqual([]);
});

it('refuses a child stream that returns another session', () => {
  let receive: ((event: MessageEvent) => void) | undefined;
  const close = vi.fn();
  vi.stubGlobal('EventSource', class {
    close = close;
    addEventListener(type: string, listener: (event: MessageEvent) => void) { if (type === 'ir') receive = listener; }
  });
  const parent = emptySession('parent');
  parent.children.child = { session_id: 'private-child', turn_id: 'child-run', role: 'review', status: 'running' };
  render(<IRTranscript state={parent} />);
  const details = screen.getByText('Delegate: review — running').closest('details')!;
  details.open = true;
  fireEvent(details, new Event('toggle'));
  act(() => receive?.(new MessageEvent('ir', { data: JSON.stringify({ version: 1, session_id: 'wrong-owner', sequence: '1', event_id: 'e', turn_id: 't', actor: 'alice', event: { kind: 'execution', payload: { status: 'completed' } } }) })));
  expect(screen.getByRole('alert').textContent).toContain('could not be read');
  expect(close).toHaveBeenCalled();
});

it('displays exact opaque numbers without quoting them or rounding tool arguments', () => {
  const state = reduceSession(emptySession('s'), { version: 1, session_id: 's', sequence: '1', event_id: 'a', turn_id: 't', actor: 'alice',
    event: { kind: 'message', message_id: 'a', payload: { role: 'assistant', blocks: [
      { type: 'tool_use', tool_id: 'call', name: 'read', input: parseJSON('{"id":9007199254740993}') },
    ] } } });
  const { container } = render(<IRTranscript state={state} />);
  expect(container.textContent).toContain('9007199254740993');
  expect(container.textContent).not.toContain('9007199254740992');
});

it('keeps reused native tool results on their own cards and shows ambiguous evidence', () => {
  const events: Envelope['event'][] = [
    { kind:'message',message_id:'a',payload:{role:'assistant',blocks:[{type:'tool_use',tool_id:'same',name:'firstTool',input:{}}]} },
    { kind:'message',message_id:'ra',payload:{role:'tool',blocks:[{type:'tool_result',tool_id:'same',content:'FIRST_ONLY'}]} },
    { kind:'message',message_id:'b',payload:{role:'assistant',blocks:[{type:'tool_use',tool_id:'same',name:'secondTool',input:{}}]} },
    { kind:'message',message_id:'rb',payload:{role:'tool',blocks:[{type:'tool_result',tool_id:'same',content:'SECOND_ONLY'}]} },
    { kind:'message',message_id:'orphan',payload:{role:'tool',blocks:[{type:'tool_result',tool_id:'unknown',content:'VISIBLE_ORPHAN'}]} },
    { kind:'message',message_id:'duplicates',payload:{role:'assistant',blocks:[{type:'tool_use',tool_id:'dup',name:'left'},{type:'tool_use',tool_id:'dup',name:'right'}]} },
    { kind:'message',message_id:'ambiguous',payload:{role:'tool',blocks:[{type:'tool_result',tool_id:'dup',content:'VISIBLE_AMBIGUOUS'}]} },
  ];
  const state = events.reduce((state,event,index) => reduceSession(state,{version:1,session_id:'s',sequence:String(index+1),event_id:String(index+1),turn_id:'t',actor:'alice',event}),emptySession('s'));
  render(<IRTranscript state={state} />);
  expect(screen.getAllByText(/FIRST_ONLY/)).toHaveLength(1);
  expect(screen.getAllByText(/SECOND_ONLY/)).toHaveLength(1);
  expect(screen.getByText('VISIBLE_ORPHAN')).toBeTruthy();
  expect(screen.getAllByText(/VISIBLE_AMBIGUOUS/)).toHaveLength(1);
});


it('bounds mounted canonical history while keeping current requests available', () => {
  let state = emptySession('windowed');
  for (let index = 1; index <= 200; index++) state = reduceSession(state, {
    version: 1, session_id: 'windowed', sequence: String(index), event_id: String(index), turn_id: `turn-${index}`, actor: 'alice',
    event: { kind: 'message', message_id: `message-${index}`, payload: { role: 'assistant', blocks: [{ type: 'text', text: `Message ${index}` }] } },
  });
  state = reduceSession(state, { version: 1, session_id: 'windowed', sequence: '201', event_id: 'pending', turn_id: 'older-turn', actor: 'alice',
    event: { kind: 'control_request', request_id: 'pending', control_kind: 'permission', payload: { title: 'Still needs your reply' } } });
  const view = render(<IRTranscript state={state} messageLimit={10} onReply={vi.fn()} />);
  expect(screen.queryByText('Message 190')).toBeNull();
  expect(screen.getAllByText(/^Message \d+$/)).toHaveLength(10);
  expect(screen.getByRole('button', { name: 'Allow once' })).toBeTruthy();
  view.rerender(<IRTranscript state={state} messageLimit={200} onReply={vi.fn()} />);
  expect(screen.getAllByText(/^Message \d+$/)).toHaveLength(200);
});

it('answers a durable free-text host question and resolves it without creating model messages', async () => {
  const envelope = (sequence: string, event: Envelope['event']): Envelope => ({
    version: 1, session_id: 'primary', sequence, event_id: `host-question-${sequence}`,
    turn_id: 'host-turn', actor: 'alice', event,
  });
  const state = reduceSession(emptySession('primary'), envelope('1', {
    kind: 'control_request', request_id: 'host-turn:question', control_kind: 'question',
    payload: { questions: [{ id: 'answer', question: 'Which workspace?', multiple: false, options: [] }] },
  }));
  const onReply = vi.fn().mockResolvedValue(undefined);
  const view = render(<IRTranscript state={state} onReply={onReply} />);
  expect(screen.getByRole('button', { name: 'Send answers' }).hasAttribute('disabled')).toBe(true);
  fireEvent.change(screen.getByRole('textbox', { name: 'Your answer: Which workspace?' }), {
    target: { value: 'aimee' },
  });
  fireEvent.click(screen.getByRole('button', { name: 'Send answers' }));
  await waitFor(() => expect(onReply).toHaveBeenCalledWith('host-turn:question', {
    decision: 'allow', answers: { answer: ['aimee'] },
  }));
  expect(onReply).toHaveBeenCalledTimes(1);
  const answered = reduceSession(state, envelope('2', {
    kind: 'control_response', request_id: 'host-turn:question', control_kind: 'question',
    payload: { decision: 'allow', answers: { answer: ['aimee'] } },
  }));
  view.rerender(<IRTranscript state={answered} onReply={onReply} />);
  expect(screen.getByText('question: Resolved')).toBeTruthy();
  expect(screen.queryByRole('button')).toBeNull();
  expect(answered.messages).toEqual([]);
});


it('does not carry question answers or sent state between sessions with reused request IDs', async () => {
  function question(session: string) {
    return reduceSession(emptySession(session), { version: 1, session_id: session, sequence: '1', event_id: 'request', turn_id: 'turn', actor: 'alice',
      event: { kind: 'control_request', request_id: 'same-id', control_kind: 'question', payload: { questions: [
        { id: 'answer', question: 'Your choice?', multiple: false, options: [] },
      ] } } });
  }
  const onReply = vi.fn().mockResolvedValue(undefined);
  const view = render(<IRTranscript state={question('one')} onReply={onReply} />);
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Only for session one' } });
  fireEvent.click(screen.getByRole('button', { name: 'Send answers' }));
  await act(async () => {});
  view.rerender(<IRTranscript state={question('two')} onReply={onReply} />);
  expect(screen.getByRole('textbox')).toHaveProperty('value', '');
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Only for session two' } });
  fireEvent.click(screen.getByRole('button', { name: 'Send answers' }));
  await act(async () => {});
  expect(onReply).toHaveBeenCalledTimes(2);
});

it('shows the interrupted call as unknown without inventing an output or a retry action', () => {
  const envelope = (sequence: string, event: Envelope['event']): Envelope => ({ version: 1, session_id: 's', sequence, event_id: `e${sequence}`, turn_id: 't', actor: 'operator', event });
  const state = [envelope('1', { kind: 'execution', payload: { status: 'running' } }),
    envelope('2', { kind: 'message', message_id: 'm', native_owner: nativeOwner(), payload: { role: 'assistant', blocks: [{ type: 'tool_use', tool_id: 'call', name: 'write_file', input: { path: 'file' } }] } }),
    envelope('3', { kind: 'control_request', request_id: 'old-permission', control_kind: 'permission', payload: {} }),
    envelope('4', { kind: 'execution', payload: { status: 'completed' } }),
    envelope('5', { kind: 'tool_interruption', payload: {
      version: 1, source: { session_id: 's', turn_id: 't', sequence: '2', event_id: 'e2', actor: 'operator', message_id: 'm', block_index: 0, tool_id: 'call', name: 'write_file' },
      owner: { owner_id: 'a'.repeat(32), binding_sha256: 'b'.repeat(64), containment_sha256: 'c'.repeat(64), epoch: '0' },
      terminal_sequence: '4', outcome: 'unknown', retry: 'explicit_permission_required' } }),
  ].reduce(reduceSession, emptySession('s'));
  const reply = vi.fn();
  render(<IRTranscript state={state} onReply={reply} />);
  expect(screen.getByText('Tool: write_file — outcome unknown')).toBeTruthy();
  expect(screen.getByRole('note').textContent).toContain('Its effects are unknown');
  expect(screen.getByRole('note').textContent).toContain('explicit user authorization');
  expect(screen.getByRole('note').getAttribute('aria-live')).toBe('off');
  expect(screen.queryByRole('alert')).toBeNull();
  expect(screen.queryByText('Output')).toBeNull();
  expect(screen.queryByText('Running: write_file')).toBeNull();
  expect(screen.queryByText('Turn cancelled')).toBeNull();
  expect(screen.queryByRole('button', { name: /allow|retry/i })).toBeNull();
  expect(reply).not.toHaveBeenCalled();
  expect(state.messages).toHaveLength(1);
});

it('keeps an expanded delegate stream for an interruption receipt after its real terminal', () => {
  let receive: ((event: MessageEvent) => void) | undefined;
  const close = vi.fn();
  vi.stubGlobal('EventSource', class { close = close;
    addEventListener(type: string, listener: (event: MessageEvent) => void) { if (type === 'ir') receive = listener; }
  });
  const parent = reduceSession(emptySession('parent'), { version: 1, session_id: 'parent', sequence: '1', event_id: 'ref', turn_id: 'p', actor: 'operator',
    event: { kind: 'child_execution', payload: { session_id: 's', turn_id: 't', role: 'build', status: 'completed' } } });
  const view = render(<IRTranscript state={parent} />);
  const details = screen.getByText('Delegate: build — completed').closest('details')!;
  details.open = true;
  fireEvent(details, new Event('toggle'));
  const emit = (n: number, event: Envelope['event']) => act(() => receive?.(new MessageEvent('ir', { data: JSON.stringify({ version: 1, session_id: 's', sequence: String(n), event_id: `e${n}`, turn_id: 't', actor: 'operator', event }) })));
  emit(1, { kind: 'execution', payload: { status: 'running' } });
  emit(2, { kind: 'message', message_id: 'm', native_owner: nativeOwner(), payload: { role: 'assistant', blocks: [{ type: 'tool_use', tool_id: 'call', name: 'write_file' }] } });
  emit(3, { kind: 'execution', payload: { status: 'completed' } });
  expect(close).not.toHaveBeenCalled();
  const interruption = { kind: 'tool_interruption' as const, payload: {
    version: 1 as const, source: { session_id: 's', turn_id: 't', sequence: '2', event_id: 'e2', actor: 'operator', message_id: 'm', block_index: 0, tool_id: 'call', name: 'write_file' },
    owner: { owner_id: 'a'.repeat(32), binding_sha256: 'b'.repeat(64), containment_sha256: 'c'.repeat(64), epoch: '0' },
    terminal_sequence: '3', outcome: 'unknown' as const, retry: 'explicit_permission_required' as const } };
  emit(4, interruption);
  emit(4, interruption); // Exact replay must not duplicate an annotation or action.
  expect(screen.getAllByText('Tool: write_file — outcome unknown')).toHaveLength(1);
  expect(screen.getAllByRole('note')).toHaveLength(1);
  expect(parent.messages).toHaveLength(0);
  view.unmount();
  expect(close).toHaveBeenCalledTimes(1);
});
