/* Connections and credentials live here; model configuration lives on Models. */
import { useCallback, useEffect, useState } from "react";
import { Panel, Badge, Spinner, Modal, InlineStatus, EmptyState, Button } from "@rakuensoftware/smoothgui";
import { providerRequest, type ProviderConnection } from "../providers/api";

const inputStyle: React.CSSProperties = { width: "100%", boxSizing: "border-box", padding: "6px 8px", marginTop: 4 };
const protocols = ["openai", "anthropic", "gemini", "mistral", "ollama", "llama_native", "claude", "chatgpt"];

function ProviderEditor({ connection, onClose, onSaved }: {
  connection: ProviderConnection | null;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [name, setName] = useState(connection?.name || "");
  const [provider, setProvider] = useState(connection?.provider || "openai");
  const [endpoint, setEndpoint] = useState(connection?.endpoint || "");
  const [auth, setAuth] = useState(connection?.auth_type || "bearer");
  const [key, setKey] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const save = async (event: React.FormEvent) => {
    event.preventDefault();
    setBusy(true);
    setError("");
    try {
      await providerRequest("/api/providers/save", {
        name: name.trim(), provider, endpoint: endpoint.trim(), auth_type: auth,
        create: !connection, ...(key.trim() ? { api_key: key.trim() } : {}),
      });
      onSaved();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Could not save provider");
    } finally { setBusy(false); }
  };
  return (
    <Modal open title={connection ? "Edit provider" : "Add provider"} onClose={() => { if (!busy) onClose(); }} size="md">
      <form onSubmit={save}>
        <div style={{ display: "grid", gap: 12 }}>
          <label>Provider name
            <input style={inputStyle} value={name} onChange={e => setName(e.target.value)} required maxLength={63}
              disabled={busy || !!connection} placeholder="e.g. work-openai" autoFocus={!connection} />
          </label>
          <label>Provider type
            <select style={inputStyle} value={provider} disabled={busy || !!connection?.model_count}
              onChange={e => { setProvider(e.target.value); setAuth(e.target.value === "anthropic" ? "x-api-key" : e.target.value === "claude" ? "none" : "bearer"); }}>
              {[...new Set([...protocols, provider])].map(p => <option key={p}>{p}</option>)}
            </select>
          </label>
          <label>Endpoint
            <input style={inputStyle} value={endpoint} onChange={e => setEndpoint(e.target.value)} disabled={busy}
              placeholder="https://api.example.com/v1" required={provider !== "claude" && provider !== "claude-code"} />
          </label>
          <label>Authentication
            <select style={inputStyle} value={auth} onChange={e => setAuth(e.target.value)} disabled={busy}>
              {[...new Set(["bearer", "x-api-key", "none", auth])].map(a => <option key={a}>{a}</option>)}
            </select>
          </label>
          {auth !== "none" && <label>{connection ? "API key (leave blank to keep current)" : "API key"}
            <input style={inputStyle} type="password" autoComplete="new-password" value={key}
              onChange={e => setKey(e.target.value)} disabled={busy} />
          </label>}
        </div>
        {connection && <p>Connection changes apply to its attached models.</p>}
        {error && <InlineStatus status={{ kind: "err", msg: error }} />}
        <div style={{ display: "flex", gap: 8, marginTop: 16 }}>
          <Button type="submit" disabled={busy}>{busy ? "Saving…" : "Save provider"}</Button>
          <Button type="button" onClick={onClose} disabled={busy}>Cancel</Button>
        </div>
      </form>
    </Modal>
  );
}

export default function Providers() {
  const [providers, setProviders] = useState<ProviderConnection[]>([]);
  const [loading, setLoading] = useState(true);
  const [loaded, setLoaded] = useState(false);
  const [status, setStatus] = useState<{ kind: "ok" | "err"; msg: string } | null>(null);
  const [editing, setEditing] = useState<ProviderConnection | null | undefined>(undefined);
  const [deleting, setDeleting] = useState<ProviderConnection | null>(null);
  const [busy, setBusy] = useState(false);
  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      const data = await providerRequest<{ providers: ProviderConnection[] }>("/api/providers");
      setProviders(data.providers);
      setLoaded(true);
    } catch (err) {
      setStatus({ kind: "err", msg: err instanceof Error ? err.message : "Could not load providers" });
    } finally { setLoading(false); }
  }, []);
  useEffect(() => { void refresh(); }, [refresh]);
  const remove = async () => {
    if (!deleting) return;
    setBusy(true);
    try {
      await providerRequest("/api/providers/remove", { name: deleting.name, remove_models: true });
      setStatus({ kind: "ok", msg: `${deleting.name} deleted` });
      setDeleting(null);
      await refresh();
    } catch (err) {
      setStatus({ kind: "err", msg: err instanceof Error ? err.message : "Could not delete provider" });
    } finally { setBusy(false); }
  };
  return (
    <div style={{ padding: 16, fontFamily: "system-ui", height: "100%", overflow: "auto" }}>
      <div style={{ display: "flex", flexWrap: "wrap", gap: 8, alignItems: "center", marginBottom: 12 }}>
        <h2 style={{ margin: 0 }}>Providers</h2>
        <Badge label={String(providers.length)} variant="neutral" />
        <Button onClick={() => setEditing(null)}>+ Add provider</Button>
        <Button onClick={() => void refresh()} disabled={loading}>Refresh</Button>
        <Spinner loading={loading} />
      </div>
      <InlineStatus status={status} />
      {!loading && loaded && !providers.length && <EmptyState message="No providers configured. Add a provider to get started." />}
      <div style={{ display: "grid", gap: 12 }}>
      {providers.map(p => (
        <Panel key={p.name} title={p.name}>
          <div style={{ padding: 12 }}>
          <p style={{ margin: "0 0 6px" }}>{p.provider} · {p.auth_type}</p>
          <p style={{ overflowWrap: "anywhere", margin: "0 0 12px" }}>{p.endpoint || "CLI connection"}</p>
          <div style={{ display: "flex", flexWrap: "wrap", gap: 8 }}>
            <Button onClick={() => setEditing(p)}>Edit provider</Button>
            <Button variant="danger" onClick={() => { setStatus(null); setDeleting(p); }}>Delete provider</Button>
          </div>
          </div>
        </Panel>
      ))}
      </div>
      {editing !== undefined && <ProviderEditor connection={editing} onClose={() => setEditing(undefined)} onSaved={() => {
        setEditing(undefined); setStatus({ kind: "ok", msg: "Provider saved" }); void refresh();
      }} />}
      {deleting && <Modal open title={`Delete ${deleting.name}?`} onClose={() => { if (!busy) setDeleting(null); }} size="md">
        <p>{deleting.model_count ? `This removes the provider and its ${deleting.model_count} attached model(s) from routing.` : "This removes the provider connection."}</p>
        <InlineStatus status={status?.kind === "err" ? status : null} />
        <Button variant="danger" disabled={busy} onClick={() => void remove()}>{busy ? "Deleting…" : "Confirm delete"}</Button>{" "}
        <Button disabled={busy} onClick={() => setDeleting(null)}>Cancel</Button>
      </Modal>}
    </div>
  );
}
