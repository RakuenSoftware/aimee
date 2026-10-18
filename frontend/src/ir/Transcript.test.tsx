// @vitest-environment jsdom
import { render, screen } from '@testing-library/react';
import { expect, it } from 'vitest';
import IRTranscript from './Transcript';
import { emptySession, reduceSession, type Envelope } from './session';
it('displays authored message groups without changing persisted text fragments', () => {
  const envelope: Envelope = { version: 1, session_id: 's', sequence: '1', event_id: 'e', turn_id: 't', actor: 'alice',
    event: { kind: 'message', message_id: 'a', payload: { role: 'assistant', blocks: [
      { type: 'text', text: 'P', message_group: 1 }, { type: 'text', text: 'R.', message_group: 1 },
      { type: 'text', text: 'GitHub', message_group: 2 },
    ] } } };
  const state = reduceSession(emptySession('s'), envelope);
  render(<IRTranscript state={state} />);
  expect(screen.getByText('PR.')).toBeTruthy();
  expect(screen.getByText('GitHub')).toBeTruthy();
  expect(state.messages[0].blocks.map(block => block.text)).toEqual(['P', 'R.', 'GitHub']);
});

it('reports output-boundary loss while retaining the canonical reasoning', () => {
  const before = reduceSession(emptySession('s'), { version: 1, session_id: 's', sequence: '1', event_id: 'a', turn_id: 't', actor: 'alice',
    event: { kind: 'message', message_id: 'a', payload: { role: 'assistant', blocks: [{ type: 'thinking', text: 'retained reasoning', signature: 'opaque' }] } } });
  const envelope: Envelope = { version: 1, session_id: 's', sequence: '2', event_id: 'p', turn_id: 't', actor: 'alice',
    event: { kind: 'projection', payload: { model: 'model-b', boundary: 'request', omitted_reasoning_blocks: 1, omitted_opaque_state_blocks: 1 } } };
  const state = reduceSession(before, envelope);
  expect(state.messages).toBe(before.messages);
  expect(reduceSession(JSON.parse(JSON.stringify(before)), JSON.parse(JSON.stringify(envelope)))).toEqual(state);
  render(<IRTranscript state={state} />);
  expect(screen.getByRole('status').textContent).toContain("1 reasoning block remains in history and was omitted from model-b's input");
  expect(state.messages[0].blocks[0].signature).toBe('opaque');
});
