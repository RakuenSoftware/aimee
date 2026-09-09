/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import SetupChip from './SetupChip';
import { DISMISSED_KEY } from '../setup/setupState';

const mocks = vi.hoisted(() => ({ open: vi.fn(), accountReady: true }));

vi.mock('../setup/configApi', () => ({
  loadConfig: async () => ({ provider: 'claude', embedder_url: 'http://embedder' }),
}));

vi.mock('../setup/setupSignals', () => ({
  fetchSetupAccountReady: async () => mocks.accountReady,
  fetchProjectCount: async () => 1,
  fetchHostCount: async () => 0,
  fetchGitIdentityReady: async () => false,
}));

vi.mock('../setup/setupState', async (importOriginal) => ({
  ...await importOriginal<typeof import('../setup/setupState')>(),
  requestOpenWizard: mocks.open,
}));

afterEach(() => {
  cleanup();
  mocks.open.mockClear();
  mocks.accountReady = true;
  localStorage.clear();
});

describe('Setup chip Git-identity recovery', () => {
  it('keeps one actionable item visible after identity is skipped', async () => {
    localStorage.setItem(DISMISSED_KEY, '1');
    render(<SetupChip />);
    const chip = await waitFor(() => screen.getByRole('button', { name: /Setup — 1 left/ }));

    fireEvent.click(chip);

    expect(mocks.open).toHaveBeenCalledOnce();
  });
});

describe('First-boot setup after reinstall', () => {
  it('opens automatically despite a Finish flag left by the previous install', async () => {
    localStorage.setItem(DISMISSED_KEY, '1');
    mocks.accountReady = false;
    render(<SetupChip />);

    await waitFor(() => expect(mocks.open).toHaveBeenCalledOnce());
    expect(screen.getByRole('button', { name: /Setup — 2 left/ })).toBeTruthy();

    fireEvent.focus(window);
    await waitFor(() => expect(screen.getByRole('button', { name: /Setup — 2 left/ })).toBeTruthy());
    expect(mocks.open).toHaveBeenCalledOnce();
  });

  it('honors dismissal once the server account is set up', async () => {
    localStorage.setItem(DISMISSED_KEY, '1');
    render(<SetupChip />);

    await screen.findByRole('button', { name: /Setup — 1 left/ });
    expect(mocks.open).not.toHaveBeenCalled();
  });

  it('opens incomplete setup in a new browser', async () => {
    render(<SetupChip />);

    await waitFor(() => expect(mocks.open).toHaveBeenCalledOnce());
  });
});
