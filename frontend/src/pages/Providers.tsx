/* Providers — the operator-facing view of "where models come from".
 *
 * The Models tab lists RUNTIME TARGETS: one row per (endpoint, model) pair. That
 * is what routing schedules, but it is not how anyone thinks about their setup.
 * An operator has a handful of providers and picks models from each, so this
 * page groups the same records by provider and nests its models underneath.
 * Adding a second model to a provider reuses that provider's endpoint and
 * credentials instead of asking for them again.
 *
 * WHY EVERY NUMBER SHOWS ITS SOURCE. A context window someone typed and one a
 * catalog guessed render identically once they are just a number, and that is
 * exactly how a two-month-stale figure sat unquestioned while it understated
 * every current model fivefold. Each value here is badged declared / resolved /
 * unknown, and "unknown" is shown rather than hidden behind a plausible zero.
 */
import { useCallback, useEffect, useMemo, useState } from "react";
import {
  Panel,
  Badge,
  Spinner,
  Modal,
  InlineStatus,
  EmptyState,
  Button,
} from "@rakuensoftware/smoothgui";

/* One runtime target, as GET /api/models reports it (server_agent_to_json). */
interface ModelCfg {
  name: string;
  endpoint: string;
  model: string;
  provider: string;
  catalog_provider?: string;
  model_display_name?: string;
  enabled: boolean;
  context_window?: number;
  max_output?: number;
  /* What the fleet actually uses, and where it came from. Always present, and
   * 0 is meaningful here: it is "nobody knows", not "no capacity". */
  effective_context_window?: number;
  effective_max_output?: number;
  context_window_source?: "declared" | "resolved" | "unknown";
  max_output_source?: "declared" | "resolved" | "unknown";
  /* A price of 0 is only legible with its declared flag: declared 0 means a
   * free or subscription seat, undeclared 0 means nobody said. */
  price_in_per_mtok?: number;
  price_out_per_mtok?: number;
  price_cached_per_mtok?: number;
  price_in_declared?: boolean;
  price_out_declared?: boolean;
  price_cached_declared?: boolean;
}

/* One model as the PROVIDER describes it (POST /api/providers/models).
 * Fields are absent when the provider did not publish them -- most endpoints
 * return ids and nothing more -- so absent must read as "not published", never
 * as zero. */
interface DiscoveredModel {
  id: string;
  display_name?: string;
  context_window?: number;
  max_output?: number;
  deprecated?: boolean;
}

interface ProviderGroup {
  provider: string;
  endpoint: string;
  models: ModelCfg[];
}

async function getJSON<T>(url: string): Promise<T> {
  const r = await fetch(url, { headers: { "X-CSRF-Token": window._csrf || "" } });
  return (await r.json()) as T;
}

async function postArgs<T>(url: string, args: string[]): Promise<T> {
  const r = await fetch(url, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "X-CSRF-Token": window._csrf || "",
    },
    body: JSON.stringify({ args }),
  });
  return (await r.json()) as T;
}

const inp: React.CSSProperties = {
  width: "100%",
  padding: "6px 8px",
  border: "1px solid #ccd",
  borderRadius: 4,
  fontSize: 13,
};

/* Group runtime targets into providers. Keyed on (provider, endpoint) rather
 * than provider alone: the same vendor can legitimately be configured twice --
 * a direct account and a gateway that resells it -- and collapsing those would
 * put one provider's models under the other's credentials. */
export function groupByProvider(agents: ModelCfg[]): ProviderGroup[] {
  const byKey = new Map<string, ProviderGroup>();
  for (const a of agents) {
    const key = `${a.provider} ${a.endpoint}`;
    let g = byKey.get(key);
    if (!g) {
      g = { provider: a.provider, endpoint: a.endpoint, models: [] };
      byKey.set(key, g);
    }
    g.models.push(a);
  }
  const groups = [...byKey.values()];
  groups.sort((x, y) => x.provider.localeCompare(y.provider) || x.endpoint.localeCompare(y.endpoint));
  for (const g of groups) g.models.sort((x, y) => x.model.localeCompare(y.model));
  return groups;
}

