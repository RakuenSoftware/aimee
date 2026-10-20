import { parseJSON, stringifyJSON } from './json';
import { describe, expect, it } from 'vitest';
import { emptySession, reduceSession, toolResults, toolBlockKey } from './session';
import type { Block, Envelope, SessionEvent } from './session';
const envelope = (n: number, event: SessionEvent): Envelope => ({ version: 1, session_id: 's', sequence: String(n), event_id: `e${n}`, turn_id: 't', actor: 'operator', event });
const message = (id: string, role: string, blocks: Block[]): SessionEvent => ({ kind: 'message', message_id: id, payload: { role, blocks } });

describe('canonical session projection', () => {
  it('advances replay-proof cursors without creating model history', () => {
    const digest = 'a'.repeat(64);
    const proof = { scope: digest, block_digest: digest, wire: 'anthropic' as const,
      thinking_binding: `ap1:${digest}:${digest}:${digest}` };
    const first = reduceSession(emptySession('s'), envelope(1, { kind: 'replay_proof', payload: proof }));
    expect(first).toEqual({ ...emptySession('s'), sequence: '1' });
    const replay = JSON.parse(JSON.stringify(envelope(1, { kind: 'replay_proof', payload: proof }))) as Envelope;
    expect(reduceSession(emptySession('s'), replay)).toEqual(first);
    for (const invalid of [
      { ...proof, scope: '' }, { ...proof, block_digest: 'unknown' },
      { ...proof, thinking_binding: null }, { ...proof, wire: 'other' },
      { ...proof, prompt: 'unmodeled content' },
    ]) expect(() => reduceSession(emptySession('s'), envelope(1, { kind: 'replay_proof', payload: invalid } as SessionEvent))).toThrow();
  });
  it('renders live and replay identically, pairing interleaved tools by opaque ID', () => {
    const events = [
      envelope(1, message('u', 'user', [{ type: 'text', text: 'Investigate' }])),
      envelope(2, message('a', 'assistant', [
        { type: 'thinking', text: 'Reason', signature: 'opaque', thinking_id: 'rs_1', encrypted_content: 'cipher', redacted_content: 'redacted', cache_control: 'ephemeral' },
        { type: 'tool_use', tool_id: 'b', name: 'read_file', input: { path: 'b' } },
        { type: 'tool_use', tool_id: 'a', name: 'read_file', input: { path: 'a' } },
      ])),
      envelope(3, message('ta', 'tool', [{ type: 'tool_result', tool_id: 'a', content: 'A' }])),
      envelope(4, message('tb', 'tool', [{ type: 'tool_result', tool_id: 'b', content: 'B' }])),
      envelope(5, message('attachments', 'user', [
        { type: 'image', media_type: 'image/png', media_ref: 'artifact:png' },
        { type: 'document', media_type: 'application/pdf', media_ref: 'artifact:pdf' },
      ])),
    ];
    const live = events.reduce(reduceSession, emptySession('s'));
    expect((JSON.parse(JSON.stringify(events)) as Envelope[]).reduce(reduceSession, emptySession('s'))).toEqual(live);
    expect(toolResults(live).byCall.get(toolBlockKey(live.messages[1], 2))?.[0].content).toBe('A');
    expect(toolResults(live).byCall.get(toolBlockKey(live.messages[1], 1))?.[0].content).toBe('B');
    expect(live.messages[1].blocks[0].signature).toBe('opaque');
    expect(live.messages[1].blocks[0].encrypted_content).toBe('cipher');
    expect(live.messages[1].blocks[0].redacted_content).toBe('redacted');
    expect(live.messages[1].blocks[0].cache_control).toBe('ephemeral');
    expect(reduceSession(live, events[4])).toBe(live);
  });
  it('retains stop reason and cache usage for buffered typed responses', () => {
    const events = [
      envelope(1, { kind: 'delta', message_id: 'buffered', role: 'assistant', payload: { type: 'TURN_START' } }),
      envelope(2, message('buffered', 'assistant', [{ type: 'text', text: 'partial answer' }])),
      envelope(3, { kind: 'delta', message_id: 'buffered', role: 'assistant', payload: {
        type: 'TURN_STOP', stop_reason: 'max_tokens', raw_stop_reason: 'length', usage_in: 12, usage_out: 7,
        usage_cache_read: 9, usage_cache_write: 3, usage_reasoning: 2,
      } }),
    ];
    const state = events.reduce(reduceSession, emptySession('s'));
    expect(state.messages[0].stopReason).toBe('max_tokens');
    expect(state.messages[0].nativeStopReason).toBe('length');
    expect(state.messages[0].usage).toEqual({ input: 12, output: 7, cacheRead: 9, cacheWrite: 3, reasoning: 2 });
    expect(JSON.parse(JSON.stringify(events)).reduce(reduceSession, emptySession('s'))).toEqual(state);
  });
  it('accumulates tool JSON fragments and finalizes them only at block stop', () => {
    const delta = (n: number, payload: import('./session').Delta) => envelope(n, { kind: 'delta', message_id: 'm', role: 'assistant', payload });
    const events = [delta(1, { type: 'TURN_START' }), delta(2, { type: 'BLOCK_START', block_id: 0, kind: 'tool_use', tool_id: 'call', name: 'bash' }),
      delta(3, { type: 'BLOCK_DELTA', block_id: 0, kind: 'tool_use', tool_args_delta: '{"command":' }),
      delta(4, { type: 'BLOCK_DELTA', block_id: 0, kind: 'tool_use', tool_args_delta: '"pwd"}' }),
      delta(5, { type: 'BLOCK_STOP', block_id: 0 }), delta(6, { type: 'TURN_STOP' })];
    const partial = events.slice(0, 4).reduce(reduceSession, emptySession('s'));
    expect(partial.messages[0].blocks[0].input).toBeUndefined();
    const state = events.slice(4).reduce(reduceSession, partial);
    expect(state.messages[0].blocks[0].input).toEqual({ command: 'pwd' });
    expect(state.messages[0].complete).toBe(true);
    expect(state.activeTurns).toEqual([]);
  });
  it('correlates harness controls without manufacturing model messages', () => {
    const state = reduceSession(emptySession('s'), envelope(1, { kind: 'control_request', request_id: 'p', control_kind: 'permission', payload: { options: ['allow', 'deny'] } }));
    expect(state.messages).toEqual([]);
    const response = { kind: 'control_response', request_id: 'p', control_kind: 'permission', payload: { decision: 'deny' } } as const;
    expect(reduceSession(state, envelope(2, response)).controls.p.response).toEqual({ decision: 'deny' });
    expect(() => reduceSession(emptySession('s'), envelope(1, response))).toThrow('matching pending request');
  });
  it('rejects foreign sessions, gaps, unknown content and changed message identity', () => {
    const e = envelope(1, message('m', 'assistant', [{ type: 'text', text: 'Hello' }]));
    expect(() => reduceSession(emptySession('other'), e)).toThrow('envelope');
    expect(() => reduceSession(emptySession('s'), { ...e, sequence: '2' })).toThrow('gap');
    expect(() => reduceSession(emptySession('s'), envelope(1, message('m', 'assistant', [{ type: 'native' } as never])))).toThrow('Unsupported');
    const state = reduceSession(emptySession('s'), e);
    expect(() => reduceSession(state, envelope(2, message('m', 'user', [])))).toThrow('identity');
  });
  it('keeps exact replay sequences beyond JavaScript number precision', () => {
    const state = { ...emptySession('s'), sequence: '9007199254740992' };
    expect(reduceSession(state, { ...envelope(1, message('m', 'user', [])), sequence: '9007199254740993' }).sequence).toBe('9007199254740993');
  });
});

