import { useState } from 'react';
import { acknowledgeFleetMutation, apiGet, fleetSend, ApiError } from '../api';

export interface FleetServer {
  server_id: string;
  mgmt_cert_cn: string;
  endpoint: string;
  status: string;
  health: string;
  version: string;
}

export interface FleetAgent {
  name: string;
  provider: string;
  model: string;
  enabled: boolean;
  delegate_available: boolean;
  primary_only: boolean;
  max_parallel: number;
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

export function managementAvailable(server: FleetServer): boolean {
  return server.status === 'active' && server.mgmt_cert_cn.length > 0 && server.endpoint.length > 0;
}

export function fleetError(error: unknown): string {
  if (error instanceof ApiError) {
    if (error.status === 403) return 'Denied by team or server management policy.';
    if (error.status === 401) return 'OIDC session expired. Sign in again.';
    if (error.status === 409) return 'A prior management intent is unresolved; it was not retried.';
    if (error.status === 502) return 'The management result is ambiguous; it was not retried.';
    return `Fleet request failed (HTTP ${error.status}).`;
  }
  return 'Fleet request failed.';
}

interface FleetProps {
  mutationBlocked: boolean;
  onMutationBlocked: () => void;
}

export default function Fleet({ mutationBlocked, onMutationBlocked }: FleetProps) {
  const [team, setTeam] = useState('');
  const [servers, setServers] = useState<FleetServer[]>([]);
  const [selected, setSelected] = useState('');
  const [agents, setAgents] = useState<FleetAgent[]>([]);
  const [agent, setAgent] = useState('');
  const [message, setMessage] = useState('');
  const [busy, setBusy] = useState(false);
  const teamID = canonicalTeam(team);

  function changeTeam(value: string) {
    setTeam(value);
    setServers([]);
    setSelected('');
    setAgents([]);
    setMessage('');
  }

  async function loadFleet() {
    if (!teamID) return;
    setBusy(true);
    setMessage('');
    try {
      const result = await apiGet<{ servers: FleetServer[] }>(`/v1/servers?team=${teamID}`);
      setServers(result.servers ?? []);
      setSelected('');
      setAgents([]);
    } catch (error) {
      setMessage(fleetError(error));
    } finally {
      setBusy(false);
    }
  }

  function selectServer(serverID: string) {
    setSelected(serverID);
    setAgents([]);
    setMessage('');
  }

  async function loadAgents() {
    if (!teamID || !selected || busy) return;
    setBusy(true);
    setMessage('');
    setAgents([]);
    try {
      const result = await apiGet<{ server_id: string; team: number; agents: FleetAgent[] }>(
        `/v1/servers/${encodeURIComponent(selected)}/agents?team=${teamID}`,
      );
      if (result.server_id !== selected || String(result.team) !== teamID || !Array.isArray(result.agents)) {
        throw new Error('invalid fleet agent response');
      }
      setAgents(result.agents);
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
    if (!teamID || !selected || !validAgent(agent) || busy || mutationBlocked) return;
    if (!window.confirm(`${action === 'agent.enable' ? 'Enable' : 'Disable'} ${agent} on ${selected}?`)) return;
    setBusy(true);
    setMessage('');
    try {
      const ack = await fleetSend('POST', `/v1/servers/${encodeURIComponent(selected)}/actions?team=${teamID}`, {
        action,
        agent,
      });
      try {
        await acknowledgeFleetMutation(ack);
      } catch {
        onMutationBlocked();
        setMessage('The action result was received, but its session latch could not be acknowledged. Sign in again only after operator verification.');
        return;
      }
      setMessage(`${action} succeeded for ${agent} on ${selected}.`);
    } catch (error) {
      // Management mutations are intentionally never retried here.
      if (error instanceof ApiError && error.status !== 409 && error.status !== 502) {
        // fetch delivered a definite HTTP result even though it was non-2xx.
        // Acknowledge that result before allowing another mutation.
        try {
          await acknowledgeFleetMutation(error.fleetAck);
        } catch {
          onMutationBlocked();
          setMessage('The action result was received, but its session latch could not be acknowledged. Sign in again only after operator verification.');
          return;
        }
      } else {
        onMutationBlocked();
      }
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
        <input value={team} disabled={busy} onChange={(e) => changeTeam(e.target.value)} inputMode="numeric" />
      </label>
      <button disabled={!teamID || busy} onClick={loadFleet}>Load fleet</button>
      {team && !teamID && <p className="kbc-error">Use a canonical positive 64-bit team id.</p>}
      {message && <p>{message}</p>}
      <table>
        <thead><tr><th>Server</th><th>Status</th><th>Heartbeat health</th><th>Version</th><th>Management</th><th>Live</th></tr></thead>
        <tbody>
          {servers.map((server) => (
            <tr key={server.server_id}>
              <td><button disabled={busy} onClick={() => selectServer(server.server_id)}>{server.server_id}</button></td>
              <td>{server.status}</td><td>{server.health}</td><td>{server.version}</td>
              <td>{managementAvailable(server) ? 'Configured' : 'Unavailable'}</td>
              <td><button disabled={busy} onClick={() => liveHealth(server.server_id)}>Verify</button></td>
            </tr>
          ))}
        </tbody>
      </table>
      {mutationBlocked && <p className="kbc-error">Further mutations are blocked for this session. Resolve the prior result before signing in again.</p>}
      {selected && (
        <>
          <fieldset>
            <legend>Agents on {selected}</legend>
            <button disabled={busy} onClick={loadAgents}>Load agents</button>
            {agents.length > 0 && <table>
              <thead><tr><th>Name</th><th>Provider</th><th>Model</th><th>Enabled</th><th>Delegate</th><th>Primary only</th><th>Parallel</th></tr></thead>
              <tbody>{agents.map((item) => <tr key={item.name}>
                <td>{item.name}</td><td>{item.provider}</td><td>{item.model}</td>
                <td>{item.enabled ? 'Yes' : 'No'}</td>
                <td>{item.delegate_available ? 'Yes' : 'No'}</td>
                <td>{item.primary_only ? 'Yes' : 'No'}</td><td>{item.max_parallel}</td>
              </tr>)}</tbody>
            </table>}
          </fieldset>
          <fieldset>
            <legend>Agent action on {selected}</legend>
            <label>Agent <input value={agent} onChange={(e) => setAgent(e.target.value)} /></label>
            <button disabled={busy || mutationBlocked || !validAgent(agent)} onClick={() => mutate('agent.enable')}>Enable</button>
            <button disabled={busy || mutationBlocked || !validAgent(agent)} onClick={() => mutate('agent.disable')}>Disable</button>
            {agent && !validAgent(agent) && <p className="kbc-error">Agent names use 1–63 letters, digits, dot, underscore, or dash.</p>}
          </fieldset>
        </>
      )}
    </section>
  );
}
