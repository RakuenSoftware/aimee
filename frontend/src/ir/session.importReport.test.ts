import { expect, it } from 'vitest';
import { emptySession, reduceSession, type Envelope } from './session';
import type { ImportReport } from './importReport';
const report = (changes: Partial<ImportReport> = {}): ImportReport => ({ version: 1, source_format: 'canonical', outcome: 'exact',
  source_sha256: 'a'.repeat(64), source_session: 'owned-legacy-session', source_agent: 'agent', source_provider: '',
  source_messages: 2, imported_messages: 2, removed_thinking_bindings: 0, tool_structure: 'preserved', archive_chunks: 0, ...changes });
const envelope = (n: number, payload: unknown, actor = 'operator'): Envelope => ({ version: 1, session_id: 's', sequence: String(n),
  event_id: `e${n}`, turn_id: 'import', actor, event: { kind: 'history', payload } } as Envelope);
it('keeps import reports passive and identical on durable replay', () => {
  const before = reduceSession(emptySession('s'), { ...envelope(1, {}), turn_id: 'active', event: {
    kind: 'control_request', request_id: 'active:permission', control_kind: 'permission', payload: { title: 'Real pending action' },
  } });
  const value = envelope(2, { scope: 'primary', phase: 'import_report', import_report: report() });
  const next = reduceSession(before, value);
  expect(next.sequence).toBe('2');
  expect(next.messages).toBe(before.messages);
  expect(next.controls).toBe(before.controls);
  expect(next.activeTurns).toBe(before.activeTurns);
  expect(next.toolSources).toBe(before.toolSources);
  expect(next.importReports).toEqual([{ id: 'e2', turn: 'import', actor: 'operator', scope: 'primary', report: report(), checkpoint: { entries: [], committed: false, bytes: 0 } }]);
  expect(reduceSession(next, value)).toBe(next);
  expect(reduceSession(before, JSON.parse(JSON.stringify(value)))).toEqual(next);
});
it.each([
  { version: 2 }, { source_format: 'native_guess' }, { outcome: 'success' }, { source_sha256: 'A'.repeat(64) },
  { source_session: '' }, { source_session: 'x'.repeat(128) }, { source_agent: '火'.repeat(22) }, { source_provider: null },
  { source_provider: 'x'.repeat(64) }, { source_session: 'bad\0source' }, { source_agent: '\ud800' },
  { source_messages: -1 }, { source_messages: 16777217 }, { imported_messages: 16777217 }, { source_messages: 1.5 }, { removed_thinking_bindings: 16777217 },
  { archive_chunks: 1 }, { archive_chunks: undefined }, { imported_messages: 1 }, { removed_thinking_bindings: 1 },
  { source_format: 'anthropic' }, { tool_structure: 'unrecoverable' }, { extra: 'not typed metadata' },
])('refuses malformed exact import metadata %j', changes => {
  expect(() => reduceSession(emptySession('s'), envelope(1, { scope: 'primary', phase: 'import_report', import_report: { ...report(), ...changes } }))).toThrow();
});
it('preserves an empty historical agent selector without inventing a label', () => {
  const state = reduceSession(emptySession('s'), envelope(1, { scope: 'primary', phase: 'import_report', import_report: report({ source_agent: '' }) }));
  expect(state.importReports[0].report.source_agent).toBe('');
  expect(state.messages).toEqual([]);
});
it('admits honest normalization/refusal labels without promoting their content', () => {
  for (const value of [report({ outcome: 'normalized', source_format: 'anthropic', imported_messages: 1, removed_thinking_bindings: 3 }),
    report({ outcome: 'normalized', source_messages: 4097, imported_messages: 4097 }),
    report({ outcome: 'refused', source_format: 'unknown', source_messages: 16777216, imported_messages: 0, tool_structure: 'unrecoverable' })]) {
    const state = reduceSession(emptySession('s'), envelope(1, { scope: 'primary', phase: 'import_report', import_report: value }));
    expect(state.importReports[0].report).toEqual(value);
    expect(state.messages).toEqual([]);
    expect(state.controls).toEqual({});
  }
  for (const value of [report({ outcome: 'normalized', source_format: 'tmux_text' }), report({ outcome: 'refused', imported_messages: 1, tool_structure: 'unrecoverable' })])
    expect(() => reduceSession(emptySession('s'), envelope(1, { scope: 'primary', phase: 'import_report', import_report: value }))).toThrow();
});
it('retains contiguous archive chunks privately until the final count-bound report', () => {
  const initial = emptySession('s');
  const first = envelope(1, { scope: 'legacy-archive', phase: 'archive', index: 0, archive_text: 'old tool-looking text\n' });
  const pending = reduceSession(initial, first);
  expect(pending.messages).toBe(initial.messages);
  expect(pending.importReports).toEqual([]);
  expect(pending.pendingArchives.import.chunks).toEqual(['old tool-looking text\n']);
  expect(reduceSession(pending, first)).toBe(pending);
  for (const payload of [
    { scope: 'legacy-archive', phase: 'archive', index: 0, archive_text: 'duplicate' },
    { scope: 'legacy-archive', phase: 'archive', index: 2, archive_text: 'gap' },
    { scope: 'legacy-archive', phase: 'archive', index: 1, archive_text: '' },
    { scope: 'legacy-archive', phase: 'archive', index: 1, archive_text: '火'.repeat(21846) },
    { scope: 'primary', phase: 'archive', index: 1, archive_text: 'wrong scope' },
  ]) expect(() => reduceSession(pending, envelope(2, payload))).toThrow();
  expect(() => reduceSession(pending, envelope(2, { scope: 'legacy-archive', phase: 'archive', index: 1, archive_text: 'foreign actor' }, 'foreign'))).toThrow();
  const archived = report({ outcome: 'archived', source_format: 'tmux_text', source_messages: 0, imported_messages: 0,
    removed_thinking_bindings: 0, tool_structure: 'unrecoverable', archive_chunks: 1 });
  const complete = reduceSession(pending, envelope(2, { scope: 'legacy-archive', phase: 'import_report', import_report: archived }));
  expect(complete.messages).toEqual([]);
  expect(complete.pendingArchives).toEqual({});
  expect(complete.importReports[0].chunks).toEqual(['old tool-looking text\n']);
  expect(() => reduceSession(initial, envelope(1, { scope: 'legacy-archive', phase: 'import_report', import_report: archived }))).toThrow();
  expect(() => reduceSession(pending, envelope(2, { scope: 'legacy-archive', phase: 'import_report', import_report: { ...archived, archive_chunks: 2 } }))).toThrow();
  expect(() => reduceSession(pending, envelope(2, { scope: 'primary', phase: 'import_report', import_report: archived }))).toThrow();
});