it('keeps host execution running across model-response stops and tool results', () => {
  let state = reduceSession(emptySession('s'), envelope(1, { kind: 'execution', payload: { status: 'running' } }));
  state = reduceSession(state, envelope(2, { kind: 'delta', message_id: 'model', role: 'assistant', payload: { type: 'TURN_START' } }));
  state = reduceSession(state, envelope(3, { kind: 'delta', message_id: 'model', role: 'assistant', payload: { type: 'TURN_STOP' } }));
  expect(state.activeTurns).toEqual(['t']);
  state = reduceSession(state, envelope(4, message('result', 'tool', [{ type: 'tool_result', tool_id: 'call', content: 'done' }])));
  expect(state.activeTurns).toEqual(['t']);
  state = reduceSession(state, envelope(5, { kind: 'execution', payload: { status: 'completed' } }));
  expect(state.activeTurns).toEqual([]);
  expect(() => reduceSession(state, envelope(6, { kind: 'execution', payload: { status: 'running' } }))).toThrow('already terminal');
});

it('rejects deltas after message completion and premature message stops', () => {
  const d = (n: number, payload: import('./session').Delta) => envelope(n, { kind: 'delta', message_id: 'm', role: 'assistant', payload });
  let state = reduceSession(emptySession('s'), d(1, { type: 'TURN_START' }));
  expect(() => reduceSession(state, d(2, { type: 'TURN_START' }))).toThrow('Duplicate');
  state = reduceSession(state, d(2, { type: 'BLOCK_START', block_id: 0, kind: 'text' }));
  expect(() => reduceSession(state, d(3, { type: 'TURN_STOP' }))).toThrow('open blocks');
  state = reduceSession(state, d(3, { type: 'BLOCK_STOP', block_id: 0, kind: 'text' }));
  state = reduceSession(state, d(4, { type: 'TURN_STOP' }));
  expect(() => reduceSession(state, d(5, { type: 'BLOCK_DELTA', block_id: 0, kind: 'text', text_delta: 'late' }))).toThrow('completion');
});


