import { expect, it } from 'vitest';
import { emptySession, reduceSession, toolBlockKey, toolResults, type Envelope, type SessionEvent } from './session';
import { nativeOwner, receipt } from './interruption.fixture';
const envelope = (n: number, event: SessionEvent): Envelope => ({ version: 1, session_id: 's', sequence: String(n), event_id: `e${n}`, turn_id: 't', actor: 'operator', event });
function original(status: 'completed' | 'cancelled' = 'completed') {
  return [envelope(1, { kind: 'execution', payload: { status: 'running' } }),
    envelope(2, { kind: 'message', message_id: 'm', native_owner: nativeOwner(), payload: { role: 'assistant', blocks: [{ type: 'tool_use', tool_id: 'call', name: 'write_file', input: { path: 'file' } }] } }),
    envelope(3, { kind: 'control_request', request_id: 'permission', control_kind: 'permission', payload: {} }),
    envelope(4, { kind: 'execution', payload: { status } })].reduce(reduceSession, emptySession('s'));
}
it.each(['completed', 'cancelled'] as const)('adds an unknown-effect annotation after %s without inventing a result or changing terminal controls', status => {
  const before = original(status);
  const event = envelope(5, { kind: 'tool_interruption', payload: receipt() });
  const after = reduceSession(before, event);
  expect(after.sequence).toBe('5');
  expect(after.messages).toBe(before.messages);
  expect(after.controls).toBe(before.controls);
  expect(after.executions).toBe(before.executions);
  expect(after.activeTurns).toBe(before.activeTurns);
  expect(after.controls.permission.closed).toBe(true);
  expect(after.toolInterruptions[toolBlockKey(after.messages[0], 0)].outcome).toBe('unknown');
  expect(toolResults(after).byCall.size).toBe(0);
  expect(reduceSession(after, event)).toBe(after);
  expect(JSON.parse(JSON.stringify([envelope(1, { kind: 'execution', payload: { status: 'running' } }),
    envelope(2, { kind: 'message', message_id: 'm', native_owner: nativeOwner(), payload: { role: 'assistant', blocks: before.messages[0].blocks } }),
    envelope(3, { kind: 'control_request', request_id: 'permission', control_kind: 'permission', payload: {} }),
    envelope(4, { kind: 'execution', payload: { status } }), event])).reduce(reduceSession, emptySession('s')).toolInterruptions).toEqual(after.toolInterruptions);
});
it('binds exact source event, message, actor, tool block and terminal sequence', () => {
  const changes = [
    { source: { ...receipt().source, session_id: 'foreign' } },
    { source: { ...receipt().source, actor: 'foreign' } },
    { source: { ...receipt().source, turn_id: 'foreign' } },
    { source: { ...receipt().source, event_id: 'stale' } },
    { source: { ...receipt().source, sequence: '1' } },
    { source: { ...receipt().source, message_id: 'missing' } },
    { source: { ...receipt().source, block_index: 1 } },
    { source: { ...receipt().source, tool_id: 'other' } },
    { source: { ...receipt().source, name: 'run_cmd' } },
    { terminal_sequence: '3' },
  ];
  for (const change of changes) expect(() => reduceSession(original(), envelope(5, { kind: 'tool_interruption', payload: { ...receipt(), ...change } }))).toThrow('matching source');
  expect(() => reduceSession(original(), envelope(5, { kind: 'tool_interruption', payload: { ...receipt(), extra: {} } } as unknown as SessionEvent))).toThrow('Invalid tool interruption');
  const live = original();
  delete live.terminalSources.t;
  expect(() => reduceSession(live, envelope(5, { kind: 'tool_interruption', payload: receipt() }))).toThrow('matching source');
});
it('rejects duplicate annotations and receipt claims for an actual matched result', () => {
  const once = reduceSession(original(), envelope(5, { kind: 'tool_interruption', payload: receipt() }));
  expect(() => reduceSession(once, envelope(6, { kind: 'tool_interruption', payload: receipt() }))).toThrow('completion observation');
  const before = original();
  before.messages.push({ ...before.messages[0], id: 'result', role: 'tool', blocks: [{ type: 'tool_result', tool_id: 'call', content: 'actual result' }] });
  expect(() => reduceSession(before, envelope(5, { kind: 'tool_interruption', payload: receipt() }))).toThrow('completion observation');
});
it('does not retrigger controls or annotate a later call reusing the tool id on replay', () => {
  const interruption = envelope(5, { kind: 'tool_interruption', payload: receipt() });
  let state = reduceSession(original(), interruption);
  const later = (n: number, event: SessionEvent) => ({ ...envelope(n, event), turn_id: 'later' });
  state = reduceSession(state, later(6, { kind: 'execution', payload: { status: 'running' } }));
  state = reduceSession(state, later(7, { kind: 'message', message_id: 'later-call', payload: { role: 'assistant', blocks: [{ type: 'tool_use', tool_id: 'call', name: 'write_file' }] } }));
  state = reduceSession(state, later(8, { kind: 'control_request', request_id: 'later-permission', control_kind: 'permission', payload: {} }));
  expect(reduceSession(state, interruption)).toBe(state);
  expect(state.toolInterruptions[toolBlockKey(state.messages[1], 0)]).toBeUndefined();
  expect(state.controls['later-permission'].closed).toBeUndefined();
  expect(state.controls['later-permission'].response).toBeUndefined();
});