/* A number plus where it came from. `undefined` and 0 both mean unknown here,
 * and unknown is rendered as a word rather than as "0" -- a zero that looks like
 * a measurement is the failure this page exists to prevent. */
function ValueWithSource({
  value,
  source,
  suffix,
}: {
  value?: number;
  source?: "declared" | "resolved" | "unknown";
  suffix?: string;
}) {
  const known = typeof value === "number" && value > 0;
  if (!known) {
    return (
      <span title="Neither you nor the provider has supplied this value. Set it below.">
        <Badge label="unknown" variant="warning" />
      </span>
    );
  }
  const label = source === "declared" ? "you set this" : "from provider/catalog";
  return (
    <span style={{ display: "inline-flex", gap: 6, alignItems: "center" }}>
      <span style={{ fontFamily: "monospace" }}>
        {value.toLocaleString()}
        {suffix || ""}
      </span>
      <span title={label}>
        <Badge
          label={source === "declared" ? "declared" : "resolved"}
          variant={source === "declared" ? "success" : "neutral"}
        />
      </span>
    </span>
  );
}

function priceText(value?: number, declared?: boolean): string {
  if (declared && (value ?? 0) === 0) return "free (declared)";
  if (typeof value === "number" && value > 0) return `$${value}/Mtok`;
  return "unknown";
}

/* Per-model editor. Everything here is a DECLARATION: naming a field states a
 * value, and clearing it withdraws the statement. */