it('admits strict message fragments without exposing prompt content or replaying controls', () => {
  const before = reduceSession(emptySession('s'), { ...envelope(1, {}), turn_id: 'active', event: {
    kind: 'control_request', request_id: 'active:question', control_kind: 'question', payload: { question: 'Real pending question' },
  } });
  const fragment = { message_index: 16777215, part_index: 4095, part_count: 4096, message_sha256: 'f'.repeat(64), text: 'private checkpoint text' };
  const value = envelope(2, { scope: 'primary', phase: 'message_fragment', fragment });
  const next = reduceSession(before, value);
  expect(next.sequence).toBe('2');
  for (const key of ['messages', 'controls', 'activeTurns', 'toolSources', 'importReports', 'pendingArchives'] as const)
    expect(next[key]).toBe(before[key]);
  expect(reduceSession(next, value)).toBe(next);
  for (const changes of [{ message_index: 16777216 }, { part_index: -1 }, { part_index: 4096 }, { part_count: 0 },
    { part_count: 4097 }, { part_index: 2, part_count: 2 }, { part_index: 0.5 }, { message_sha256: 'F'.repeat(64) },
    { text: '' }, { text: '火'.repeat(21846) }, { text: '\ud800' }, { extra: true }, { text: undefined }])
    expect(() => reduceSession(before, envelope(2, { scope: 'primary', phase: 'message_fragment', fragment: { ...fragment, ...changes } }))).toThrow();
  expect(() => reduceSession(before, envelope(2, { scope: '', phase: 'message_fragment', fragment }))).toThrow();
  expect(() => reduceSession(before, envelope(2, { scope: 'primary', phase: 'message_fragment', fragment, message: 'unexpected' }))).toThrow();
});

it('binds imported checkpoints to the report actor/scope/turn and exact count', () => {
  const initial = reduceSession(emptySession('s'), envelope(1, { scope: 'primary', phase: 'import_report', import_report: report({ source_messages: 1, imported_messages: 1 }) }));
  const payload = { scope: 'primary', phase: 'message', index: 0, message: { role: 'user', blocks: [{ type: 'text', text: 'Acknowledged old input' }] } };
  expect(() => reduceSession(initial, envelope(2, payload, 'foreign'))).toThrow();
  expect(() => reduceSession(initial, envelope(2, { ...payload, scope: 'external-sdk' }))).toThrow();
  expect(() => reduceSession(initial, envelope(2, { ...payload, index: 1 }))).toThrow();
  expect(() => reduceSession(initial, envelope(2, { scope: 'primary', phase: 'commit', count: 1 }))).toThrow();
  const foreignTurn = reduceSession(initial, { ...envelope(2, payload), turn_id: 'ordinary-checkpoint' });
  expect(foreignTurn.importReports[0].checkpoint!.entries).toEqual([]);
  const staged = reduceSession(initial, envelope(2, payload));
  expect(staged.messages).toBe(initial.messages);
  expect(staged.controls).toBe(initial.controls);
  expect(staged.importReports[0].checkpoint!.committed).toBe(false);
  expect(() => reduceSession(staged, envelope(3, { scope: 'primary', phase: 'commit', count: 0 }))).toThrow();
  const committed = reduceSession(staged, envelope(3, { scope: 'primary', phase: 'commit', count: 1 }));
  expect(committed.importReports[0].checkpoint!.committed).toBe(true);
  expect(reduceSession(committed, envelope(3, { scope: 'primary', phase: 'commit', count: 1 }))).toBe(committed);
  expect(() => reduceSession(committed, envelope(4, payload))).toThrow();
});
