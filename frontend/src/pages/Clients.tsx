import { useCallback, useEffect, useState } from "react";
import { Button, Panel } from "@rakuensoftware/smoothgui";
import { remoteSetCommand } from "../setup/DeployPanel";

type Client = { id: string; name: string; state: "pending" | "paired" | "expired" | "revoked"; expires_at: number };
type Invitation = { id: string; bearer_token: string; tls_port: number; expires_at: number };
export async function clientRequest(path: string, body?: unknown) {
  const r = await fetch(path, { method: body === undefined ? "GET" : "POST", cache: "no-store",
    headers: { "Content-Type": "application/json", "X-CSRF-Token": window._csrf || "" },
    ...(body === undefined ? {} : { body: JSON.stringify(body) }) });
  const result = await r.json();
  if (!r.ok) throw new Error(typeof result.error === "string" ? result.error : "Could not manage clients");
  return result;
}

export default function Clients() {
  const [clients, setClients] = useState<Client[]>([]);
  const [name, setName] = useState("");
  const [invitation, setInvitation] = useState<Invitation | null>(null);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [copied, setCopied] = useState(false);
  const refresh = useCallback(async () => {
    const data = await clientRequest("/api/clients");
    setClients(data.clients || []);
    setInvitation(current => current && data.clients?.some((c: Client) => c.id === current.id && c.state === "pending") ? current : null);
  }, []);
  useEffect(() => {
    let active = true;
    const update = () => refresh().catch(e => { if (active) setError(e.message); });
    void update(); const timer = window.setInterval(update, 10000);
    return () => { active = false; window.clearInterval(timer); };
  }, [refresh]);
  async function add() {
    setBusy(true); setError(""); setCopied(false);
    try { const invite = await clientRequest("/api/clients", { name: name.trim() }); setInvitation(invite); setName(""); await refresh(); }
    catch (e) { setError((e as Error).message); } finally { setBusy(false); }
  }
  async function revoke(client: Client) {
    if (!window.confirm(`Revoke ${client.name}? This disconnects this client only.`)) return;
    setBusy(true); setError("");
    try { await clientRequest("/api/clients/revoke", { id: client.id }); if (invitation?.id === client.id) setInvitation(null); await refresh(); }
    catch (e) { setError((e as Error).message); } finally { setBusy(false); }
  }
  const command = invitation ? remoteSetCommand(window.location.hostname, invitation.tls_port || 8743, invitation.bearer_token) : "";
  return <Panel title="Clients">
    <p>Pair multiple devices with this server. Each client has its own certificate and access as your account.</p>
    <label>Client name <input aria-label="Client name" value={name} maxLength={64} onChange={e => setName(e.target.value)} placeholder="Work laptop" /></label>{" "}
    <Button disabled={busy || !name.trim()} onClick={add}>Add client</Button>{" "}
    <Button disabled={busy} onClick={() => refresh().catch(e => setError(e.message))}>Refresh clients</Button>
    {error && <p role="alert">{error}</p>}
    {invitation && <div>
      <p>Run this command on the new Linux client before {new Date(invitation.expires_at * 1000).toLocaleTimeString()}. The token pairs one device and is shown only here. If you lose it, revoke the pending invitation and add another.</p>
      <pre style={{ whiteSpace: "pre-wrap", overflowWrap: "anywhere" }}>{command}</pre>
      <Button onClick={async () => { try { await navigator.clipboard.writeText(command); setCopied(true); } catch { setError("Copy failed; select the command and copy it manually."); } }}>{copied ? "Copied" : "Copy pairing command"}</Button>
      <p>Automatic certificate enrollment currently requires the Linux thin client.</p>
    </div>}
    {clients.length === 0 && !error && <p>No clients have been registered.</p>}
    <ul style={{ paddingLeft: 20 }}>{clients.map(client => <li key={client.id} style={{ marginBottom: 8 }}>
      <strong>{client.name}</strong> — {client.state}{" "}
      {client.state !== "revoked" && <Button disabled={busy} onClick={() => revoke(client)}>Revoke</Button>}
    </li>)}</ul>
  </Panel>;
}
