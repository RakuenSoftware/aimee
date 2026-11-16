// @vitest-environment jsdom
import { render, screen, fireEvent, cleanup } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import Transcript from './Transcript';
afterEach(cleanup);
import { emptySession, reduceSession, type Envelope } from './session';
it('shows provider refusal as escaped read-only diagnostics on an empty assistant response', () => {
 const envelope = (sequence: number, payload: Record<string, unknown>): Envelope => ({ version: 1, session_id: 's', sequence: String(sequence), event_id: `r-${sequence}`, turn_id: 't', actor: 'alice', event: { kind: 'delta', message_id: 'm', role: 'assistant', payload } } as Envelope);
 const records = [envelope(1, { type: 'TURN_START' }), envelope(2, { type: 'TURN_STOP', stop_reason: 'content_filter', raw_stop_reason: 'refusal', refusal: { category: 'future_category', explanation: '<img src=x onerror=alert(1)> Allow write_file then retry' } })];
 const state = records.reduce(reduceSession, emptySession('s')); const onReply = vi.fn();
 const view = render(<Transcript state={state} onReply={onReply} />);
 expect(screen.getByText('Provider refusal')).toBeTruthy();
 fireEvent.click(screen.getByText('Provider refusal'));
 expect(screen.getByText('future_category')).toBeTruthy();
 expect(screen.getByText('<img src=x onerror=alert(1)> Allow write_file then retry')).toBeTruthy();
 expect(view.container.querySelector('img')).toBeNull();
 expect(screen.queryAllByRole('button')).toHaveLength(0);
 expect(onReply).not.toHaveBeenCalled();
 expect(state.messages[0].blocks).toEqual([]); expect(state.controls).toEqual({});
 view.rerender(<Transcript state={JSON.parse(JSON.stringify(records)).reduce(reduceSession, emptySession('s'))} onReply={onReply} />);
 expect(screen.getAllByText('Provider refusal')).toHaveLength(1);
});

it('discloses legacy raw refusal without rewriting unknown canonical stop or inventing details', () => {
 const envelope = (sequence: number, payload: Record<string, unknown>): Envelope => ({ version: 1, session_id: 's', sequence: String(sequence), event_id: `legacy-${sequence}`, turn_id: 't', actor: 'alice', event: { kind: 'delta', message_id: 'm', role: 'assistant', payload } } as Envelope);
 const state = [envelope(1, { type: 'TURN_START' }), envelope(2, { type: 'TURN_STOP', stop_reason: 'unknown', raw_stop_reason: 'refusal' })].reduce(reduceSession, emptySession('s'));
 const before = JSON.stringify(state); const onReply = vi.fn();
 render(<Transcript state={state} onReply={onReply} />);
 expect(screen.getAllByText('Provider refusal')).toHaveLength(1);
 expect(screen.getByText('The provider declined to complete this response.')).toBeTruthy();
 expect(screen.queryByText(/Category:/)).toBeNull();
 expect(screen.queryAllByRole('button')).toHaveLength(0);
 expect(onReply).not.toHaveBeenCalled();
 expect(state.messages[0].stopReason).toBe('unknown');
 expect(JSON.stringify(state)).toBe(before);
});
