/** @vitest-environment jsdom */
import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { useState } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ApiError } from '../api';
import Fleet, { canonicalTeam, managementAvailable, parseSafeConfigResponse, validAgent, type FleetServer } from './Fleet';

const mocks = vi.hoisted(() => ({ get: vi.fn(), text: vi.fn(), send: vi.fn(), ack: vi.fn() }));

vi.mock('../api', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../api')>();
  return { ...actual, apiGet: mocks.get, apiGetText: mocks.text, fleetSend: mocks.send, acknowledgeFleetMutation: mocks.ack };
});

const server: FleetServer = {
  server_id: 'server-1',
  mgmt_cert_cn: 'server-1-mgmt',
  endpoint: 'https://server-1.example',
  status: 'active',
  health: 'ok',
  version: '1.2.3',
};

function FleetHarness() {
  const [blocked, setBlocked] = useState(false);
  return <Fleet mutationBlocked={blocked} onMutationBlocked={() => setBlocked(true)} />;
}

function FleetNavigationHarness() {
  const [blocked, setBlocked] = useState(false);
  const [showFleet, setShowFleet] = useState(true);
  return <>
    <button onClick={() => setShowFleet(false)}>Navigate away</button>
    <button onClick={() => setShowFleet(true)}>Return to fleet</button>
    {showFleet && <Fleet mutationBlocked={blocked} onMutationBlocked={() => setBlocked(true)} />}
  </>;
}

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
  mocks.text.mockReset();
  mocks.send.mockReset().mockResolvedValue('ack-token');
  mocks.ack.mockReset().mockResolvedValue(undefined);
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

  it('preserves the full signed-64-bit team id while parsing safe config', () => {
    const result = parseSafeConfigResponse('{"server_id":"server-1","team":9223372036854775807,"config":{"mtls":"required","remote_writes":"off","client_transport":"socket","cli_session_forwarding":false,"require_aimee_git":true}}');
    expect(result?.team).toBe('9223372036854775807');
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
    render(<FleetHarness />);
    await loadFleet();
    expect(screen.getByText('1.2.3')).toBeTruthy();
    expect(screen.getByText('Configured')).toBeTruthy();
    expect(mocks.get).toHaveBeenCalledWith('/v1/servers?team=7');
  });

  it('surfaces live-health degradation', async () => {
    mocks.get.mockResolvedValueOnce({ servers: [server] }).mockRejectedValueOnce(new ApiError(503));
    render(<FleetHarness />);
    await loadFleet();
    fireEvent.click(screen.getByRole('button', { name: 'Verify' }));
    expect(await screen.findByText('Fleet request failed (HTTP 503).')).toBeTruthy();
  });

  it('loads the bounded agents drill-down with the OIDC fleet route', async () => {
    mocks.get.mockResolvedValueOnce({ servers: [server] }).mockResolvedValueOnce({
      server_id: 'server-1', team: 7, agents: [{
        name: 'agent.one', provider: 'openai', model: 'gpt-5', enabled: true,
        delegate_available: false, primary_only: true, max_parallel: 2,
      }],
    });
    render(<FleetHarness />);
    await loadFleet();
    fireEvent.click(screen.getByRole('button', { name: 'server-1' }));
    fireEvent.click(screen.getByRole('button', { name: 'Load agents' }));
    expect(await screen.findByText('agent.one')).toBeTruthy();
    expect(screen.getByText('gpt-5')).toBeTruthy();
    expect(mocks.get).toHaveBeenLastCalledWith('/v1/servers/server-1/agents?team=7');
    expect(mocks.send).not.toHaveBeenCalled();
    expect(mocks.ack).not.toHaveBeenCalled();
  });

  it('deduplicates an in-flight agents read and reports policy denial without mutation ACK', async () => {
    let rejectRead!: (error: Error) => void;
    mocks.get.mockResolvedValueOnce({ servers: [server] }).mockImplementationOnce(
      () => new Promise((_resolve, reject) => { rejectRead = reject; }),
    );
    render(<FleetHarness />);
    await loadFleet();
    fireEvent.click(screen.getByRole('button', { name: 'server-1' }));
    const load = screen.getByRole('button', { name: 'Load agents' });
    fireEvent.click(load);
    expect((load as HTMLButtonElement).disabled).toBe(true);
    fireEvent.click(load);
    expect(mocks.get).toHaveBeenCalledTimes(2);
    rejectRead(new ApiError(403));
    expect(await screen.findByText('Denied by team or server management policy.')).toBeTruthy();
    expect(mocks.send).not.toHaveBeenCalled();
    expect(mocks.ack).not.toHaveBeenCalled();
  });

  it('does not commit an agents response after the selected server changes', async () => {
    const second = { ...server, server_id: 'server-2' };
    let resolveRead!: (value: unknown) => void;
    mocks.get.mockResolvedValueOnce({ servers: [server, second] }).mockImplementationOnce(
      () => new Promise((resolve) => { resolveRead = resolve; }),
    );
    render(<FleetHarness />);
    await loadFleet();
    fireEvent.click(screen.getByRole('button', { name: 'server-1' }));
    act(() => {
      (screen.getByRole('button', { name: 'Load agents' }) as HTMLButtonElement).click();
      (screen.getByRole('button', { name: 'server-2' }) as HTMLButtonElement).click();
    });
    await act(async () => resolveRead({
      server_id: 'server-1', team: 7, agents: [{
        name: 'stale-agent', provider: 'local', model: 'model', enabled: true,
        delegate_available: true, primary_only: false, max_parallel: 1,
      }],
    }));
    expect(screen.queryByText('stale-agent')).toBeNull();
    expect(screen.getByText('Agents on server-2')).toBeTruthy();
  });

  it('loads the fixed safe-config projection and clears it on server change', async () => {
    const second = { ...server, server_id: 'server-2' };
    mocks.get.mockResolvedValueOnce({ servers: [server, second] });
    mocks.text.mockResolvedValueOnce('{"server_id":"server-1","team":7,"config":{"mtls":"required","remote_writes":"off","client_transport":"socket","cli_session_forwarding":false,"require_aimee_git":true}}');
    render(<FleetHarness />);
    await loadFleet();
    fireEvent.click(screen.getByRole('button', { name: 'server-1' }));
    fireEvent.click(screen.getByRole('button', { name: 'Load config' }));
    expect(await screen.findByText('required')).toBeTruthy();
    expect(screen.getByText('Remote writes')).toBeTruthy();
    expect(screen.getAllByText('Off').length).toBeGreaterThan(0);
    expect(mocks.text).toHaveBeenLastCalledWith('/v1/servers/server-1/config?team=7');
    expect(mocks.send).not.toHaveBeenCalled();
    expect(mocks.ack).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'server-2' }));
    expect(screen.queryByText('required')).toBeNull();
  });

  it('deduplicates an in-flight config read and clears stale config on denial', async () => {
    let rejectRead!: (error: Error) => void;
    mocks.get.mockResolvedValueOnce({ servers: [server] });
    mocks.text.mockImplementationOnce(
      () => new Promise((_resolve, reject) => { rejectRead = reject; }),
    );
    render(<FleetHarness />);
    await loadFleet();
    fireEvent.click(screen.getByRole('button', { name: 'server-1' }));
    const load = screen.getByRole('button', { name: 'Load config' });
    act(() => {
      (load as HTMLButtonElement).click();
      (load as HTMLButtonElement).click();
    });
    expect((load as HTMLButtonElement).disabled).toBe(true);
    expect(mocks.get).toHaveBeenCalledTimes(1);
    expect(mocks.text).toHaveBeenCalledTimes(1);
    rejectRead(new ApiError(403));
    expect(await screen.findByText('Denied by team or server management policy.')).toBeTruthy();
    expect(screen.queryByText('Remote writes')).toBeNull();
  });

  it('rejects a config response for a different team', async () => {
    mocks.get.mockResolvedValueOnce({ servers: [server] });
    mocks.text.mockResolvedValueOnce('{"server_id":"server-1","team":8,"config":{"mtls":"required","remote_writes":"off","client_transport":"socket","cli_session_forwarding":false,"require_aimee_git":true}}');
    render(<FleetHarness />);
    await loadFleet();
    fireEvent.click(screen.getByRole('button', { name: 'server-1' }));
    fireEvent.click(screen.getByRole('button', { name: 'Load config' }));
    expect(await screen.findByText('Fleet request failed.')).toBeTruthy();
    expect(screen.queryByText('Remote writes')).toBeNull();
  });

  it('does not commit a config response after the selected server changes', async () => {
    const second = { ...server, server_id: 'server-2' };
    let resolveRead!: (value: unknown) => void;
    mocks.get.mockResolvedValueOnce({ servers: [server, second] });
    mocks.text.mockImplementationOnce(
      () => new Promise((resolve) => { resolveRead = resolve; }),
    );
    render(<FleetHarness />);
    await loadFleet();
    fireEvent.click(screen.getByRole('button', { name: 'server-1' }));
    act(() => {
      (screen.getByRole('button', { name: 'Load config' }) as HTMLButtonElement).click();
      (screen.getByRole('button', { name: 'server-2' }) as HTMLButtonElement).click();
    });
    await act(async () => resolveRead('{"server_id":"server-1","team":7,"config":{"mtls":"required","remote_writes":"off","client_transport":"socket","cli_session_forwarding":false,"require_aimee_git":true}}'));
    expect(screen.queryByText('Remote writes')).toBeNull();
    expect(screen.getByText('Safe configuration on server-2')).toBeTruthy();
  });

  it('requires confirmation and sends each confirmed action once', async () => {
    mocks.get.mockResolvedValueOnce({ servers: [server] });
    render(<FleetHarness />);
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
    expect(mocks.ack).toHaveBeenCalledTimes(1);
    expect(mocks.ack).toHaveBeenCalledWith('ack-token');
  });

  it('blocks in-flight duplicates and redispatch after an ambiguous failure', async () => {
    mocks.get.mockResolvedValueOnce({ servers: [server] });
    let rejectRequest!: (error: Error) => void;
    mocks.send.mockImplementationOnce(() => new Promise((_resolve, reject) => { rejectRequest = reject; }));
    render(<FleetNavigationHarness />);
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
    fireEvent.click(screen.getByRole('button', { name: 'Navigate away' }));
    fireEvent.click(screen.getByRole('button', { name: 'Return to fleet' }));
    expect(screen.getByText(/Further mutations are blocked for this session/)).toBeTruthy();
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
    mocks.send.mockRejectedValueOnce(new ApiError(403, 'denial-ack'));
    render(<FleetHarness />);
    await loadFleet();
    await selectAgent();
    fireEvent.click(screen.getByRole('button', { name: 'Enable' }));
    expect(await screen.findByText('Denied by team or server management policy.')).toBeTruthy();
    expect(mocks.ack).toHaveBeenCalledTimes(1);
    expect(mocks.ack).toHaveBeenCalledWith('denial-ack');
    expect((screen.getByRole('button', { name: 'Enable' }) as HTMLButtonElement).disabled).toBe(false);
  });

  it('blocks future actions when a definite result cannot be acknowledged', async () => {
    mocks.get.mockResolvedValueOnce({ servers: [server] });
    mocks.send.mockRejectedValueOnce(new ApiError(403, 'denial-ack'));
    mocks.ack.mockRejectedValueOnce(new ApiError(503));
    render(<FleetHarness />);
    await loadFleet();
    await selectAgent();
    fireEvent.click(screen.getByRole('button', { name: 'Enable' }));
    expect(await screen.findByText(/session latch could not be acknowledged/)).toBeTruthy();
    expect((screen.getByRole('button', { name: 'Enable' }) as HTMLButtonElement).disabled).toBe(true);
    expect(mocks.send).toHaveBeenCalledTimes(1);
  });
});
