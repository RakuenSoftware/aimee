/** @vitest-environment jsdom */
import { webcrypto } from 'node:crypto';
import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import IRTranscript from './Transcript';
import { emptySession, reduceSession, type Envelope } from './session';
import type { ImportReport } from './importReport';
afterEach(() => { cleanup(); vi.unstubAllGlobals(); });
const report = (changes: Partial<ImportReport> = {}): ImportReport => ({ version: 1, source_format: 'canonical', outcome: 'exact',
  source_sha256: 'a'.repeat(64), source_session: 'private-source', source_agent: 'private-agent', source_provider: 'private-provider',
  source_messages: 2, imported_messages: 2, removed_thinking_bindings: 0, tool_structure: 'preserved', archive_chunks: 0, ...changes });
const envelope = (n: number, payload: unknown): Envelope => ({ version: 1, session_id: 's', sequence: String(n),
  event_id: `e${n}`, turn_id: 'import', actor: 'operator', event: { kind: 'history', payload } } as Envelope);
it('describes normalization loss passively without exposing custody or creating controls', () => {
  const value = report({ outcome: 'normalized', source_format: 'anthropic', removed_thinking_bindings: 2 });
  const state = reduceSession(emptySession('s'), envelope(1, { scope: 'primary', phase: 'import_report', import_report: value }));
  const reply = vi.fn();
  const view = render(<IRTranscript state={state} onReply={reply} />);
  expect(screen.getByText('History imported after normalization (2 messages).')).toBeTruthy();
  expect(screen.getByText('2 reasoning replay bindings were removed.')).toBeTruthy();
  expect(screen.getByRole('note').getAttribute('aria-live')).toBe('off');
  expect(view.container.textContent).not.toContain('private-');
  expect(view.container.textContent).not.toContain('a'.repeat(64));
  expect(screen.queryByRole('button')).toBeNull();
  view.rerender(<IRTranscript state={reduceSession(state, envelope(1, { scope: 'primary', phase: 'import_report', import_report: value }))} onReply={reply} />);
  expect(screen.getAllByRole('note')).toHaveLength(1);
  expect(reply).not.toHaveBeenCalled();
  expect(state.messages).toEqual([]);
});
it('shows archived text only after contiguous source bytes verify and an explicit expansion', async () => {
  vi.stubGlobal('crypto', webcrypto);
  const text = 'Original terminal text <script>not executable</script>\n火';
  const digest = await webcrypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
  const hash = Buffer.from(digest).toString('hex');
  let state = reduceSession(emptySession('s'), envelope(1, { scope: 'legacy-archive', phase: 'archive', index: 0, archive_text: text.slice(0, 15) }));
  const reply = vi.fn();
  const view = render(<IRTranscript state={state} onReply={reply} />);
  expect(view.container.textContent).toBe('');
  state = reduceSession(state, envelope(2, { scope: 'legacy-archive', phase: 'archive', index: 1, archive_text: text.slice(15) }));
  state = reduceSession(state, envelope(3, { scope: 'legacy-archive', phase: 'import_report', import_report: report({
    source_format: 'tmux_text', outcome: 'archived', source_messages: 0, imported_messages: 0, tool_structure: 'unrecoverable', archive_chunks: 2, source_sha256: hash,
  }) }));
  view.rerender(<IRTranscript state={state} onReply={reply} />);
  await screen.findByText('View archived terminal text');
  expect(view.container.querySelector('pre')).toBeNull();
  const details = screen.getByText('View archived terminal text').closest('details')!;
  act(() => { details.open = true; fireEvent(details, new Event('toggle')); });
  expect(view.container.querySelector('pre')?.textContent).toBe(text);
  expect(view.container.querySelector('script')).toBeNull();
  expect(screen.getByRole('note').getAttribute('aria-live')).toBe('off');
  expect(state.messages).toEqual([]);
  expect(reply).not.toHaveBeenCalled();
});
it('withholds a mismatched archive instead of presenting it as imported conversation', async () => {
  vi.stubGlobal('crypto', webcrypto);
  let state = reduceSession(emptySession('s'), envelope(1, { scope: 'legacy-archive', phase: 'archive', index: 0, archive_text: 'altered archived bytes' }));
  state = reduceSession(state, envelope(2, { scope: 'legacy-archive', phase: 'import_report', import_report: report({
    source_format: 'tmux_text', outcome: 'archived', source_messages: 0, imported_messages: 0, tool_structure: 'unrecoverable', archive_chunks: 1,
  }) }));
  const view = render(<IRTranscript state={state} />);
  await waitFor(() => expect(screen.getByRole('note').textContent).toContain('could not be verified'));
  expect(view.container.textContent).not.toContain('altered archived bytes');
  expect(view.container.querySelector('details')).toBeNull();
  expect(state.messages).toEqual([]);
});

