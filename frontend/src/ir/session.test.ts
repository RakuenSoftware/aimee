import { describe, expect, it } from 'vitest';
import { emptySession, reduceSession, toolResults } from './session';
import type { Block, Envelope, SessionEvent } from './session';
const envelope = (n: number, event: SessionEvent): Envelope => ({ version: 1, session_id: 's', sequence: String(n), event_id: `e${n}`, turn_id: 't', actor: 'operator', event });
const message = (id: string, role: string, blocks: Block[]): SessionEvent => ({ kind: 'message', message_id: id, payload: { role, blocks } });

describe('canonical session projection', () => {
  it('renders live and replay identically, pairing interleaved tools by opaque ID', () => {
    const events = [
      envelope(1, message('u', 'user', [{ type: 'text', text: 'Investigate' }])),
      envelope(2, message('a', 'assistant', [
        { type: 'thinking', text: 'Reason', signature: 'opaque' },
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
    expect(toolResults(live).get('a')?.[0].content).toBe('A');
    expect(toolResults(live).get('b')?.[0].content).toBe('B');
    expect(live.messages[1].blocks[0].signature).toBe('opaque');
    expect(reduceSession(live, events[4])).toBe(live);
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
