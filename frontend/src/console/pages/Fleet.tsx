import { useRef, useState } from 'react';
import { acknowledgeFleetMutation, apiGet, apiGetText, fleetSend, ApiError } from '../api';

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

export interface FleetSafeConfig {
  mtls: 'off' | 'optional' | 'required';
  remote_writes: 'off' | 'data' | 'full';
  client_transport: 'socket' | 'http' | 'auto';
  cli_session_forwarding: boolean;
  require_aimee_git: boolean;
}

interface FleetSafeConfigResponse {
  server_id: string;
  team: string;
  config: FleetSafeConfig;
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

export function parseSafeConfigResponse(raw: string): FleetSafeConfigResponse | null {
  const matches = [...raw.matchAll(/"team":([1-9][0-9]{0,18})(?=,)/g)];
  if (matches.length !== 1 || matches[0].index === undefined || !canonicalTeam(matches[0][1])) return null;
  const match = matches[0];
  const valueStart = match.index + '"team":'.length;
  const normalized = `${raw.slice(0, valueStart)}"${match[1]}"${raw.slice(valueStart + match[1].length)}`;
  try {
    return JSON.parse(normalized) as FleetSafeConfigResponse;
  } catch {
    return null;
  }
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
  const [safeConfig, setSafeConfig] = useState<FleetSafeConfig | null>(null);
  const [agent, setAgent] = useState('');
  const [message, setMessage] = useState('');
  const [busy, setBusy] = useState(false);
  const busyRef = useRef(false);
  const selectionEpoch = useRef(0);
  const teamID = canonicalTeam(team);

  function beginRequest(): boolean {
    if (busyRef.current) return false;
    busyRef.current = true;
    setBusy(true);
    return true;
  }

  function endRequest() {
    busyRef.current = false;
    setBusy(false);
  }

  function changeTeam(value: string) {
    selectionEpoch.current += 1;
    setTeam(value);
    setServers([]);
    setSelected('');
    setAgents([]);
    setSafeConfig(null);
    setMessage('');
  }

  async function loadFleet() {
    if (!teamID || !beginRequest()) return;
    const requestEpoch = selectionEpoch.current;
    setMessage('');
    try {
      const result = await apiGet<{ servers: FleetServer[] }>(`/v1/servers?team=${teamID}`);
      if (requestEpoch !== selectionEpoch.current) return;
      setServers(result.servers ?? []);
      setSelected('');
      setAgents([]);
      setSafeConfig(null);
    } catch (error) {
      if (requestEpoch === selectionEpoch.current) setMessage(fleetError(error));
    } finally {
      endRequest();
    }
  }

  function selectServer(serverID: string) {
    selectionEpoch.current += 1;
    setSelected(serverID);
    setAgents([]);
    setSafeConfig(null);
    setMessage('');
  }

  async function loadAgents() {
    if (!teamID || !selected || !beginRequest()) return;
    const requestedServer = selected;
    const requestEpoch = selectionEpoch.current;
    setMessage('');
    setAgents([]);
    try {
      const result = await apiGet<{ server_id: string; team: unknown; agents: FleetAgent[] }>(
        `/v1/servers/${encodeURIComponent(requestedServer)}/agents?team=${teamID}`,
      );
      // JSON numbers cannot preserve every positive int64 team id. The KB has already
      // authenticated and bound the exact canonical query value; keep the UI check to
      // fields it can compare without precision loss.
      if (requestEpoch !== selectionEpoch.current) return;
      if (result.server_id !== requestedServer || !Array.isArray(result.agents)) {
        throw new Error('invalid fleet agent response');
      }
      setAgents(result.agents);
    } catch (error) {
      if (requestEpoch === selectionEpoch.current) setMessage(fleetError(error));
    } finally {
      endRequest();
    }
  }

  async function loadConfig() {
    if (!teamID || !selected || !beginRequest()) return;
    const requestedTeam = teamID;
    const requestedServer = selected;
    const requestEpoch = selectionEpoch.current;
    setMessage('');
    setSafeConfig(null);
    try {
      const result = parseSafeConfigResponse(await apiGetText(
        `/v1/servers/${encodeURIComponent(requestedServer)}/config?team=${requestedTeam}`,
      ));
      if (requestEpoch !== selectionEpoch.current) return;
      const config = result?.config;
      if (!result || result.server_id !== requestedServer || result.team !== requestedTeam || !config ||
          !['off', 'optional', 'required'].includes(config.mtls) ||
          !['off', 'data', 'full'].includes(config.remote_writes) ||
          !['socket', 'http', 'auto'].includes(config.client_transport) ||
          typeof config.cli_session_forwarding !== 'boolean' ||
          typeof config.require_aimee_git !== 'boolean') {
        throw new Error('invalid fleet config response');
      }
      setSafeConfig(config);
    } catch (error) {
      if (requestEpoch === selectionEpoch.current) setMessage(fleetError(error));
    } finally {
      endRequest();
    }
  }

  async function liveHealth(serverID: string) {
    if (!teamID || !beginRequest()) return;
    const requestEpoch = selectionEpoch.current;
    setMessage('');
    try {
      await apiGet(`/v1/servers/${encodeURIComponent(serverID)}/health?team=${teamID}`);
      if (requestEpoch === selectionEpoch.current)
        setMessage(`${serverID} passed live management health verification.`);
    } catch (error) {
      if (requestEpoch === selectionEpoch.current) setMessage(fleetError(error));
    } finally {
      endRequest();
    }
  }

  async function mutate(action: 'agent.enable' | 'agent.disable') {
    if (!teamID || !selected || !validAgent(agent) || busyRef.current || mutationBlocked) return;
    if (!window.confirm(`${action === 'agent.enable' ? 'Enable' : 'Disable'} ${agent} on ${selected}?`)) return;
    if (!beginRequest()) return;
    const requestEpoch = selectionEpoch.current;
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
        if (requestEpoch === selectionEpoch.current)
          setMessage('The action result was received, but its session latch could not be acknowledged. Sign in again only after operator verification.');
        return;
      }
      if (requestEpoch === selectionEpoch.current)
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
          if (requestEpoch === selectionEpoch.current)
            setMessage('The action result was received, but its session latch could not be acknowledged. Sign in again only after operator verification.');
          return;
        }
      } else {
        onMutationBlocked();
      }
      if (requestEpoch === selectionEpoch.current) setMessage(fleetError(error));
    } finally {
      endRequest();
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
            <legend>Safe configuration on {selected}</legend>
            <button disabled={busy} onClick={loadConfig}>Load config</button>
            {safeConfig && <table>
              <thead><tr><th>Setting</th><th>Value</th></tr></thead>
              <tbody>
                <tr><td>mTLS</td><td>{safeConfig.mtls}</td></tr>
                <tr><td>Remote writes</td><td>{safeConfig.remote_writes}</td></tr>
                <tr><td>Client transport</td><td>{safeConfig.client_transport}</td></tr>
                <tr><td>CLI session forwarding</td><td>{safeConfig.cli_session_forwarding ? 'On' : 'Off'}</td></tr>
                <tr><td>Require Aimee Git</td><td>{safeConfig.require_aimee_git ? 'On' : 'Off'}</td></tr>
              </tbody>
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
