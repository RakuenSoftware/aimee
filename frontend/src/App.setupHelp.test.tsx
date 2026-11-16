/** @vitest-environment jsdom */
import type { ReactNode } from 'react';
import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { MemoryRouter } from 'react-router-dom';
import App from './App';
import { hasSeen } from './help/tutorialState';
import { requestOpenWizard, setDismissed } from './setup/setupState';

const fixture = vi.hoisted(() => ({ ready: false, save: vi.fn() }));
vi.mock('@rakuensoftware/smoothgui', () => ({
  ErrorBoundary: ({ children }: { children: ReactNode }) => <>{children}</>,
  Toast: () => null, ThemeToggle: () => null,
  Button: ({ children, ...props }: { children: ReactNode } & Record<string, unknown>) => <button {...props}>{children}</button>,
  useToast: () => Object.assign(() => {}, { error: () => {}, success: () => {}, info: () => {} }),
}));
vi.mock('./SessionContext', () => ({
  SessionProvider: ({ children }: { children: ReactNode }) => <>{children}</>,
  useSessions: () => ({ sessions: [], activeId: '', addSession: vi.fn(), closeSession: vi.fn(), selectSession: vi.fn(), renameSession: vi.fn() }),
}));
vi.mock('./components/HealthBanner', () => ({ default: () => null }));
vi.mock('./setup/configApi', () => ({
  loadConfig: async () => ({ provider: fixture.ready ? 'claude' : '', memory_backend_engine: 'native', embedder_model: 'bundled' }),
  saveConfigValue: fixture.save,
}));
vi.mock('./setup/setupSignals', () => ({
  fetchAppliance: async () => false, fetchHostCount: async () => 1,
  fetchProjectCount: async () => 1, fetchGitIdentityReady: async () => true,
  fetchSetupAccountStatus: async () => ({ complete: true, dismissal_scope: 'a'.repeat(64) }),
}));
vi.mock('./pages/Chat', () => ({ default: () => null }));
vi.mock('./pages/Dashboard', () => ({ default: () => null }));
vi.mock('./pages/Logs', () => ({ default: () => null }));
vi.mock('./pages/EditWorkflows', () => ({ default: () => null }));
vi.mock('./pages/WorkflowActions', () => ({ default: () => null }));
vi.mock('./pages/Providers', () => ({ default: () => null }));
vi.mock('./pages/Models', () => ({ default: () => null }));
vi.mock('./pages/Personas', () => ({ default: () => null }));
vi.mock('./pages/Roles', () => ({ default: () => null }));
vi.mock('./pages/Roundtable', () => ({ default: () => null }));
vi.mock('./pages/Settings', () => ({ default: () => null }));
vi.mock('./pages/Projects', () => ({ default: () => null }));
vi.mock('./pages/Graph', () => ({ default: () => null }));
vi.mock('./pages/Editor', () => ({ default: () => null }));
vi.mock('./pages/Memory', () => ({ default: () => null }));
vi.mock('./setup/CreateAccount', () => ({ default: () => null }));
vi.mock('./setup/PrimaryChooser', () => ({ default: () => null }));
vi.mock('./setup/DeployTopology', () => ({ default: () => null }));
vi.mock('./setup/DeployPanel', () => ({ default: () => null }));
vi.mock('./setup/ConnectHosts', () => ({ default: () => null }));
vi.mock('./setup/ConnectWorkspace', () => ({ default: () => null }));
vi.mock('./setup/GitIdentity', () => ({ default: () => null }));
vi.mock('./setup/MemoryBackend', () => ({ default: () => null }));

beforeEach(() => {
  fixture.ready = false;
  setDismissed(false);
  localStorage.clear();
  vi.stubGlobal('fetch', vi.fn(async (path: string, init?: RequestInit) => {
    expect(path).toBe('/api/chat/session');
    expect(init?.method ?? 'GET').toBe('GET');
    return { status: 200, json: async () => ({ csrf: 'fixture-csrf' }) };
  }));
});
afterEach(() => { cleanup(); vi.unstubAllGlobals(); localStorage.clear(); setDismissed(false); fixture.save.mockClear(); });
function mount() { render(<MemoryRouter initialEntries={['/chat']}><App /></MemoryRouter>); }
function expectSetupPriority() {
  expect(screen.getAllByRole('dialog').length).toBe(1);
  expect(screen.queryByRole('dialog', { name: 'Chat tab help' })).toBeNull();
  expect(screen.queryByRole('button', { name: 'Show tab help' })).toBeNull();
  expect(hasSeen('/chat')).toBe(false);
  expect(fixture.save).not.toHaveBeenCalled();
}
it('defers untouched Chat help behind automatic setup, then preserves a real help acknowledgement on reopening setup', async () => {
  mount();
  await screen.findByRole('dialog', { name: 'Setup wizard' });
  expectSetupPriority();
  fireEvent.click(await screen.findByRole('button', { name: 'Later', exact: true }));
  await screen.findByRole('dialog', { name: 'Chat tab help' });
  expect(hasSeen('/chat')).toBe(false);
  fireEvent.click(screen.getByRole('button', { name: 'Got it', exact: true }));
  expect(hasSeen('/chat')).toBe(true);
  fireEvent.click(screen.getByRole('button', { name: /Setup — 1 left/ }));
  await screen.findByRole('dialog', { name: 'Setup wizard' });
  expect(screen.getAllByRole('dialog').length).toBe(1);
  expect(screen.queryByRole('button', { name: 'Show tab help' })).toBeNull();
  fireEvent.click(screen.getByRole('button', { name: 'Close setup', exact: true }));
  await screen.findByRole('button', { name: 'Show tab help' });
  expect(screen.queryByRole('dialog', { name: 'Chat tab help' })).toBeNull();
  expect(hasSeen('/chat')).toBe(true);
  expect(fixture.save).not.toHaveBeenCalled();
});
it('Escape closes setup without acknowledging or consuming the deferred tutorial', async () => {
  mount();
  await screen.findByRole('dialog', { name: 'Setup wizard' });
  expectSetupPriority();
  fireEvent.keyDown(window, { key: 'Escape' });
  await screen.findByRole('dialog', { name: 'Chat tab help' });
  expect(hasSeen('/chat')).toBe(false);
  expect(fixture.save).not.toHaveBeenCalled();
});
it('restores unacknowledged help after normally finishing an already-complete setup', async () => {
  fixture.ready = true;
  mount();
  await screen.findByRole('dialog', { name: 'Chat tab help' });
  act(() => requestOpenWizard());
  await screen.findByRole('dialog', { name: 'Setup wizard' });
  expectSetupPriority();
  fireEvent.click(await screen.findByRole('button', { name: 'Finish', exact: true }));
  await screen.findByRole('dialog', { name: 'Chat tab help' });
  await waitFor(() => expect(screen.queryByRole('dialog', { name: 'Setup wizard' })).toBeNull());
  expect(hasSeen('/chat')).toBe(false);
  expect(fixture.save).not.toHaveBeenCalled();
});
