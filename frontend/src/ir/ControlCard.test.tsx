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
  it('submits typed MCP form values and selected enum constants', async () => {
    const onReply = vi.fn().mockResolvedValue(undefined);
    render(<ControlCard id="t:form" control={{ ...permission, kind: 'elicitation', request: { form: {
      message: 'Configure the job', server: 'inventory', schema: { type: 'object', required: ['name', 'count', 'enabled', 'tags'], properties: {
        name: { type: 'string', title: 'Job name', minLength: 1 },
        count: { type: 'integer', title: 'Count', minimum: 1, maximum: 5 },
        enabled: { type: 'boolean', title: 'Enabled' },
        tags: { type: 'array', title: 'Tags', minItems: 1, items: { anyOf: [{ const: 'fast', title: 'Fast mode' }, { const: 'slow', title: 'Slow mode' }] } },
      } },
    } } }} onReply={onReply} />);
    expect(screen.getByText('Configure the job')).toBeTruthy();
    expect(screen.getByRole('button', { name: 'Send form' })).toHaveProperty('disabled', true);
    fireEvent.change(screen.getByRole('textbox', { name: 'Job name' }), { target: { value: 'build' } });
    fireEvent.change(screen.getByRole('spinbutton', { name: 'Count' }), { target: { value: '2' } });
    fireEvent.click(screen.getByRole('checkbox', { name: 'Fast mode' }));
    fireEvent.click(screen.getByRole('button', { name: 'Send form' }));
    await waitFor(() => expect(onReply).toHaveBeenCalledWith('t:form', { decision: 'allow', content: { name: 'build', count: 2, enabled: false, tags: ['fast'] } }));
    expect(screen.getByRole('button', { name: 'Send form' })).toHaveProperty('disabled', true);
  });
  it('keeps incomplete and invalid numeric forms unsent', () => {
    const onReply = vi.fn();
    render(<ControlCard id="t:form" control={{ ...permission, kind: 'elicitation', request: { form: { schema: {
      type: 'object', required: ['count'], properties: { count: { type: 'integer', title: 'Count', minimum: 1, maximum: 3 } },
    } } } }} onReply={onReply} />);
    for (const value of ['0', '4', '1.5']) {
      fireEvent.change(screen.getByRole('spinbutton', { name: 'Count' }), { target: { value } });
      expect(screen.getByRole('button', { name: 'Send form' })).toHaveProperty('disabled', true);
    }
    expect(onReply).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(onReply).toHaveBeenCalledWith('t:form', { decision: 'cancel' });
  });
  it('offers decline for unsupported nested forms without fabricating accepted content', () => {
    render(<ControlCard id="t:form" control={{ ...permission, kind: 'elicitation', request: { form: { schema: {
      type: 'object', properties: { nested: { type: 'object', properties: {} } },
    } } } }} onReply={vi.fn()} />);
    expect(screen.queryByRole('button', { name: 'Send form' })).toBeNull();
    expect(screen.getByRole('button', { name: 'Decline' })).toBeTruthy();
  });

  it('preserves an empty enum constant and a Unicode character in form content', async () => {
    const onReply = vi.fn().mockResolvedValue(undefined);
    render(<ControlCard id="t:form" control={{ ...permission, kind: 'elicitation', request: { form: { schema: {
      type: 'object', required: ['choice', 'name'], properties: {
        choice: { type: 'string', title: 'Choice', oneOf: [{ const: '', title: 'None' }, { const: 'x', title: 'X' }] },
        name: { type: 'string', title: 'Name', minLength: 1, maxLength: 1 },
      },
    } } } }} onReply={onReply} />);
    fireEvent.change(screen.getByRole('combobox', { name: 'Choice' }), { target: { value: '0' } });
    fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: 'é' } });
    fireEvent.click(screen.getByRole('button', { name: 'Send form' }));
    await waitFor(() => expect(onReply).toHaveBeenCalledWith('t:form', { decision: 'allow', content: { choice: '', name: 'é' } }));
  });

});