it('closes partial messages and unresolved controls when execution is cancelled', () => {
  let state = reduceSession(emptySession('s'), envelope(1, { kind: 'execution', payload: { status: 'running' } }));
  state = reduceSession(state, envelope(2, { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'TURN_START' } }));
  state = reduceSession(state, envelope(3, { kind: 'control_request', request_id: 'p', control_kind: 'permission', payload: {} }));
  state = reduceSession(state, envelope(4, { kind: 'execution', payload: { status: 'cancelled' } }));
  expect(state.messages[0]).toMatchObject({ complete: true, cancelled: true });
  expect(state.controls.p.closed).toBe(true);
  expect(state.activeTurns).toEqual([]);
  expect(() => reduceSession(state, envelope(5, { kind: 'control_response', request_id: 'p', control_kind: 'permission', payload: { decision: 'allow' } }))).toThrow('pending request');
});

it('retains a failed model response while its host handles the failure', () => {
  let state = reduceSession(emptySession('s'), envelope(1, { kind: 'execution', payload: { status: 'running' } }));
  state = reduceSession(state, envelope(2, { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'TURN_START' } }));
  state = reduceSession(state, envelope(3, { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'ERROR', error_message: 'Provider failed' } }));
  expect(state.messages[0]).toMatchObject({ complete: true, error: 'Provider failed' });
  expect(state.activeTurns).toEqual(['t']);
  state = reduceSession(state, envelope(4, { kind: 'execution', payload: { status: 'failed', error: 'Retry budget exhausted' } }));
  expect(state.activeTurns).toEqual([]);
  expect(state.errors).toEqual(['Provider failed', 'Retry budget exhausted']);
});


it('preserves usage when a final typed snapshot replaces streamed content', () => {
  let state = reduceSession(emptySession('s'), envelope(1, { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'TURN_START' } }));
  state = reduceSession(state, envelope(2, { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'TURN_STOP', stop_reason: 'end_turn', usage_in: 123, usage_out: 45 } }));
  state = reduceSession(state, envelope(3, message('m', 'assistant', [{ type: 'thinking', text: 'Reason', signature: 'opaque' }])));
  expect(state.messages[0].usage).toEqual({ input: 123, output: 45 });
  expect(state.messages[0].stopReason).toBe('end_turn');
});

