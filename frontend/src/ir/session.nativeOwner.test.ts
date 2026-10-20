import { expect, it } from 'vitest';
import { parseJSON } from './json';
import { nativeOwner, receipt } from './interruption.fixture';
import { emptySession, reduceSession, toolBlockKey, type Block, type Envelope, type SessionEvent } from './session';
const envelope = (n: number, event: SessionEvent): Envelope => ({ version: 1, session_id: 's', sequence: String(n), event_id: `e${n}`, turn_id: 't', actor: 'operator', event });
const call: Block = { type: 'tool_use', tool_id: 'call', name: 'write_file', input: { path: 'file' } };
const snapshot = (blocks: Block[], partial = true): SessionEvent => ({ kind: 'message', message_id: 'm', native_owner: nativeOwner(), ...(partial ? { partial: true } : {}), payload: { role: 'assistant', blocks } });
it('keeps a standalone protected partial snapshot incomplete until an actual lifecycle terminal', () => {
  let state = reduceSession(emptySession('s'), envelope(1, { kind: 'execution', payload: { status: 'running' } }));
  state = reduceSession(state, envelope(2, snapshot([call])));
  expect(state.messages[0].complete).toBe(false);
  expect(state.messages[0].partial).toBe(true);
  expect(() => reduceSession(state, envelope(3, { kind: 'execution', payload: { status: 'completed' } }))).toThrow('open messages');
  const closed = reduceSession(state, envelope(3, { kind: 'execution', payload: { status: 'cancelled' } }));
  expect(closed.messages[0].complete).toBe(true);
  expect(closed.messages[0].partial).toBe(true);
  expect(closed.messages[0].blocks).toEqual([call]);
  expect(closed.toolInterruptions).toEqual({});
});
it('retains the first protected tool source through prefix expansion and a final snapshot', () => {
  let state = reduceSession(emptySession('s'), envelope(1, { kind: 'execution', payload: { status: 'running' } }));
  state = reduceSession(state, envelope(2, snapshot([call])));
  const key = toolBlockKey(state.messages[0], 0);
  const original = state.toolSources[key];
  state = reduceSession(state, envelope(3, snapshot([call, { type: 'text', text: 'A closed continuation.' }])));
  state = reduceSession(state, envelope(4, snapshot([call, { type: 'text', text: 'A closed continuation.' }], false)));
  expect(state.toolSources[key]).toBe(original);
  expect(state.messages[0].partial).toBe(false);
  state = reduceSession(state, envelope(5, { kind: 'execution', payload: { status: 'completed' } }));
  const value = { ...receipt(), terminal_sequence: '5' };
  state = reduceSession(state, envelope(6, { kind: 'tool_interruption', payload: value }));
  expect(state.toolInterruptions[key].source.sequence).toBe('2');
  const finalRef = { ...value, source: { ...value.source, sequence: '4', event_id: 'e4' } };
  expect(() => reduceSession({ ...state, sequence: '5', toolInterruptions: {} }, envelope(6, { kind: 'tool_interruption', payload: finalRef }))).toThrow('matching source');
});
it('merges a closed prefix without losing a later streaming block or its argument fragments', () => {
  const events: SessionEvent[] = [
    { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'TURN_START' } },
    { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'BLOCK_START', block_id: 0, kind: 'tool_use', tool_id: 'call', name: 'write_file', input: { path: 'file' } } },
    { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'BLOCK_STOP', block_id: 0 } },
    { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'BLOCK_START', block_id: 1, kind: 'tool_use', tool_id: 'later', name: 'read_file' } },
    { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'BLOCK_DELTA', block_id: 1, kind: 'tool_use', tool_args_delta: '{"path":' } },
  ];
  const before = events.reduce((state, event, index) => reduceSession(state, envelope(index + 1, event)), emptySession('s'));
  const after = reduceSession(before, envelope(6, snapshot([call])));
  expect(after.messages[0].blocks[1]).toBe(before.messages[0].blocks[1]);
  expect(after.messages[0].argumentFragments).toEqual({ 1: '{"path":' });
  expect(after.messages[0].stoppedBlocks).toEqual([0]);
  expect(after.messages[0].complete).toBe(false);
  expect(after.toolSources[toolBlockKey(after.messages[0], 1)]).toBeUndefined();
  expect(reduceSession(after, envelope(7, { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'BLOCK_DELTA', block_id: 1, kind: 'tool_use', tool_args_delta: '"README.md"}' } })).messages[0].argumentFragments[1]).toBe('{"path":"README.md"}');
});
it('refuses ownerless replacement, changed owners, altered protected calls and truncated prefixes', () => {
  const before = reduceSession(emptySession('s'), envelope(1, snapshot([call])));
  const altered: SessionEvent[] = [
    { kind: 'message', message_id: 'm', payload: { role: 'assistant', blocks: [call] } },
    { ...snapshot([call]), native_owner: { ...nativeOwner(), owner_id: 'd'.repeat(32) } } as SessionEvent,
    snapshot([{ ...call, tool_id: 'replacement' }]), snapshot([{ ...call, name: 'run_cmd' }]),
    snapshot([{ ...call, namespace: 'new' }]), snapshot([{ ...call, input: { path: 'other' } }]), snapshot([]),
  ];
  for (const event of altered) expect(() => reduceSession(before, envelope(2, event))).toThrow(/snapshot/);
  for (const event of [
    { kind: 'message', message_id: 'm', partial: true, payload: { role: 'assistant', blocks: [call] } },
    { ...snapshot([call]), partial: false }, { ...snapshot([call]), native_owner: null },
    { ...snapshot([call]), payload: { role: 'user', blocks: [call] } },
  ]) expect(() => reduceSession(emptySession('s'), envelope(1, event as SessionEvent))).toThrow(/snapshot/);
});
it('keeps opaque argument precision while comparing protected snapshots independent of object key order', () => {
  const first = { ...call, input: parseJSON('{"number":9007199254740993,"path":"file"}') };
  const equivalent = { ...call, input: parseJSON('{"path":"file","number":9007199254740993}') };
  const before = reduceSession(emptySession('s'), envelope(1, snapshot([first])));
  expect(reduceSession(before, envelope(2, snapshot([equivalent]))).sequence).toBe('2');
  expect(() => reduceSession(before, envelope(2, snapshot([{ ...call, input: parseJSON('{"path":"file","number":9007199254740992}') }])))).toThrow('closed block');
});
it('refuses interruption claims for unowned sources or a different native resource', () => {
  const event = { kind: 'message' as const, message_id: 'm', payload: { role: 'assistant', blocks: [call] } };
  const before = [envelope(1, { kind: 'execution', payload: { status: 'running' } }), envelope(2, event),
    envelope(3, { kind: 'projection', payload: { model: 'm', boundary: 'request', omitted_reasoning_blocks: 1, omitted_opaque_state_blocks: 0 } }),
    envelope(4, { kind: 'execution', payload: { status: 'completed' } })].reduce(reduceSession, emptySession('s'));
  expect(() => reduceSession(before, envelope(5, { kind: 'tool_interruption', payload: receipt() }))).toThrow('matching source');
  const owned = [envelope(1, { kind: 'execution', payload: { status: 'running' } }), envelope(2, snapshot([call], false)),
    envelope(3, { kind: 'projection', payload: { model: 'm', boundary: 'request', omitted_reasoning_blocks: 1, omitted_opaque_state_blocks: 0 } }),
    envelope(4, { kind: 'execution', payload: { status: 'completed' } })].reduce(reduceSession, emptySession('s'));
  for (const field of ['owner_id', 'binding_sha256', 'epoch', 'containment_sha256'] as const) {
    const value = receipt(); value.owner[field] = field === 'owner_id' || field === 'epoch' ? 'd'.repeat(32) : 'd'.repeat(64);
    expect(() => reduceSession(owned, envelope(5, { kind: 'tool_interruption', payload: value }))).toThrow('matching source');
  }
});

