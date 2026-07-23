import { useState } from 'react';
import { apiGet, apiSend, ApiError } from '../api';

export interface FleetServer {
  server_id: string;
  status: string;
  health: string;
  version: string;
  endpoint?: string;
}

const maxInt64 = 9223372036854775807n;

export function canonicalTeam(value: string): string | null {
  if (!/^[1-9][0-9]*$/.test(value)) return null;
  try {
    if (BigInt(value) > maxInt64) return null;
  } catch {
    return null;
  }
  return value;
}

export function validAgent(value: string): boolean {
  return /^[A-Za-z0-9._-]{1,63}$/.test(value);
}

function fleetError(error: unknown): string {
  if (error instanceof ApiError) {
    if (error.status === 403) return 'Denied by team policy or the server remote_writes policy.';
    if (error.status === 401) return 'OIDC session expired. Sign in again.';
    if (error.status === 502) return 'The management result is ambiguous; it was not retried.';
    return `Fleet request failed (HTTP ${error.status}).`;
  }
  return 'Fleet request failed.';
}

export default function Fleet() {
  const [team, setTeam] = useState('');
  const [servers, setServers] = useState<FleetServer[]>([]);
  const [selected, setSelected] = useState('');
  const [agent, setAgent] = useState('');
  const [message, setMessage] = useState('');
  const [busy, setBusy] = useState(false);
  const teamID = canonicalTeam(team);

  async function loadFleet() {
    if (!teamID) return;
    setBusy(true);
    setMessage('');
    try {
      const result = await apiGet<{ servers: FleetServer[] }>(`/v1/servers?team=${teamID}`);
      setServers(result.servers ?? []);
    } catch (error) {
      setMessage(fleetError(error));
    } finally {
      setBusy(false);
    }
  }

  async function liveHealth(serverID: string) {
    if (!teamID || busy) return;
    setBusy(true);
    setMessage('');
    try {
      await apiGet(`/v1/servers/${encodeURIComponent(serverID)}/health?team=${teamID}`);
      setMessage(`${serverID} passed live management health verification.`);
    } catch (error) {
      setMessage(fleetError(error));
    } finally {
      setBusy(false);
    }
  }

  async function mutate(action: 'agent.enable' | 'agent.disable') {
    if (!teamID || !selected || !validAgent(agent) || busy) return;
    if (!window.confirm(`${action === 'agent.enable' ? 'Enable' : 'Disable'} ${agent} on ${selected}?`)) return;
    setBusy(true);
    setMessage('');
    try {
      await apiSend('POST', `/v1/servers/${encodeURIComponent(selected)}/actions?team=${teamID}`, {
        action,
        agent,
      });
      setMessage(`${action} succeeded for ${agent} on ${selected}.`);
    } catch (error) {
      // Management mutations are intentionally never retried here.
      setMessage(fleetError(error));
    } finally {
      setBusy(false);
    }
  }

  return (
    <section>
      <h1>Fleet</h1>
      <p>Inspect one team&apos;s registered servers and perform bounded management actions.</p>
      <label>
        Team id
        <input value={team} onChange={(e) => setTeam(e.target.value)} inputMode="numeric" />
      </label>
      <button disabled={!teamID || busy} onClick={loadFleet}>Load fleet</button>
      {team && !teamID && <p className="kbc-error">Use a canonical positive 64-bit team id.</p>}
      {message && <p>{message}</p>}
      <table>
        <thead><tr><th>Server</th><th>Status</th><th>Heartbeat health</th><th>Version</th><th>Live</th></tr></thead>
        <tbody>
          {servers.map((server) => (
            <tr key={server.server_id}>
              <td><button disabled={busy} onClick={() => setSelected(server.server_id)}>{server.server_id}</button></td>
              <td>{server.status}</td><td>{server.health}</td><td>{server.version}</td>
              <td><button disabled={busy} onClick={() => liveHealth(server.server_id)}>Verify</button></td>
            </tr>
          ))}
        </tbody>
      </table>
      {selected && (
        <fieldset>
          <legend>Agent action on {selected}</legend>
          <label>Agent <input value={agent} onChange={(e) => setAgent(e.target.value)} /></label>
          <button disabled={busy || !validAgent(agent)} onClick={() => mutate('agent.enable')}>Enable</button>
          <button disabled={busy || !validAgent(agent)} onClick={() => mutate('agent.disable')}>Disable</button>
          {agent && !validAgent(agent) && <p className="kbc-error">Agent names use 1–63 letters, digits, dot, underscore, or dash.</p>}
        </fieldset>
      )}
    </section>
  );
}