it('rejects cross-turn control replies and content arriving after cancellation', () => {
  let state = reduceSession(emptySession('s'), envelope(1, { kind: 'execution', payload: { status: 'running' } }));
  state = reduceSession(state, envelope(2, { kind: 'control_request', request_id: 'p', control_kind: 'permission', payload: {} }));
  expect(() => reduceSession(state, { ...envelope(3, { kind: 'control_response', request_id: 'p', control_kind: 'permission', payload: {} }), turn_id: 'other' })).toThrow('pending request');
  state = reduceSession(state, envelope(3, { kind: 'execution', payload: { status: 'cancelled' } }));
  expect(() => reduceSession(state, envelope(4, message('late', 'assistant', [{ type: 'text', text: 'late' }])))).toThrow('execution completion');
});

it('advances past host history checkpoints without duplicating the observation timeline', () => {
  let state = reduceSession(emptySession('s'), envelope(1, {
    kind: 'message', message_id: 'visible', payload: { role: 'assistant', blocks: [{ type: 'text', text: 'visible' }] },
  }));
  state = reduceSession(state, envelope(2, { kind: 'history', payload: {
    scope: 'delegate:child', phase: 'message', index: 0,
    message: { role: 'user', blocks: [{ type: 'text', text: 'private child prompt' }] },
  } }));
  state = reduceSession(state, envelope(3, { kind: 'history', payload: {
    scope: 'delegate:child', phase: 'commit', count: 1,
  } }));
  expect(state.sequence).toBe('3');
  expect(state.messages.map(message => message.blocks[0].text)).toEqual(['visible']);
  expect(() => reduceSession(state, envelope(4, { kind: 'history', payload: {
    scope: '', phase: 'commit', count: 0,
  } }))).toThrow('Invalid history');
});


it('preserves initial content, opaque thinking state and complete tool inputs during stream replay', () => {
  const d = (n: number, payload: import('./session').Delta) => envelope(n, { kind: 'delta', message_id: 'm', role: 'assistant', payload });
  const events = [d(1, { type: 'TURN_START' }),
    d(2, { type: 'BLOCK_START', block_id: 0, kind: 'thinking', text_delta: 'initial', signature_delta: 'sig', redacted_content: 'opaque' }),
    d(3, { type: 'BLOCK_DELTA', block_id: 0, kind: 'thinking', text_delta: ' thought', signature_delta: 'nature' }),
    d(4, { type: 'BLOCK_STOP', block_id: 0 }),
    d(5, { type: 'BLOCK_START', block_id: 1, kind: 'tool_use', tool_id: 'call', name: 'read', namespace: 'files', input: { path: 'a' } }),
    d(6, { type: 'BLOCK_STOP', block_id: 1 }), d(7, { type: 'TURN_STOP' })];
  const state = events.reduce(reduceSession, emptySession('s'));
  expect(state.messages[0].blocks[0]).toMatchObject({ text: 'initial thought', signature: 'signature', redacted_content: 'opaque' });
  expect(state.messages[0].blocks[1]).toMatchObject({ namespace: 'files', input: { path: 'a' } });
  expect(JSON.parse(JSON.stringify(events)).reduce(reduceSession, emptySession('s'))).toEqual(state);
  const partial = events.slice(0, 5).reduce(reduceSession, emptySession('s'));
  expect(() => reduceSession(partial, d(6, { type: 'BLOCK_DELTA', block_id: 1, tool_args_delta: '{}' }))).toThrow('complete initial input');
});

