/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import Memories from './Memories';

const mocks = vi.hoisted(() => ({ get: vi.fn(), send: vi.fn() }));
vi.mock('../api', () => ({ apiGet: mocks.get, apiSend: mocks.send }));

beforeEach(() => {
  mocks.get.mockReset().mockResolvedValue({
    count: 1,
    memories: [{
      id: 17, tier: 'L2', kind: 'fact', key: 'deploy:target', content: 'deploy to prod',
      confidence: 0, lifecycle: 'rejected', review_reason: 'incorrect extraction',
      scope_type: 'project', scope_value: 'project-a', created_at: '2026-01-01', updated_at: '2026-01-02',
    }],
  });
  mocks.send.mockReset().mockResolvedValue({ ok: true });
});
afterEach(cleanup);

describe('operator memory review', () => {
  it('shows retained content and performs an explicit restore', async () => {
    render(<Memories />);
    expect(await screen.findByText('deploy to prod')).toBeTruthy();
    expect(screen.getByText('project:project-a')).toBeTruthy();
    fireEvent.click(screen.getByRole('button', { name: 'Restore' }));
    await waitFor(() => expect(mocks.send).toHaveBeenCalledWith(
      'POST', '/v1/console/memories/review', { memory_id: 17, action: 'restore', reason: undefined },
    ));
  });
});