it('renders the acknowledged typed import base before later observations, without reply authority', async () => {
  const value = report({ source_messages: 2, imported_messages: 2 });
  let state = reduceSession(emptySession('s'), envelope(1, { scope: 'primary', phase: 'import_report', import_report: value }));
  state = reduceSession(state, envelope(2, { scope: 'primary', phase: 'message', index: 0, message: { role: 'user', blocks: [{ type: 'text', text: 'Old exact input' }] } }));
  state = reduceSession(state, envelope(3, { scope: 'primary', phase: 'message', index: 1, message: { role: 'assistant', blocks: [{ type: 'tool_use', tool_id: 'old-call', name: 'write_file', input: { path: 'old.txt' } }] } }));
  const reply = vi.fn();
  const view = render(<IRTranscript state={state} onReply={reply} />);
  expect(view.container.textContent).not.toContain('Old exact input');
  state = reduceSession(state, envelope(4, { scope: 'primary', phase: 'commit', count: 2 }));
  state = reduceSession(state, { ...envelope(5, {}), turn_id: 'later', event: { kind: 'message', message_id: 'new', payload: { role: 'assistant', blocks: [{ type: 'text', text: 'Later real observation' }] } } });
  view.rerender(<IRTranscript state={state} onReply={reply} />);
  await screen.findByText('Old exact input');
  expect(view.container.textContent!.indexOf('Old exact input')).toBeLessThan(view.container.textContent!.indexOf('Later real observation'));
  expect(screen.getByRole('region', { name: 'Imported historical conversation' }).textContent).toContain('user');
  expect(view.container.textContent).toContain('old.txt');
  expect(screen.queryByRole('button')).toBeNull();
  expect(reply).not.toHaveBeenCalled();
  expect(state.messages).toHaveLength(1);
  // Ordinary prompt checkpoint remains cursor-only, even while an imported base is visible.
  state = reduceSession(state, { ...envelope(6, { scope: 'primary', phase: 'message', index: 0, message: { role: 'user', blocks: [{ type: 'text', text: 'Do not duplicate prompt' }] } }), turn_id: 'normal' });
  view.rerender(<IRTranscript state={state} onReply={reply} />);
  expect(view.container.textContent).not.toContain('Do not duplicate prompt');
  expect(screen.getAllByText('Old exact input')).toHaveLength(1);
});
it('verifies fragmented imported messages before exposure and hides altered bytes', async () => {
  vi.stubGlobal('crypto', webcrypto);
  const raw = JSON.stringify({ role: 'developer', blocks: [{ type: 'text', text: 'Exact large-message content 火' }] });
  const digest = Buffer.from(await webcrypto.subtle.digest('SHA-256', new TextEncoder().encode(raw))).toString('hex');
  const build = (hash: string) => {
    let state = reduceSession(emptySession('s'), envelope(1, { scope: 'primary', phase: 'import_report', import_report: report({ source_messages: 1, imported_messages: 1 }) }));
    for (const [index, text] of [raw.slice(0, 29), raw.slice(29)].entries()) state = reduceSession(state, envelope(index + 2, { scope: 'primary', phase: 'message_fragment', fragment: { message_index: 0, part_index: index, part_count: 2, message_sha256: hash, text } }));
    return reduceSession(state, envelope(4, { scope: 'primary', phase: 'commit', count: 1 }));
  };
  const view = render(<IRTranscript state={build(digest)} />);
  await screen.findByText('Exact large-message content 火');
  expect(view.container.textContent).toContain('developer');
  view.rerender(<IRTranscript state={build('0'.repeat(64))} />);
  await screen.findByText('Imported conversation could not be verified and is hidden.');
  expect(view.container.textContent).not.toContain('Exact large-message content 火');
});