it('keeps child references separate from parent model history and replays terminal state', () => {
  const reference = { session_id: 'private-child', turn_id: 'child-run', role: 'review', status: 'running' as const };
  const events = [envelope(1, { kind: 'child_execution', payload: reference }),
    envelope(2, { kind: 'child_execution', payload: { ...reference, status: 'completed' } })];
  const state = events.reduce(reduceSession, emptySession('s'));
  expect(state.messages).toEqual([]);
  expect(state.executions).toEqual({});
  expect(state.children['private-child:child-run'].status).toBe('completed');
  expect(JSON.parse(JSON.stringify(events)).reduce(reduceSession, emptySession('s'))).toEqual(state);
  expect(() => reduceSession(state, envelope(3, { kind: 'child_execution', payload: reference }))).toThrow(/terminal/);
  expect(() => reduceSession(emptySession('s'), envelope(1, { kind: 'child_execution', payload: { ...reference, session_id: 's' } }))).toThrow(/reference/);
});

 it('retains opaque decimals identically in message snapshots and completed tool fragments', () => {
  const argumentsJSON = '{"id":9007199254740993,"ratio":0.12345678901234567890123456789}';
  const events = [
    envelope(1, { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'TURN_START' } }),
    envelope(2, { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'BLOCK_START', block_id: 0, kind: 'tool_use', tool_id: 'call', name: 'read' } }),
    envelope(3, { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'BLOCK_DELTA', block_id: 0, kind: 'tool_use', tool_args_delta: argumentsJSON } }),
    envelope(4, { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'BLOCK_STOP', block_id: 0 } }),
    envelope(5, { kind: 'delta', message_id: 'm', role: 'assistant', payload: { type: 'TURN_STOP' } }),
  ];
  const live = events.reduce(reduceSession, emptySession('s'));
  const replay = events.map(event => parseJSON(stringifyJSON(event)!) as Envelope).reduce(reduceSession, emptySession('s'));
  expect(replay).toEqual(live);
  expect(stringifyJSON(live.messages[0].blocks[0].input)).toBe(argumentsJSON);
  const snapshot = reduceSession(emptySession('s'), envelope(1, message('m', 'assistant', [
    { type: 'tool_use', tool_id: 'call', name: 'read', input: parseJSON(argumentsJSON) },
  ])));
  expect(snapshot.messages[0].blocks[0].input).toEqual(live.messages[0].blocks[0].input);
});

it('retains and validates thinking replay provenance without placing it in visible text', () => {
 const binding = `ap1:${'a'.repeat(64)}:${'b'.repeat(64)}:${'c'.repeat(64)}`;
 const event = envelope(1, message('m', 'assistant', [{type:'thinking',text:'reason',signature:'opaque',thinking_binding:binding}]));
 const state = reduceSession(emptySession('s'), event);
 expect(state.messages[0].blocks[0].thinking_binding).toBe(binding);
 for (const bad of [binding+'a', binding.replace('ap1:', 'ap2:')])
  expect(() => reduceSession(emptySession('s'), envelope(1,message('m','assistant',[{type:'thinking',text:'reason',thinking_binding:bad}])))).toThrow('replay binding');
});

