/** @vitest-environment jsdom */
import { useState } from 'react';
import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import SetupWizard from './SetupWizard';
import SetupChip from './SetupChip';
import { DISMISSED_KEY, OPEN_WIZARD_EVENT, setDismissed } from '../setup/setupState';

const scope = 'a'.repeat(64);
const config = vi.hoisted(() => ({ save: vi.fn() }));
vi.mock('@rakuensoftware/smoothgui', () => ({
  Button: ({ children, ...rest }: { children?: unknown } & Record<string, unknown>) => <button {...rest}>{children as never}</button>,
  useToast: () => Object.assign(() => {}, { error: () => {}, success: () => {}, info: () => {} }),
}));
vi.mock('../setup/configApi', () => ({ loadConfig: async () => ({ provider: 'claude', embedder_model: 'bundled' }), saveConfigValue: config.save }));
vi.mock('../setup/setupSignals', async original => ({
  ...await original<typeof import('../setup/setupSignals')>(),
  fetchAppliance: async () => false, fetchHostCount: async () => 1,
  fetchProjectCount: async () => 1, fetchGitIdentityReady: async () => true,
}));
vi.mock('../setup/CreateAccount', () => ({ default: () => null }));
vi.mock('../setup/PrimaryChooser', () => ({ default: () => null }));
vi.mock('../setup/DeployTopology', () => ({ default: () => null }));
vi.mock('../setup/DeployPanel', () => ({ default: () => null }));
vi.mock('../setup/ConnectHosts', () => ({ default: () => null }));
vi.mock('../setup/ConnectWorkspace', () => ({ default: () => null }));
vi.mock('../setup/GitIdentity', () => ({ default: () => null }));
vi.mock('../setup/MemoryBackend', () => ({ default: () => null }));
function Wizard() {
  const [open, setOpen] = useState(true);
  return <SetupWizard open={open} onClose={() => setOpen(false)} />;
}
afterEach(() => { cleanup(); vi.unstubAllGlobals(); setDismissed(false); localStorage.clear(); config.save.mockClear(); });
function statusFetch(status: Promise<Record<string, unknown>>) {
  const fetcher = vi.fn(async (path: string, init?: RequestInit) => {
    expect(init?.method ?? 'GET').toBe('GET');
    expect(init?.body).toBeUndefined();
    expect(path).toBe('/api/setup/account');
    return { ok: true, json: async () => status };
  });
  vi.stubGlobal('fetch', fetcher);
  return fetcher;
}
it.each(['Later', 'Close setup'])('remembers normal %s for this instance while account readiness stays false', async name => {
  const fetcher = statusFetch(Promise.resolve({ complete: false, dismissal_scope: scope }));
  render(<Wizard />);
  await screen.findByRole('button', { name: 'Later', exact: true });
  fireEvent.click(screen.getByRole('button', { name, exact: true }));
  expect(screen.queryByRole('dialog', { name: 'Setup wizard' })).toBeNull();
  expect(localStorage.getItem(`${DISMISSED_KEY}:scope:${scope}`)).toBe('1');
  cleanup(); setDismissed(false); // A new document loses memory, not the scoped preference.
  const opened = vi.fn(); window.addEventListener(OPEN_WIZARD_EVENT, opened);
  try {
    render(<SetupChip />);
    await screen.findByRole('button', { name: /Setup — 1 left/ });
    expect(opened).not.toHaveBeenCalled();
    expect(config.save).not.toHaveBeenCalled();
    expect(fetcher.mock.calls.every(([, init]) => !init?.method || init.method === 'GET')).toBe(true);
  } finally { window.removeEventListener(OPEN_WIZARD_EVENT, opened); }
});
it('honors close before authenticated status resolves and prevents a late automatic reopen', async () => {
  let resolve!: (value: Record<string, unknown>) => void;
  const pending = new Promise<Record<string, unknown>>(done => { resolve = done; });
  statusFetch(pending);
  render(<Wizard />);
  fireEvent.click(screen.getByRole('button', { name: 'Close setup', exact: true }));
  const opened = vi.fn(); window.addEventListener(OPEN_WIZARD_EVENT, opened);
  try {
    render(<SetupChip />);
    await act(async () => resolve({ complete: false, dismissal_scope: scope }));
    await waitFor(() => expect(localStorage.getItem(`${DISMISSED_KEY}:scope:${scope}`)).toBe('1'));
    await screen.findByRole('button', { name: /Setup — 1 left/ });
    expect(opened).not.toHaveBeenCalled();
    expect(config.save).not.toHaveBeenCalled();
  } finally { window.removeEventListener(OPEN_WIZARD_EVENT, opened); }
});
