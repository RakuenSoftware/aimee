/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import NativeMemory from './NativeMemory';

const mocks = vi.hoisted(() => ({ raw: vi.fn() }));
vi.mock('../api', () => ({ apiSendRaw: mocks.raw }));
beforeEach(() => mocks.raw.mockReset());
afterEach(cleanup);

describe('native memory administration adapter', () => {
  it('sends exact wide version tokens and displays the complete raw response', async () => {
    const request = '{"operation":"correction_review","request":{"version":{"operation":18446744073709551615,"belief":9007199254740993}}}';
    const response = '{"status":"ok","result":{"operation":18446744073709551615,"belief":9007199254740993}}';
    mocks.raw.mockResolvedValue({ status: 200, ok: true, text: response });
    render(<NativeMemory />);
    expect(mocks.raw).not.toHaveBeenCalled();
    fireEvent.change(screen.getByLabelText('Native database owner UUID'), { target: { value: '00000000-0000-0000-0000-000000000001' } });
    fireEvent.change(screen.getByLabelText('Native request JSON'), { target: { value: request } });
    fireEvent.click(screen.getByRole('button', { name: 'Send request' }));
    await waitFor(() => expect(mocks.raw).toHaveBeenCalledWith('POST', '/v1/console/memory', '{"owner":"00000000-0000-0000-0000-000000000001","request":' + request + '}'));
    expect((await screen.findByLabelText('Native response')).textContent).toBe(response);
  });

  it('keeps a native refusal response intact and offers read-only examples', async () => {
    const response = '{"status":"error","kind":"native_admin_refused","exact":18446744073709551615}';
    mocks.raw.mockResolvedValue({ status: 403, ok: false, text: response });
    render(<NativeMemory />);
    fireEvent.change(screen.getByLabelText('Native database owner UUID'), { target: { value: '00000000-0000-0000-0000-000000000001' } });
    fireEvent.click(screen.getByRole('button', { name: 'Correction list' }));
    const correction = JSON.parse((screen.getByLabelText('Native request JSON') as HTMLTextAreaElement).value);
    expect(correction.operation).toBe('correction_list');
    expect(Object.keys(correction.scope.principal).sort()).toEqual(['identity', 'kind', 'verified_by']);
    fireEvent.click(screen.getByRole('button', { name: 'Ontology migrations' }));
    expect((screen.getByLabelText('Native request JSON') as HTMLTextAreaElement).value).toContain('ontology_migrations');
    fireEvent.click(screen.getByRole('button', { name: 'Send request' }));
    expect((await screen.findByLabelText('Native response')).textContent).toBe(response);
    expect(screen.getByRole('status').textContent).toBe('HTTP 403');
  });
});