it('preserves native IDs while deriving distinct results for reused tool cycles', () => {
  const events = [
    envelope(1, message('first', 'assistant', [{ type: 'tool_use', tool_id: 'same', name: 'read', input: {} }])),
    envelope(2, message('result1', 'tool', [{ type: 'tool_result', tool_id: 'same', content: 'FIRST' }])),
    envelope(3, message('second', 'assistant', [{ type: 'tool_use', tool_id: 'same', name: 'read', input: {} }])),
    envelope(4, message('result2', 'tool', [{ type: 'tool_result', tool_id: 'same', content: 'SECOND' }])),
  ];
  const state = events.reduce(reduceSession, emptySession('s'));
  const original = stringifyJSON(state);
  const results = toolResults(state);
  expect(results.byCall.get(toolBlockKey(state.messages[0], 0))?.map(block => block.content)).toEqual(['FIRST']);
  expect(results.byCall.get(toolBlockKey(state.messages[2], 0))?.map(block => block.content)).toEqual(['SECOND']);
  expect(results.matched.size).toBe(2);
  expect(stringifyJSON(state)).toBe(original);
  expect(state.messages.map(message => message.blocks[0].tool_id)).toEqual(['same', 'same', 'same', 'same']);
  expect(toolResults(events.map(event => parseJSON(stringifyJSON(event)!) as Envelope).reduce(reduceSession, emptySession('s')))).toEqual(results);
});
it('uses execution scope for interleaved native IDs and leaves ambiguous results visible', () => {
  const events = [
    { ...envelope(1, message('a', 'assistant', [{ type: 'tool_use', tool_id: 'same', name: 'read' }])), turn_id: 'a' },
    { ...envelope(2, message('b', 'assistant', [{ type: 'tool_use', tool_id: 'same', name: 'read' }])), turn_id: 'b' },
    { ...envelope(3, message('unknown', 'tool', [{ type: 'tool_result', tool_id: 'same', content: 'AMBIGUOUS' }])), turn_id: 'unknown' },
    { ...envelope(4, message('rb', 'tool', [{ type: 'tool_result', tool_id: 'same', content: 'B' }])), turn_id: 'b' },
    { ...envelope(5, message('ra', 'tool', [{ type: 'tool_result', tool_id: 'same', content: 'A' }])), turn_id: 'a' },
  ];
  const state = events.reduce(reduceSession, emptySession('s'));
  const result = toolResults(state);
  expect(result.byCall.get(toolBlockKey(state.messages[0], 0))?.[0].content).toBe('A');
  expect(result.byCall.get(toolBlockKey(state.messages[1], 0))?.[0].content).toBe('B');
  expect(result.matched.has(toolBlockKey(state.messages[2], 0))).toBe(false);
});
it('correlates an external result from the following request without reusing a completed result', () => {
  const events = [
    envelope(1, message('a', 'assistant', [{ type: 'tool_use', tool_id: 'native', name: 'read' }])),
    { ...envelope(2, message('result', 'tool', [{ type: 'tool_result', tool_id: 'native', content: 'CROSS_TURN' }])), turn_id: 'next' },
    { ...envelope(3, message('orphan', 'tool', [{ type: 'tool_result', tool_id: 'native', content: 'UNMATCHED' }])), turn_id: 'later' },
  ];
  const state = events.reduce(reduceSession, emptySession('s'));
  const result = toolResults(state);
  expect(result.byCall.get(toolBlockKey(state.messages[0], 0))?.map(block => block.content)).toEqual(['CROSS_TURN']);
  expect(result.matched.has(toolBlockKey(state.messages[2], 0))).toBe(false);
});


it('retains complete reasoning metadata without concatenation or identity changes', () => {
  const d = (n: number, payload: import('./session').Delta) => envelope(n, { kind: 'delta', message_id: 'opaque', role: 'assistant', payload });
  const events = [d(1, { type: 'TURN_START' }),
    d(2, { type: 'BLOCK_START', block_id: 0, kind: 'thinking', thinking_id: 'rs_original' }),
    d(3, { type: 'BLOCK_DELTA', block_id: 0, kind: 'thinking', encrypted_content: 'cipher' }),
    d(4, { type: 'BLOCK_DELTA', block_id: 0, kind: 'thinking', thinking_id: 'rs_original', encrypted_content: 'cipher' })];
  const partial = events.reduce(reduceSession, emptySession('s'));
  expect(partial.messages[0].blocks[0]).toMatchObject({ thinking_id: 'rs_original', encrypted_content: 'cipher' });
  for (const payload of [
    { type: 'BLOCK_DELTA', block_id: 0, kind: 'thinking', thinking_id: 'changed' },
    { type: 'BLOCK_DELTA', block_id: 0, kind: 'thinking', encrypted_content: 'replacement' },
    { type: 'BLOCK_DELTA', block_id: 0, kind: 'text', encrypted_content: 'cipher' },
    { type: 'BLOCK_STOP', block_id: 0, kind: 'thinking', encrypted_content: 'cipher' },
    { type: 'TURN_STOP', thinking_id: 'rs_original' },
  ] as import('./session').Delta[]) expect(() => reduceSession(partial, d(5, payload))).toThrow();
  const state = [d(5, { type: 'BLOCK_STOP', block_id: 0 }), d(6, { type: 'TURN_STOP' })].reduce(reduceSession, partial);
  expect(state.messages[0].blocks[0].encrypted_content).toBe('cipher');
  expect(JSON.parse(JSON.stringify([...events, d(5, { type: 'BLOCK_STOP', block_id: 0 }), d(6, { type: 'TURN_STOP' })])).reduce(reduceSession, emptySession('s'))).toEqual(state);
});

