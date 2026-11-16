import { expect, it } from 'vitest';
import { parseJSON } from './json';
import { emptySession, reduceSession, type Envelope } from './session';
const event = (sequence: number, payload: Record<string, unknown>): Envelope => ({ version: 1, session_id: 's', sequence: String(sequence), event_id: `r-${sequence}`, turn_id: 't', actor: 'alice', event: { kind: 'delta', message_id: 'm', role: 'assistant', payload } } as Envelope);
const start = event(1, { type: 'TURN_START' });
const stop = (refusal: unknown) => event(2, { type: 'TURN_STOP', stop_reason: 'content_filter', raw_stop_reason: 'refusal', refusal });
it('retains bounded refusal diagnostics across live replay and final snapshots without making content or controls', () => {
 const refusal = { category: 'new_category', explanation: '<b>Not a tool instruction</b>' };
 const records = [start, stop(refusal)];
 const state = records.reduce(reduceSession, emptySession('s'));
 expect(state.messages[0]).toMatchObject({ refusal, stopReason: 'content_filter', complete: true, blocks: [] });
 expect(state.controls).toEqual({});
 const replay = JSON.parse(JSON.stringify(records)).reduce(reduceSession, emptySession('s'));
 expect(replay).toEqual(state);
 const final: Envelope = { ...event(3, {}), event: { kind: 'message', message_id: 'm', payload: { role: 'assistant', blocks: [] } } };
 expect(reduceSession(state, final).messages[0]).toMatchObject({ refusal });
});
it('accepts legacy/no-details events and exact UTF8 bounds', () => {
 for (const refusal of [null, { category: null, explanation: null }, { category: 'é'.repeat(32), explanation: 'é'.repeat(2048) }])
  expect(() => [start, stop(refusal)].reduce(reduceSession, emptySession('s'))).not.toThrow();
 expect(() => [start, event(2, { type: 'TURN_STOP', stop_reason: 'end_turn', refusal: null })].reduce(reduceSession, emptySession('s'))).not.toThrow();
});
it('rejects malformed, oversized, NUL and misplaced refusal metadata', () => {
 expect(() => parseJSON('{"category":null,"category":"changed","explanation":null}')).toThrow();
 for (const refusal of [{ category: null }, { category: null, explanation: null, retry: true }, { category: 4, explanation: null }, { category: null, explanation: '\ud800' }, { category: 'é'.repeat(33), explanation: null }, { category: null, explanation: 'é'.repeat(2049) }, { category: null, explanation: 'bad\0value' }])
  expect(() => [start, stop(refusal)].reduce(reduceSession, emptySession('s'))).toThrow();
 expect(() => reduceSession(emptySession('s'), event(1, { type: 'TURN_START', refusal: null }))).toThrow();
 expect(() => [start, event(2, { type: 'TURN_STOP', stop_reason: 'end_turn', refusal: { category: null, explanation: null } })].reduce(reduceSession, emptySession('s'))).toThrow();
});
