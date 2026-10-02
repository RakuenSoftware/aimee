/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import Governance from './Governance';
import { apiGet, apiSend } from '../api';
vi.mock('../api', () => ({ apiGet: vi.fn(), apiSend: vi.fn(), ApiError: class extends Error {} }));
afterEach(() => { cleanup(); vi.resetAllMocks(); });
it('opens the due operator queue and closes reviewed decisions through the existing status API', async () => {
  vi.mocked(apiGet).mockImplementation(async (path) => path.startsWith('/v1/decisions') ? {
    decisions: [{ id: 7, subject: 'cache policy', chosen: 'redis', status: 'revisit_due', revisit_when: '2026-09-01', created_at: '2026-08-01' }],
  } as never : { actions: [] } as never);
  vi.mocked(apiSend).mockResolvedValue({});
  render(<Governance />);
  fireEvent.click(await screen.findByRole('button', { name: 'Close review' }));
  await waitFor(() => expect(apiSend).toHaveBeenCalledWith('POST', '/v1/decisions/7/status', { status: 'superseded' }));
  expect(apiGet).toHaveBeenCalledWith('/v1/decisions?status=revisit_due');
  expect(await screen.findByText('Closed review for #7; decision marked superseded.')).toBeTruthy();
});