it('retains authored text groups in live and durable deltas and rejects misplaced groups', () => {
  const d = (sequence: number, payload: import('./session').Delta) => envelope(sequence, {
    kind: 'delta', message_id: 'grouped', role: 'assistant', payload,
  });
  const events = [d(1, { type: 'TURN_START' }), d(2, { type: 'BLOCK_START', block_id: 0, kind: 'text', message_group: 2 })];
  const state = events.reduce(reduceSession, emptySession('s'));
  expect(state.messages[0].blocks[0].message_group).toBe(2);
  expect(JSON.parse(JSON.stringify(events)).reduce(reduceSession, emptySession('s'))).toEqual(state);
  for (const payload of [
    { type: 'BLOCK_DELTA', block_id: 0, kind: 'text', message_group: 2 },
    { type: 'BLOCK_START', block_id: 1, kind: 'thinking', message_group: 2 },
    { type: 'BLOCK_START', block_id: 1, kind: 'text', message_group: 0 },
    { type: 'BLOCK_START', block_id: 1, kind: 'text', message_group: 1.5 },
    { type: 'TURN_STOP', message_group: 2 },
  ] as import('./session').Delta[]) expect(() => reduceSession(state, d(3, payload))).toThrow();
});


it('advances native checkpoint cursors without adding model content', () => {
  const checkpoint = { scope: 'a'.repeat(64), context_digest: 'b'.repeat(64), prefix_digest: 'c'.repeat(64),
    claim: 'd'.repeat(32), harness: 'codex' as const, native_id: 'opaque:native', message_count: 2 };
  const event = envelope(1, { kind: 'native_checkpoint', payload: checkpoint });
  const state = reduceSession(emptySession('s'), event);
  expect(state).toEqual({ ...emptySession('s'), sequence: '1' });
  expect(reduceSession(emptySession('s'), JSON.parse(JSON.stringify(event)))).toEqual(state);
  for (const payload of [
    { ...checkpoint, scope: '' }, { ...checkpoint, context_digest: null }, { ...checkpoint, prefix_digest: 'X'.repeat(64) },
    { ...checkpoint, claim: 'd'.repeat(31) }, { ...checkpoint, harness: 'unknown' }, { ...checkpoint, native_id: '' },
    { ...checkpoint, native_id: '😀'.repeat(257) }, { ...checkpoint, message_count: 0 },
    { ...checkpoint, message_count: 1.5 }, { ...checkpoint, message_count: 4097 }, { ...checkpoint, unmodeled_prompt: 'private' },
  ]) expect(() => reduceSession(emptySession('s'), envelope(1, { kind: 'native_checkpoint', payload } as SessionEvent))).toThrow();
});


it('records native compaction without replacing canonical model history', () => {
  const state = reduceSession(emptySession('s'), envelope(1, { kind: 'native_compaction', payload: { harness: 'codex' } }));
  expect(state).toEqual({ ...emptySession('s'), sequence: '1' });
  for (const payload of [{}, { harness: 'unknown' }, { harness: null }, { harness: 'claude', summary: 'native summary' }])
    expect(() => reduceSession(emptySession('s'), envelope(1, { kind: 'native_compaction', payload } as SessionEvent))).toThrow();
});