it('allows final host context/binding classification without rewriting original reasoning or call identity', () => {
  const reasoning: Block = { type: 'thinking', text: 'retained reasoning', signature: 'original signature', encrypted_content: 'opaque' };
  const before = reduceSession(emptySession('s'), envelope(1, snapshot([reasoning, call])));
  const digest = 'f'.repeat(64);
  const classified: Block = { ...reasoning, thinking_binding: `ap1:${digest}:${digest}:${digest}`,
    context: { origin: 1, authority: 0, trust: 4, sensitivity: 1, model_visible: true } };
  const final = snapshot([classified, { ...call, context: { origin: 1, authority: 0 } }], false);
  const after = reduceSession(before, envelope(2, final));
  expect(after.messages[0].blocks[0]).toEqual(classified);
  expect(after.toolSources[toolBlockKey(after.messages[0], 1)]).toBe(before.toolSources[toolBlockKey(before.messages[0], 1)]);
  for (const change of [{ text: 'rewritten' }, { signature: 'new signature' }, { encrypted_content: 'replacement' }])
    expect(() => reduceSession(before, envelope(2, snapshot([{ ...classified, ...change }, call], false)))).toThrow('closed block');
});
it.each([false, true])('uses actual TURN_STOP completion rather than inferring it from a partial/final snapshot (stopped=%s)', stopped => {
  const events: SessionEvent[] = [
    { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'TURN_START' } },
    { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'BLOCK_START', block_id: 0, kind: 'tool_use', tool_id: 'call', name: 'write_file', input: { path: 'file' } } },
    { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'BLOCK_STOP', block_id: 0 } },
  ];
  if (stopped) events.push({ kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'TURN_STOP' } });
  let state = events.reduce((state, event, index) => reduceSession(state, envelope(index + 1, event)), emptySession('s'));
  let sequence = events.length;
  state = reduceSession(state, envelope(++sequence, snapshot([call])));
  expect(state.messages[0].complete).toBe(stopped);
  state = reduceSession(state, envelope(++sequence, snapshot([call], false)));
  expect(state.messages[0].complete).toBe(stopped);
  if (!stopped) state = reduceSession(state, envelope(++sequence, { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'TURN_STOP' } }));
  expect(state.messages[0].complete).toBe(true);
});
