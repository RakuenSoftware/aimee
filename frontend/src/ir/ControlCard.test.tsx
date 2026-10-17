/** @vitest-environment jsdom */
import { describe, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach } from 'vitest';
import ControlCard from './ControlCard';
afterEach(cleanup);
const permission = { kind: 'permission', request: { title: 'Run command', input: { command: 'make test' } }, requestedBy: 'alice', turn: 't' };
describe('canonical browser controls', () => {
  it('sends a single correlated decision and waits for the native response', async () => {
    const onReply = vi.fn().mockResolvedValue(undefined);
    render(<ControlCard id="t:p" control={permission} onReply={onReply} />);
    fireEvent.click(screen.getByRole('button', { name: 'Allow once' }));
    await waitFor(() => expect(onReply).toHaveBeenCalledWith('t:p', { decision: 'allow' }));
    fireEvent.click(screen.getByRole('button', { name: 'Allow once' }));
    expect(onReply).toHaveBeenCalledTimes(1);
  });
  it('answers the common question schema with selected and free-form text', async () => {
    const onReply = vi.fn().mockResolvedValue(undefined);
    render(<ControlCard id="t:q" control={{ ...permission, kind: 'question', request: { questions: [
      { id: 'q1', question: 'Which model?', multiple: false, options: [{ label: 'A' }, { label: 'B' }] },
    ] } }} onReply={onReply} />);
    expect(screen.queryByRole('button', { name: 'Send answers' })).toBeNull();
    fireEvent.click(screen.getByRole('radio', { name: 'B' }));
    fireEvent.click(screen.getByRole('button', { name: 'Send answers' }));
    await waitFor(() => expect(onReply).toHaveBeenCalledWith('t:q', { decision: 'allow', answers: { q1: ['B'] } }));
  });
  it('surfaces stale/refused responses and allows a retry', async () => {
    const onReply = vi.fn().mockRejectedValue(new Error('Control request is stale'));
    render(<ControlCard id="t:p" control={permission} onReply={onReply} />);
    fireEvent.click(screen.getByRole('button', { name: 'Decline' }));
    expect(await screen.findByRole('alert')).toHaveProperty('textContent', 'Control request is stale');
  });
  it('does not offer decisions for resolved or cancelled requests', () => {
    render(<ControlCard id="t:p" control={{ ...permission, closed: true }} onReply={vi.fn()} />);
    expect(screen.queryByRole('button')).toBeNull();
  });
});
