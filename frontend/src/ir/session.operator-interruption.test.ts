import { expect, it } from 'vitest';
import { emptySession, reduceSession, toolBlockKey, toolResults, type Envelope, type SessionEvent } from './session';
import { operatorCancelError, validToolInterruption, type ToolInterruption } from './interruption';
import { nativeOwner } from './interruption.fixture';
const event = (n: number, value: SessionEvent, id = `e${n}`, actor = 'operator'): Envelope => ({ version: 1, session_id: 's', sequence: String(n), event_id: id, turn_id: 't', actor, event: value });
const call = (path = 'exact'): SessionEvent => ({ kind: 'message', message_id: 'm', payload: { role: 'assistant', blocks: [{ type: 'tool_use', tool_id: 'api-call', name: 'write_file', input: { path } }] } });
const receipt = (actor = 'operator'): ToolInterruption => ({ version: 1, source: { session_id: 's', turn_id: 't', actor, sequence: '2', event_id: 'e2', message_id: 'm', block_index: 0, tool_id: 'api-call', name: 'write_file' }, owner: { operator_cancel_event_id: 'operator-cancel:t' }, terminal_sequence: '3', outcome: 'unknown', retry: 'explicit_permission_required' });
function source(actor = 'operator', terminalID = 'operator-cancel:t', error = operatorCancelError, status: 'cancelled' | 'failed' = 'cancelled') {
 return [event(1, { kind: 'execution', payload: { status: 'running' } }, 'e1', actor), event(2, call(), 'e2', actor), event(3, { kind: 'execution', payload: { status, error } }, terminalID, actor)].reduce(reduceSession, emptySession('s'));
}
const interruption = (r = receipt(), id = 'operator-interruption:t:2:0') => event(4, { kind: 'tool_interruption', payload: r }, id, r.source.actor);
it('admits an exact ownerless API call/operator cancellation as unknown with no effects or control retries', () => {
 const before = source(); const result = reduceSession(before, interruption());
 expect(result.messages).toBe(before.messages); expect(result.executions).toBe(before.executions); expect(result.controls).toBe(before.controls);
 expect(toolResults(result).byCall.size).toBe(0); expect(result.toolInterruptions[toolBlockKey(result.messages[0], 0)].outcome).toBe('unknown');
 expect(reduceSession(result, interruption())).toBe(result);
});
it('allows the full159-byte canonical operator actor, retaining native128 bound', () => {
 const actor = 'a'.repeat(159); expect(validToolInterruption(receipt(actor))).toBe(true); expect(() => reduceSession(source(actor), interruption(receipt(actor)))).not.toThrow();
 expect(validToolInterruption(receipt('a'.repeat(160)))).toBe(false);
});
it('rejects mixed or forged operator owners and mismatched actual terminals', () => {
 for (const owner of [{ operator_cancel_event_id: 'operator-cancel:other' }, { operator_cancel_event_id: 'operator-cancel:t', owner_id: 'a'.repeat(32) }])
  expect(validToolInterruption({ ...receipt(), owner })).toBe(false);
 for (const before of [source('operator', 'wrong'), source('operator', 'operator-cancel:t', 'ordinary cancellation'), source('operator', 'operator-cancel:t', operatorCancelError, 'failed')])
  expect(() => reduceSession(before, interruption())).toThrow('matching source');
 expect(() => reduceSession(source(), interruption(receipt(), 'forged-receipt'))).toThrow('matching source');
 expect(() => reduceSession(source(), interruption(receipt('foreign')))).toThrow('matching source');
 for (const change of [{message_id: 'reused-id'}, {tool_id: 'reused-call'}, {name: 'bash'}, {block_index: 1}, {event_id: 'other-source'}, {turn_id: 'foreign'}]) {
  const changed = {...receipt(), source: {...receipt().source, ...change}};
  expect(() => reduceSession(source(), interruption(changed))).toThrow();
 }
 const completed = source();
 completed.messages.push({...completed.messages[0], id: 'actual-result', role: 'tool', blocks: [{type: 'tool_result', tool_id: 'api-call', content: 'actual result'}]});
 expect(() => reduceSession(completed, interruption())).toThrow('completion observation');

});
it('never borrows native custody for an operator receipt', () => {
 const owned = call(); if (owned.kind !== 'message') throw Error(); owned.native_owner = nativeOwner();
 const before = [event(1, {kind: 'execution', payload: {status: 'running'}}), event(2, owned), event(3, {kind: 'execution', payload: {status: 'cancelled', error: operatorCancelError}}, 'operator-cancel:t')].reduce(reduceSession, emptySession('s'));
 expect(() => reduceSession(before, interruption())).toThrow('matching source');
});
it('binds the latest complete ownerless snapshot and rejects stale or changed call identities', () => {
 let before = [event(1, {kind: 'execution', payload: {status: 'running'}}), event(2, call()), event(3, call('changed'))].reduce(reduceSession, emptySession('s'));
 before = reduceSession(before, event(4, {kind: 'execution', payload: {status: 'cancelled', error: operatorCancelError}}, 'operator-cancel:t'));
 expect(() => reduceSession(before, event(5, {kind: 'tool_interruption', payload: {...receipt(), terminal_sequence: '4'}}, 'operator-interruption:t:2:0'))).toThrow('matching source');
 const latest = {...receipt(), source: {...receipt().source, sequence: '3', event_id: 'e3'}, terminal_sequence: '4'};
 expect(() => reduceSession(before, event(5, {kind: 'tool_interruption', payload: latest}, 'operator-interruption:t:3:0'))).not.toThrow();
 before.messages[0].blocks[0].input = {path: 'mutated after source'};
 expect(() => reduceSession(before, event(5, {kind: 'tool_interruption', payload: latest}, 'operator-interruption:t:3:0'))).toThrow('matching source');
});
