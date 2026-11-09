import { describe, expect, it } from 'vitest';
import { emptySession, reduceSession, type Block, type Envelope, type SessionEvent } from './session';
const attachment = (n: number): NonNullable<Block['input_attachment']> => ({ schema_version: 1,
  kind: 'host.original-input.v1', operation_uuid: `00000000-0000-4000-8000-${String(n).padStart(12, '0')}`,
  text_sha256: 'a'.repeat(64), opaque: 'synthetic unchanged source bytes' });
const input = (n: number): SessionEvent => ({ kind: 'message', message_id: 'user', payload: {
  role: 'user', blocks: [{ type: 'text', text: `normalized synthetic input ${n}`, input_attachment: attachment(n) }] } });
const envelope = (n: number, turn: string, event: SessionEvent): Envelope => ({ version: 1,
  session_id: 's', sequence: String(n), event_id: `event-${n}`, turn_id: turn, actor: 'fixture-user', event });
describe('legacy Original input display identities', () => {
  it('preserves both turns and immutable wire through live projection, replay and reload', () => {
    const events = [envelope(1, 'first', input(1)), envelope(2, 'second', input(2))];
    const before = JSON.stringify(events);
    const live = events.reduce(reduceSession, emptySession('s'));
    expect(live.messages.map(m => m.id)).toEqual(['first:user', 'second:user']);
    expect(live.messages.map(m => m.blocks)).toEqual(events.map(e => e.event.kind === 'message' ? e.event.payload.blocks : []));
    expect(JSON.stringify(events)).toBe(before);
    expect(JSON.parse(before).reduce(reduceSession, emptySession('s'))).toEqual(live);
    expect(reduceSession(live, events[1])).toBe(live);
    expect(reduceSession(live, envelope(3, 'second', input(2))).messages).toEqual(live.messages);
  });
  it('refuses changed text, opaque evidence, operation identity and cross-turn operation reuse', () => {
    const state = reduceSession(emptySession('s'), envelope(1, 'first', input(1)));
    for (const change of ['text', 'opaque', 'operation'] as const) {
      const next = input(1);
      if (next.kind !== 'message') throw new Error('fixture');
      if (change === 'text') next.payload.blocks[0].text = 'changed';
      else if (change === 'opaque') next.payload.blocks[0].input_attachment!.opaque = 'changed';
      else next.payload.blocks[0].input_attachment!.operation_uuid = attachment(2).operation_uuid;
      expect(() => reduceSession(state, envelope(2, 'first', next))).toThrow('Original input identity changed');
    }
    expect(() => reduceSession(state, envelope(2, 'second', input(1)))).toThrow('Original input identity changed');
  });
  it('keeps duplicate ordinary, Assistant and native identities refused', () => {
    const ordinary: SessionEvent = { kind: 'message', message_id: 'user', payload: { role: 'user', blocks: [{ type: 'text', text: 'ordinary' }] } };
    const old = reduceSession(emptySession('s'), envelope(1, 'first', ordinary));
    expect(() => reduceSession(old, envelope(2, 'second', ordinary))).toThrow('Message identity changed');
    expect(() => reduceSession(old, envelope(2, 'second', input(2)))).toThrow('Message identity changed');
    const original = reduceSession(emptySession('s'), envelope(1, 'first', input(1)));
    for (const event of [ordinary,
      { kind: 'message', message_id: 'user', payload: { role: 'assistant', blocks: [{ type: 'text', text: 'collision' }] } },
      { kind: 'delta', message_id: 'user', role: 'assistant', payload: { type: 'TURN_START' } },
    ] as SessionEvent[]) expect(() => reduceSession(original, envelope(2, 'second', event))).toThrow('Message identity changed');
    const assistant: SessionEvent = { kind: 'message', message_id: 'assistant', payload: { role: 'assistant', blocks: [{ type: 'text', text: 'stable' }] } };
    const withAssistant = reduceSession(original, envelope(2, 'first', assistant));
    expect(() => reduceSession(withAssistant, envelope(3, 'second', assistant))).toThrow('Message identity changed');
  });
  it('refuses malformed attachments instead of using them to bypass identity guards', () => {
    for (const change of ['version', 'kind', 'uuid', 'zero', 'hash', 'opaque', 'extra'] as const) {
      const event = input(1);
      if (event.kind !== 'message') throw new Error('fixture');
      const a = event.payload.blocks[0].input_attachment!;
      if (change === 'version') a.schema_version = 2;
      if (change === 'kind') a.kind = 'forged';
      if (change === 'uuid') a.operation_uuid = 'forged';
      if (change === 'zero') a.operation_uuid = '00000000-0000-0000-0000-000000000000';
      if (change === 'hash') a.text_sha256 = 'forged';
      if (change === 'opaque') a.opaque = '';
      if (change === 'extra') Object.assign(a, { forged: true });
      expect(() => reduceSession(emptySession('s'), envelope(1, 'first', event))).toThrow('Invalid original input attachment');
    }
  });
});
