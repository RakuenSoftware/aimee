/** @vitest-environment jsdom */
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ApiError } from '../api';
import Fleet, { canonicalTeam, managementAvailable, validAgent, type FleetServer } from './Fleet';

const mocks = vi.hoisted(() => ({ get: vi.fn(), send: vi.fn() }));

vi.mock('../api', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../api')>();
  return { ...actual, apiGet: mocks.get, apiSend: mocks.send };
});

const server: FleetServer = {
  server_id: 'server-1',
  mgmt_cert_cn: 'server-1-mgmt',
  endpoint: 'https://server-1.example',
  status: 'active',
  health: 'ok',
  version: '1.2.3',
};

async function loadFleet() {
  fireEvent.change(screen.getByLabelText('Team id'), { target: { value: '7' } });
  fireEvent.click(screen.getByRole('button', { name: 'Load fleet' }));
  await screen.findByRole('button', { name: 'server-1' });
}

async function selectAgent() {
  fireEvent.click(screen.getByRole('button', { name: 'server-1' }));
  fireEvent.change(screen.getByLabelText('Agent'), { target: { value: 'agent.one' } });
}

beforeEach(() => {
  mocks.get.mockReset();
  mocks.send.mockReset();
  vi.spyOn(window, 'confirm').mockReturnValue(true);
});

afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
});

describe('fleet input contracts', () => {
  it('accepts only canonical positive signed-64-bit team ids', () => {
    expect(canonicalTeam('1')).toBe('1');
    expect(canonicalTeam('9223372036854775807')).toBe('9223372036854775807');
    for (const bad of ['', '0', '01', '+1', '-1', '1.0', '9223372036854775808']) {
      expect(canonicalTeam(bad)).toBeNull();
    }
  });

  it('matches agent grammar and management prerequisites', () => {
    expect(validAgent('agent.one-2')).toBe(true);
    expect(validAgent('')).toBe(false);
    expect(validAgent('a/b')).toBe(false);
    expect(validAgent('a'.repeat(64))).toBe(false);
    expect(managementAvailable(server)).toBe(true);
    expect(managementAvailable({ ...server, endpoint: '' })).toBe(false);
  });
});

describe('Fleet interactions', () => {
  it('renders registry state and management availability', async () => {
    mocks.get.mockResolvedValueOnce({ servers: [server] });
    render(<Fleet />);
    await loadFleet();
    expect(screen.getByText('1.2.3')).toBeTruthy();
    expect(screen.getByText('Configured')).toBeTruthy();
    expect(mocks.get).toHaveBeenCalledWith('/v1/servers?team=7');
  });

  it('surfaces live-health degradation', async () => {
    mocks.get.mockResolvedValueOnce({ servers: [server] }).mockRejectedValueOnce(new ApiError(503));
    render(<Fleet />);
    await loadFleet();
    fireEvent.click(screen.getByRole('button', { name: 'Verify' }));
    expect(await screen.findByText('Fleet request failed (HTTP 503).')).toBeTruthy();
  });

  it('requires confirmation and sends each confirmed action once', async () => {
    mocks.get.mockResolvedValueOnce({ servers: [server] });
    mocks.send.mockResolvedValue({ status: 'applied' });
    render(<Fleet />);
    await loadFleet();
    await selectAgent();
    vi.mocked(window.confirm).mockReturnValueOnce(false).mockReturnValueOnce(true);
    fireEvent.click(screen.getByRole('button', { name: 'Enable' }));
    expect(mocks.send).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Disable' }));
    await waitFor(() => expect(mocks.send).toHaveBeenCalledTimes(1));
    expect(mocks.send).toHaveBeenCalledWith('POST', '/v1/servers/server-1/actions?team=7', {
      action: 'agent.disable', agent: 'agent.one',
    });
  });

  it('blocks in-flight duplicates and redispatch after an ambiguous failure', async () => {
    mocks.get.mockResolvedValueOnce({ servers: [server] });
    let rejectRequest!: (error: Error) => void;
    mocks.send.mockImplementationOnce(() => new Promise((_resolve, reject) => { rejectRequest = reject; }));
    render(<Fleet />);
    await loadFleet();
    await selectAgent();
    fireEvent.click(screen.getByRole('button', { name: 'Enable' }));
    expect((screen.getByRole('button', { name: 'Disable' }) as HTMLButtonElement).disabled).toBe(true);
    fireEvent.click(screen.getByRole('button', { name: 'Disable' }));
    expect(mocks.send).toHaveBeenCalledTimes(1);
    rejectRequest(new Error('connection reset'));
    expect(await screen.findByText(/Further mutations are blocked for this session/)).toBeTruthy();
    expect((screen.getByRole('button', { name: 'Enable' }) as HTMLButtonElement).disabled).toBe(true);
    fireEvent.click(screen.getByRole('button', { name: 'Enable' }));
    expect(mocks.send).toHaveBeenCalledTimes(1);
    fireEvent.change(screen.getByLabelText('Team id'), { target: { value: '8' } });
    expect(screen.getByText(/Further mutations are blocked for this session/)).toBeTruthy();
    expect(mocks.send).toHaveBeenCalledTimes(1);
    mocks.get.mockResolvedValueOnce({ servers: [server] });
    fireEvent.click(screen.getByRole('button', { name: 'Load fleet' }));
    await screen.findByRole('button', { name: 'server-1' });
    await selectAgent();
    expect((screen.getByRole('button', { name: 'Enable' }) as HTMLButtonElement).disabled).toBe(true);
    fireEvent.click(screen.getByRole('button', { name: 'Enable' }));
    expect(mocks.send).toHaveBeenCalledTimes(1);
  });

  it('surfaces policy denial without locking future actions', async () => {
    mocks.get.mockResolvedValueOnce({ servers: [server] });
    mocks.send.mockRejectedValueOnce(new ApiError(403));
    render(<Fleet />);
    await loadFleet();
    await selectAgent();
    fireEvent.click(screen.getByRole('button', { name: 'Enable' }));
    expect(await screen.findByText('Denied by team policy or the server remote_writes policy.')).toBeTruthy();
    expect((screen.getByRole('button', { name: 'Enable' }) as HTMLButtonElement).disabled).toBe(false);
  });
});