function verificationProof() {
  return {
    version: 1 as const, session_id: 's', turn_id: 't', actor: 'operator', scope_kind: 'project' as const,
    scope_id: 'project', isolation: 'private_source_snapshot' as const, snapshot_id: 'a'.repeat(32),
    snapshot_commit: 'b'.repeat(40), final_commit: 'b'.repeat(40), snapshot_tree: 'c'.repeat(40), final_tree: 'c'.repeat(40),
    configuration_sha256: 'd'.repeat(64), final_configuration_sha256: 'd'.repeat(64), profile_sha256: 'e'.repeat(64),
    completed: true, observed_at: '2026-10-20T12:00:00Z',
    steps: [{ name: 'unit', command_sha256: 'f'.repeat(64), ran: true, skipped: false, termination: 'exited' as const, exit_code: '0' }],
  };
}
it('replays completed verifier receipts as metadata without creating messages or controls', () => {
  const start = reduceSession(emptySession('s'), envelope(1, { kind: 'execution', payload: { status: 'running' } }));
  const terminal = envelope(2, { kind: 'execution', payload: { status: 'completed', verification_proofs: [verificationProof()] } });
  const completed = reduceSession(start, terminal);
  expect(completed.executions).toEqual({ t: 'completed' });
  expect(completed.messages).toEqual([]);
  expect(completed.controls).toEqual({});
  expect(completed.activeTurns).toEqual([]);
  expect(reduceSession(start, parseJSON(stringifyJSON(terminal)) as Envelope)).toEqual(completed);
});
it('refuses foreign, malformed and oversized verifier metadata on execution terminals', () => {
  const start = reduceSession(emptySession('s'), envelope(1, { kind: 'execution', payload: { status: 'running' } }));
  const proof = verificationProof();
  for (const invalid of [
    { ...proof, session_id: 'other' }, { ...proof, turn_id: 'other' }, { ...proof, actor: 'other' },
    { ...proof, prompt: 'not metadata' }, { ...proof, completed: null }, { ...proof, profile_sha256: 'unknown' },
    { ...proof, steps: [{ ...proof.steps[0], exit_code: '00' }] },
    { ...proof, steps: [proof.steps[0], proof.steps[0]] },
  ]) expect(() => reduceSession(start, envelope(2, { kind: 'execution', payload: {
    status: 'completed', verification_proofs: [invalid],
  } } as unknown as SessionEvent))).toThrow('Invalid execution verification proofs');
  for (const invalid of [null, [], Array.from({ length: 9 }, () => proof)])
    expect(() => reduceSession(start, envelope(2, { kind: 'execution', payload: {
      status: 'completed', verification_proofs: invalid,
    } } as unknown as SessionEvent))).toThrow('Invalid execution verification proofs');
  expect(() => reduceSession(emptySession('s'), envelope(1, { kind: 'execution', payload: {
    status: 'running', verification_proofs: [proof],
  } }))).toThrow('Invalid execution verification proofs');
});


it('admits typed delegate custody metadata without projecting messages or controls', () => {
  const learning_binding = { version: 1 as const, job_id: '2147483647', scope_kind: 'project' as const,
    scope_id: 'registered-project', agent: 'selected-agent', provider: 'actual-provider', model: 'actual-model' };
  const child = { session_id: 'private-child', turn_id: 'child-run', role: 'review', status: 'running' as const, learning_binding };
  const events = [envelope(1, { kind: 'child_execution', payload: child }),
    envelope(2, { kind: 'child_execution', payload: { ...child, status: 'completed' } })];
  const state = events.reduce(reduceSession, emptySession('s'));
  expect(state.children['private-child:child-run'].learning_binding).toEqual(learning_binding);
  expect(state.messages).toEqual([]);
  expect(state.controls).toEqual({});
  expect(state.activeTurns).toEqual([]);
  expect(state.errors).toEqual([]);
  expect(JSON.parse(JSON.stringify(events)).reduce(reduceSession, emptySession('s'))).toEqual(state);
  for (const invalid of [null, {}, { ...learning_binding, version: 2 },
    { ...learning_binding, job_id: '01' }, { ...learning_binding, job_id: '0' },
    { ...learning_binding, job_id: '2147483648' }, { ...learning_binding, job_id: 1 },
    { ...learning_binding, scope_kind: 'global' }, { ...learning_binding, scope_id: '' },
    { ...learning_binding, agent: 'é'.repeat(32) }, { ...learning_binding, provider: 'p'.repeat(128) },
    { ...learning_binding, model: 'm'.repeat(256) }, { ...learning_binding, agent: 'a\0b' },
    { ...learning_binding, verdict: 'passed' },
  ]) {
    expect(() => reduceSession(emptySession('s'), envelope(1, { kind: 'child_execution',
      payload: { ...child, learning_binding: invalid } } as unknown as SessionEvent))).toThrow(/reference/);
  }
  expect(() => reduceSession(emptySession('s'), envelope(1, { kind: 'child_execution',
    payload: { ...child, arbitrary_metadata: {} } } as unknown as SessionEvent))).toThrow(/reference/);
});
