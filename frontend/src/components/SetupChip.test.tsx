/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import SetupChip from './SetupChip';

const mocks = vi.hoisted(() => ({ open: vi.fn() }));

vi.mock('../setup/configApi', () => ({
  loadConfig: async () => ({ provider: 'claude', embedder_url: 'http://embedder' }),
}));

vi.mock('../setup/setupSignals', () => ({
  fetchSetupAccountReady: async () => true,
  fetchProjectCount: async () => 1,
  fetchHostCount: async () => 0,
  fetchGitIdentityReady: async () => false,
}));

vi.mock('../setup/setupState', () => ({
  requestOpenWizard: mocks.open,
  isDismissed: () => true,
  SETUP_UPDATED_EVENT: 'aimee-setup-updated',
}));

afterEach(() => {
  cleanup();
  mocks.open.mockClear();
});

describe('Setup chip Git-identity recovery', () => {
  it('keeps one actionable item visible after identity is skipped', async () => {
    render(<SetupChip />);
    const chip = await waitFor(() => screen.getByRole('button', { name: /Setup — 1 left/ }));

    fireEvent.click(chip);

    expect(mocks.open).toHaveBeenCalledOnce();
  });
});
