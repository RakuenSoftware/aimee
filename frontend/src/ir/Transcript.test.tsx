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