function ModelEditor({
  agent,
  onClose,
  onSaved,
  onStatus,
}: {
  agent: ModelCfg;
  onClose: () => void;
  onSaved: () => void;
  onStatus: (msg: string, ok: boolean) => void;
}) {
  const [ctx, setCtx] = useState(String(agent.context_window || ""));
  const [maxOut, setMaxOut] = useState(String(agent.max_output || ""));
  const [priceIn, setPriceIn] = useState(
    agent.price_in_declared ? String(agent.price_in_per_mtok ?? 0) : "",
  );
  const [priceOut, setPriceOut] = useState(
    agent.price_out_declared ? String(agent.price_out_per_mtok ?? 0) : "",
  );
  const [priceCached, setPriceCached] = useState(
    agent.price_cached_declared ? String(agent.price_cached_per_mtok ?? 0) : "",
  );
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState("");

  const save = async () => {
    setBusy(true);
    setErr("");
    /* agent.set describes the whole desired state, so an omitted option
     * withdraws that declaration. A price is only sent when the operator typed
     * something -- "" means "I am not stating this", while "0" is the positive
     * statement that the seat is free. */
    const args = [
      agent.name,
      "--provider", agent.provider,
      "--model", agent.model,
      "--endpoint", agent.endpoint,
      "--context-window", ctx.trim() || "0",
      "--max-output", maxOut.trim() || "0",
    ];
    /* Always sent, empty included. agent.set is a PATCH: an omitted option
     * changes nothing, so omitting a cleared field would silently keep the old
     * price. An empty value is the explicit withdrawal of the declaration. */
    args.push("--price-in", priceIn.trim());
    args.push("--price-out", priceOut.trim());
    args.push("--price-cached", priceCached.trim());
    try {
      const res = await postArgs<{ error?: string }>("/api/models/set", args);
      if (res.error) setErr(res.error);
      else {
        onStatus(`${agent.model} saved`, true);
        onSaved();
        onClose();
      }
    } catch {
      setErr("save failed");
    } finally {
      setBusy(false);
    }
  };

  const L = ({ label, hint, children }: { label: string; hint: string; children: React.ReactNode }) => (
    <label style={{ display: "block", marginBottom: 10 }} title={hint}>
      <div style={{ fontSize: 12, color: "#556", marginBottom: 3 }}>{label}</div>
      {children}
    </label>
  );

  return (
    <Modal open onClose={onClose} title="Model settings" headerExtra={
      <span style={{ fontSize: 13, color: "#667", fontFamily: "monospace" }}>{agent.model}</span>
    } size="lg">
      <p style={{ fontSize: 12, color: "#667", marginTop: 0 }}>
        Leave a field empty to say nothing about it — the provider's own figure is used where it
        publishes one. A price of <code>0</code> is a statement that this seat costs nothing per
        token, which is different from leaving it empty.
      </p>
      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 10 }}>
        <L label="context window (tokens)" hint="Total prompt + completion the model accepts. Empty = use the provider's figure.">
          <input value={ctx} onChange={(e) => setCtx(e.target.value)} style={inp} disabled={busy}
                 inputMode="numeric" placeholder="from provider" />
        </L>
        <L label="max output (tokens)" hint="Most the model will emit in one reply. Must not exceed the context window.">
          <input value={maxOut} onChange={(e) => setMaxOut(e.target.value)} style={inp} disabled={busy}
                 inputMode="numeric" placeholder="from provider" />
        </L>
        <L label="input price ($/Mtok)" hint="What you pay per million input tokens. 0 = free or covered by a subscription.">
          <input value={priceIn} onChange={(e) => setPriceIn(e.target.value)} style={inp} disabled={busy}
                 inputMode="decimal" placeholder="not stated" />
        </L>
        <L label="output price ($/Mtok)" hint="What you pay per million output tokens. 0 = free or covered by a subscription.">
          <input value={priceOut} onChange={(e) => setPriceOut(e.target.value)} style={inp} disabled={busy}
                 inputMode="decimal" placeholder="not stated" />
        </L>
        <L label="cached-read price ($/Mtok)" hint="Price for cache reads, usually far below the input rate.">
          <input value={priceCached} onChange={(e) => setPriceCached(e.target.value)} style={inp} disabled={busy}
                 inputMode="decimal" placeholder="not stated" />
        </L>
      </div>
      {err && <InlineStatus status={{ kind: "err", msg: err }} />}
      <div style={{ display: "flex", gap: 8, marginTop: 12 }}>
        <Button onClick={save} disabled={busy} size="md" title="Save these values for this model.">
          {busy ? "Saving…" : "Save"}
        </Button>
        <Button onClick={onClose} disabled={busy} size="md" title="Discard and close.">
          Cancel
        </Button>
      </div>
    </Modal>
  );
}

/* Add another model to a provider that is already configured. The endpoint and
 * credentials come from the provider, which is the whole point of grouping:
 * a second model should not mean re-entering an API key. */
