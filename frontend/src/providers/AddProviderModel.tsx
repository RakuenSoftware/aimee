import { useEffect, useState } from "react";
import { Button, InlineStatus } from "@rakuensoftware/smoothgui";
import { providerRequest, type ProviderConnection } from "./api";

export default function AddProviderModel({ onDone }: { onDone: (msg: string, ok: boolean) => void }) {
  const [providers, setProviders] = useState<ProviderConnection[]>([]);
  const [name, setName] = useState("");
  const [model, setModel] = useState("");
  const [loading, setLoading] = useState(true);
  const [busy, setBusy] = useState(false);
  const [found, setFound] = useState<{ id: string; context_window?: number }[]>([]);
  const [discovering, setDiscovering] = useState(false);
  const [note, setNote] = useState("");
  const [error, setError] = useState("");
  useEffect(() => {
    providerRequest<{ providers: ProviderConnection[] }>("/api/providers")
      .then(d => { setProviders(d.providers); setName(d.providers[0]?.name || ""); })
      .catch(err => setError(err.message))
      .finally(() => setLoading(false));
  }, []);
  const discover = async () => {
    setDiscovering(true); setFound([]); setNote("");
    try {
      const data = await providerRequest<{ details: { id: string; context_window?: number }[] }>("/api/providers/models", { name });
      setFound(data.details);
      if (!data.details.length) setNote("This provider reported no models. Enter a model ID below.");
    } catch (err) { setNote(err instanceof Error ? err.message : "Could not list models. Enter a model ID below."); }
    finally { setDiscovering(false); }
  };
  const add = async (event: React.FormEvent) => {
    event.preventDefault();
    const provider = providers.find(p => p.name === name);
    if (!provider || !model.trim()) return;
    setBusy(true); setError("");
    try {
      await providerRequest("/api/models/add", { args: [
        `${provider.name}:${model.trim()}`, provider.endpoint, model.trim(), "--registration", provider.name,
      ] });
      onDone(`Added ${model.trim()}`, true);
    } catch (err) { setError(err instanceof Error ? err.message : "Could not add model"); }
    finally { setBusy(false); }
  };
  return <form onSubmit={add} style={{ display: "grid", gap: 12, maxWidth: 640, marginTop: 12 }}>
    {loading ? <p>Loading providers…</p> : !providers.length && !error ? <p>Add a connection on the Providers page first.</p> : null}
    {!!providers.length && <>
      <label style={{ display: "grid", gap: 4 }}>Provider <select style={{ padding: "6px 8px", width: "100%" }} value={name} onChange={e => { setName(e.target.value); setFound([]); setNote(""); }} disabled={busy || discovering}>
        {providers.map(p => <option key={p.name} value={p.name}>{p.name}</option>)}
      </select></label>
      <Button style={{ justifySelf: "start" }} type="button" onClick={() => void discover()} disabled={busy || discovering}>{discovering ? "Loading models…" : "Show models this provider offers"}</Button>
      {note && <p>{note}</p>}
      {found.length > 0 && <div style={{ maxHeight: 200, overflow: "auto" }}>{found.map(m => <div key={m.id}>
        <Button type="button" disabled={busy} onClick={() => setModel(m.id)}>{m.id}</Button>{" "}
        {m.context_window ? `${m.context_window.toLocaleString()} context` : "context not published"}
      </div>)}</div>}
      <label style={{ display: "grid", gap: 4 }}>Model ID <input style={{ padding: "6px 8px", width: "100%", boxSizing: "border-box" }} required value={model} onChange={e => setModel(e.target.value)} disabled={busy} /></label>
      <p>Uses this provider’s saved connection and credentials. Set limits, prices, and routing preferences after adding the model.</p>
      <Button style={{ justifySelf: "start" }} type="submit" disabled={busy}>{busy ? "Adding…" : "Add model"}</Button>
    </>}
    {error && <InlineStatus status={{ kind: "err", msg: error }} />}
  </form>;
}
