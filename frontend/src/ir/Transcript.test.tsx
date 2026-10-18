import { parseJSON } from './json';
// @vitest-environment jsdom
import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
afterEach(() => { cleanup(); vi.unstubAllGlobals(); });
import IRTranscript from './Transcript';
import { emptySession, reduceSession, type Envelope } from './session';
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

it('loads an expanded delegate from its private canonical log without adding it to parent messages', () => {
  class Stream {
    static instances: Stream[] = [];
    listener?: (event: MessageEvent) => void;
    onerror?: () => void;
    close = vi.fn();
    constructor(public url: string) { Stream.instances.push(this); }
    addEventListener(type: string, listener: (event: MessageEvent) => void) { expect(type).toBe('ir'); this.listener = listener; }
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
    stream.emit(childEnvelope('1', { kind: 'message', message_id: 'child-answer', payload: { role: 'assistant', blocks: [{ type: 'text', text: 'Private delegate answer' }] } }));
    stream.emit(childEnvelope('2', { kind: 'execution', payload: { status: 'completed' } }));
  });
  expect(screen.getByText('Private delegate answer')).toBeTruthy();
  expect(parent.messages).toEqual([]);
  expect(stream.close).toHaveBeenCalled();
  view.unmount();
});

it('refuses a child stream that returns another session', () => {
  let receive: ((event: MessageEvent) => void) | undefined;
  const close = vi.fn();
  vi.stubGlobal('EventSource', class {
    close = close;
    addEventListener(_type: string, listener: (event: MessageEvent) => void) { receive = listener; }
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