function AddModel({
  group,
  onClose,
  onSaved,
  onStatus,
}: {
  group: ProviderGroup;
  onClose: () => void;
  onSaved: () => void;
  onStatus: (msg: string, ok: boolean) => void;
}) {
  const [model, setModel] = useState("");
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState("");
  const [found, setFound] = useState<DiscoveredModel[] | null>(null);
  const [discovering, setDiscovering] = useState(false);
  const [discoverNote, setDiscoverNote] = useState("");

  /* Ask the provider what it offers. Many cannot answer -- one configured
   * endpoint 404s, another omits its own configured model -- so a failure is
   * reported as a plain note and the operator types the id instead. It is never
   * turned into an empty list, which would read as "this provider has no
   * models". */
  const discover = async () => {
    setDiscovering(true);
    setDiscoverNote("");
    setFound(null);
    try {
      const r = await fetch("/api/providers/models", {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-CSRF-Token": window._csrf || "" },
        body: JSON.stringify({ name: group.provider }),
      });
      const d = (await r.json()) as { details?: DiscoveredModel[]; models?: string[]; error?: string };
      if (d.error) {
        setDiscoverNote(`${group.provider} could not list its models (${d.error}). Type the id below.`);
      } else {
        const list = d.details?.length ? d.details : (d.models || []).map((id) => ({ id }));
        setFound(list);
        if (!list.length) setDiscoverNote("This provider reported no models. Type the id below.");
      }
    } catch {
      setDiscoverNote("Could not reach the provider. Type the id below.");
    } finally {
      setDiscovering(false);
    }
  };

  const add = async () => {
    const id = model.trim();
    if (!id) {
      setErr("model id is required");
      return;
    }
    setBusy(true);
    setErr("");
    /* Named "<provider>:<model>" to match the canonical target form the server
     * already uses when it expands a provider registration into one target per
     * model, so a GUI-created target and a config-created one are the same shape. */
    const name = `${group.provider}:${id}`;
    try {
      const res = await postArgs<{ error?: string }>("/api/models/add", [
        name, group.endpoint, id, "--provider", group.provider,
      ]);
      if (res.error) setErr(res.error);
      else {
        onStatus(`${id} added to ${group.provider}`, true);
        onSaved();
        onClose();
      }
    } catch {
      setErr("add failed");
    } finally {
      setBusy(false);
    }
  };

  return (
    <Modal open onClose={onClose} title={`Add a model to ${group.provider}`} size="md">
      <p style={{ fontSize: 12, color: "#667", marginTop: 0 }}>
        Uses this provider's existing endpoint and credentials. You can set its limits and prices
        afterwards.
      </p>
      <div style={{ marginBottom: 10 }}>
        <Button onClick={discover} disabled={busy || discovering} size="md"
                title="Ask this provider which models it offers.">
          {discovering ? "Asking…" : "Show models this provider offers"}
        </Button>
        {discoverNote && (
          <div style={{ fontSize: 12, color: "#775", marginTop: 6 }}>{discoverNote}</div>
        )}
        {found && found.length > 0 && (
          <div style={{ maxHeight: 190, overflowY: "auto", marginTop: 8, border: "1px solid #eef", borderRadius: 4 }}>
            {found.map((f) => (
              <div key={f.id}
                   style={{ display: "flex", justifyContent: "space-between", alignItems: "center",
                            padding: "5px 8px", borderBottom: "1px solid #f4f4fa" }}>
                <div>
                  <div style={{ fontFamily: "monospace", fontSize: 12 }}>{f.id}</div>
                  <div style={{ fontSize: 11, color: "#889" }}>
                    {f.display_name ? `${f.display_name} · ` : ""}
                    {/* Absent means the provider did not publish it, which the
                        operator needs to know: they will have to state it. */}
                    {f.context_window ? `${f.context_window.toLocaleString()} ctx` : "context not published"}
                    {f.deprecated ? " · deprecated" : ""}
                  </div>
                </div>
                <Button onClick={() => setModel(f.id)} size="sm" title="Use this model id.">
                  Use
                </Button>
              </div>
            ))}
          </div>
        )}
      </div>
      <label style={{ display: "block", marginBottom: 10 }}>
        <div style={{ fontSize: 12, color: "#556", marginBottom: 3 }}>model id</div>
        <input value={model} onChange={(e) => setModel(e.target.value)} style={inp} disabled={busy}
               placeholder="e.g. claude-sonnet-5" autoFocus />
      </label>
      {err && <InlineStatus status={{ kind: "err", msg: err }} />}
      <div style={{ display: "flex", gap: 8, marginTop: 12 }}>
        <Button onClick={add} disabled={busy} size="md" title="Add this model under the provider.">
          {busy ? "Adding…" : "Add model"}
        </Button>
        <Button onClick={onClose} disabled={busy} size="md" title="Close without adding.">
          Cancel
        </Button>
      </div>
    </Modal>
  );
}

