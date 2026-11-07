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

// Exact canonical events exported read-only from the owned VM SDK journeys.
it('admits the actual Claude owned partial snapshot without rewriting its role or custody', () => {
  const event = parseJSON("{\"kind\":\"message\",\"role\":\"assistant\",\"partial\":true,\"payload\":{\"role\":\"assistant\",\"blocks\":[{\"name\":\"mcp__aimee__write_file\",\"type\":\"tool_use\",\"input\":{\"path\":\"NATIVE_INTERRUPTION_DENIED_claude\",\"content\":\"MUST_NOT_BE_WRITTEN\"},\"context\":{\"trust\":2,\"origin\":7,\"authority\":1,\"sensitivity\":1,\"model_visible\":true,\"revision_epoch\":\"0\",\"revision_scope\":\"\",\"revision_domain\":\"\"},\"tool_id\":\"interrupt_denied_write_1\"}]},\"message_id\":\"48d6ed2860f85cf509090273f3b212d0:f1699d5cd1c6a96c118088c1d92b8d67:msg_1792575677\",\"native_owner\":{\"epoch\":\"6ce691efcc3cafdad9a83e9da922af5e\",\"harness\":\"claude\",\"version\":1,\"owner_id\":\"426f9fa848373257c6d60efcd7e1993d\",\"binding_sha256\":\"46fdf321e8851da51750fd8a7bb539fe4aa7ef98484b1e18bce1aa2da8a364ba\",\"containment_sha256\":\"67dd83eedd8276a5d0ccd8a250897f2ee416167dee6883166c1a642f5b57a6de\"}}") as Extract<SessionEvent, { kind: 'message' }>;
  const state = reduceSession(emptySession('s'), envelope(1, event));
  expect(state.messages[0].blocks).toEqual(event.payload.blocks);
  expect(state.messages[0].nativeOwner).toEqual(event.native_owner);
  expect(state.messages[0].partial).toBe(true);
  expect(state.messages[0].complete).toBe(false);
  expect(state.controls).toEqual({});
});
it('admits the actual native-owned tool result as passive canonical output', () => {
  const event = parseJSON("{\"kind\":\"message\",\"payload\":{\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\",\"content\":\"{\\\"action_id\\\":\\\"56ce84207fb4ee42cc233ae98561f7e1f5f2f57ba8d47d7a33fd56cc4aa9bfd5\\\",\\\"status\\\":\\\"acknowledged\\\",\\\"completion_claim\\\":\\\"provider_acknowledged\\\",\\\"tool_result\\\":\\\"# anchored read snapshot=s0000000f4bff7823 (edit by anchor: pass snapshot_id + edits[{op,at,text}]; \\\\\\\"LINE:HH\\\\\\\" cites a line)\\\\n1:37| canonical delegate fixture\\\\n\\\"}\",\"context\":{\"trust\":0,\"origin\":0,\"authority\":0,\"sensitivity\":0,\"model_visible\":false,\"revision_epoch\":\"0\",\"revision_scope\":\"\",\"revision_domain\":\"\"},\"tool_id\":\"toolu_native_claude_anthropic_oauth\",\"is_error\":false}]},\"message_id\":\"4198b69c9c45c687cbb251d2f169fd2d:native-tool:4:0\",\"native_owner\":{\"epoch\":\"0\",\"harness\":\"claude\",\"version\":1,\"owner_id\":\"316f5dd0b593812f697b9baa598ce88a\",\"binding_sha256\":\"786e2e0ba9091459915c7e9ea0b500904381353454ea754018f6e256b76ec294\",\"containment_sha256\":\"f9de979cf5c17ee7d7bfc656dc835aec1d98c3e347f8d0122e361fb9806e21b5\"}}") as Extract<SessionEvent, { kind: 'message' }>;
  const state = reduceSession(emptySession('s'), envelope(1, event));
  expect(state.messages[0].role).toBe('tool');
  expect(state.messages[0].blocks).toEqual(event.payload.blocks);
  expect(state.messages[0].nativeOwner).toEqual(event.native_owner);
  expect(state.controls).toEqual({});
  expect(state.toolSources).toEqual({});
  expect(state.activeTurns).toEqual([]);
});
it.each(['assistant', ''])('admits the canonical optional outer role only as matching or omitted metadata (%s)', role => {
  const event = { ...snapshot([call], false), role } as SessionEvent;
  expect(reduceSession(emptySession('s'), envelope(1, event)).messages[0].role).toBe('assistant');
});
it.each([
  { ...snapshot([call]), role: 'tool' },
  { ...snapshot([call]), role: 1 },
  { ...snapshot([call]), role: null },
  { ...snapshot([call]), role: false },
  { ...snapshot([call]), role: {} },
  { ...snapshot([call]), payload: { role: 'tool', blocks: [{ type: 'tool_result', tool_id: 'call', content: 'not an admitted partial' }] } },
  snapshot([]),
  { ...snapshot([call], false), payload: { role: 'user', blocks: [call] } },
  { ...snapshot([call]), extra: true },
  { ...snapshot([call]), native_owner: { ...nativeOwner(), extra: true } },
])('refuses contradictory, untyped or unsupported native snapshot fields %#', invalid => {
  expect(() => reduceSession(emptySession('s'), envelope(1, invalid as unknown as SessionEvent))).toThrow(/snapshot/);
});