export default function Providers() {
  const [agents, setAgents] = useState<ModelCfg[]>([]);
  const [loading, setLoading] = useState(false);
  const [status, setStatus] = useState<{ kind: "ok" | "err"; msg: string } | null>(null);
  const [editing, setEditing] = useState<ModelCfg | null>(null);
  const [adding, setAdding] = useState<ProviderGroup | null>(null);

  const refresh = useCallback(() => {
    setLoading(true);
    getJSON<{ models?: ModelCfg[]; agents?: ModelCfg[] }>("/api/models")
      .then((d) => setAgents(d.models || d.agents || []))
      .catch(() => setAgents([]))
      .finally(() => setLoading(false));
  }, []);

  useEffect(refresh, [refresh]);

  const groups = useMemo(() => groupByProvider(agents), [agents]);
  const onStatus = (msg: string, ok: boolean) => setStatus({ kind: ok ? "ok" : "err", msg });

  return (
    <div>
      <div style={{ display: "flex", gap: 8, alignItems: "center", marginBottom: 12 }}>
        <h2 style={{ margin: 0 }}>Providers</h2>
        <Badge label={`${groups.length}`} variant="neutral" />
        <Button onClick={refresh} size="md" title="Reload providers and their models.">
          Refresh
        </Button>
        <Spinner loading={loading} />
        <InlineStatus status={status} />
      </div>

      {!loading && !groups.length && (
        <EmptyState message="No providers configured yet — add one from the Models tab." />
      )}

      {groups.map((g) => (
        <Panel key={`${g.provider} ${g.endpoint}`} title={g.provider}>
          <div style={{ fontSize: 12, color: "#667", fontFamily: "monospace", marginBottom: 8 }}>
            {g.endpoint || "(no endpoint — CLI seat)"}
          </div>
          <table style={{ width: "100%", borderCollapse: "collapse", fontSize: 13 }}>
            <thead>
              <tr style={{ textAlign: "left", color: "#556" }}>
                <th style={{ padding: "4px 6px" }}>model</th>
                <th style={{ padding: "4px 6px" }}>context window</th>
                <th style={{ padding: "4px 6px" }}>max output</th>
                <th style={{ padding: "4px 6px" }}>in / out price</th>
                <th style={{ padding: "4px 6px" }} />
              </tr>
            </thead>
            <tbody>
              {g.models.map((m) => (
                <tr key={m.name} style={{ borderTop: "1px solid #eef" }}>
                  <td style={{ padding: "6px" }}>
                    <div style={{ fontFamily: "monospace" }}>{m.model || "(none)"}</div>
                    {m.model_display_name && (
                      <div style={{ fontSize: 11, color: "#889" }}>{m.model_display_name}</div>
                    )}
                    {!m.enabled && <Badge label="disabled" variant="warning" />}
                  </td>
                  <td style={{ padding: "6px" }}>
                    <ValueWithSource value={m.effective_context_window}
                                     source={m.context_window_source} />
                  </td>
                  <td style={{ padding: "6px" }}>
                    <ValueWithSource value={m.effective_max_output}
                                     source={m.max_output_source} />
                  </td>
                  <td style={{ padding: "6px", fontFamily: "monospace", fontSize: 12 }}>
                    {priceText(m.price_in_per_mtok, m.price_in_declared)}
                    {" / "}
                    {priceText(m.price_out_per_mtok, m.price_out_declared)}
                  </td>
                  <td style={{ padding: "6px", textAlign: "right" }}>
                    <Button onClick={() => setEditing(m)} size="sm"
                            title="Set this model's limits and prices.">
                      Settings
                    </Button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
          <div style={{ marginTop: 10 }}>
            <Button onClick={() => setAdding(g)} size="md"
                    title="Add another model using this provider's endpoint and credentials.">
              Add model
            </Button>
          </div>
        </Panel>
      ))}

      {editing && (
        <ModelEditor agent={editing} onClose={() => setEditing(null)} onSaved={refresh}
                     onStatus={onStatus} />
      )}
      {adding && (
        <AddModel group={adding} onClose={() => setAdding(null)} onSaved={refresh}
                  onStatus={onStatus} />
      )}
    </div>
  );
}
