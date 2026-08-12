/* Models — the operator-facing list of RUNTIME TARGETS: one row per
 * (endpoint, model) pair, which is exactly what routing schedules.
 *
 * This is the flat, per-target view. The Providers tab groups these same records
 * by provider and nests its models underneath, so credentials and endpoints are
 * entered once per provider rather than once per model. Both pages read the same
 * roster; this one is where a single model's routing knobs and its role/persona
 * bindings are edited.
 */
import { useEffect, useState, useCallback, useMemo } from "react";
import { Panel, Badge, Spinner, Modal, InlineStatus, EmptyState, KeyValue, Button } from "@rakuensoftware/smoothgui";
import PrimaryChooser from "../setup/PrimaryChooser";

/* ---- API types ---- */

// GET /api/models -> one entry per configured model (server_agent_to_json).
interface ModelCfg {
  name: string;
  endpoint: string;
  model: string;
  auth_type?: string;
  provider: string;
  cost_tier?: number;
  enabled: boolean;
  tools_enabled?: boolean;
  primary_only?: boolean;
  max_turns?: number;
  max_parallel?: number;
  context_window?: number;
  roles?: string[];
  personas?: string[];
}

// GET /api/models/stats -> per-model run stats (agent_log JOIN token_audit).
interface ModelStats {
  name: string;
  total_calls: number;
  successful_calls: number;
  failed_calls: number;
  success_rate: number; // 0..1
  avg_latency_ms: number;
  prompt_tokens: number;
  completion_tokens: number;
  cache_write_tokens: number;
  cache_read_tokens: number;
  estimated_cost_usd: number;
}

// POST /api/models/probe -> live reachability of one model.
interface ProbeResult {
  name: string;
  execution_ok?: boolean;
  model_available?: boolean;
  latency_ms?: number;
  execution_message?: string;
  error?: string;
}

type ProbeState = {
  status: "idle" | "probing" | "ok" | "down";
  latency_ms?: number;
  msg?: string;
};

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

const PROVIDERS = [
  "openai",
  "anthropic",
  "chatgpt",
  "claude",
  "claude-code",
  "gemini",
  "mistral",
];

export default function Models() {
  const [models, setModels] = useState<ModelCfg[]>([]);
  const [stats, setStats] = useState<Record<string, ModelStats>>({});
  const [probes, setProbes] = useState<Record<string, ProbeState>>({});
  const [loading, setLoading] = useState(false);
  const [status, setStatus] = useState<{ kind: "ok" | "err"; msg: string } | null>(
    null,
  );
  const [showAdd, setShowAdd] = useState(false);
  // The model currently open in the Edit modal (null = closed). ALL mutations —
  // config fields, roles/personas binding, enable, remove — happen there; the base
  // list is read-only so a binding can never change from a stray click.
  const [editing, setEditing] = useState<ModelCfg | null>(null);
  // The known role + persona vocabularies, offered (with "all") when assigning
  // a model's roles/personas so they match what personas declare.
  const [knownRoles, setKnownRoles] = useState<string[]>([]);
  const [knownPersonas, setKnownPersonas] = useState<string[]>([]);

  const refresh = useCallback(() => {
    setLoading(true);
    getJSON<{ role_templates?: string[] }>("/api/roles")
      .then((d) => setKnownRoles((d.role_templates || []).slice().sort()))
      .catch(() => setKnownRoles([]));
    getJSON<{ personas?: { name: string }[] }>("/api/chat/personas")
      .then((d) => setKnownPersonas((d.personas || []).map((p) => p.name)))
      .catch(() => setKnownPersonas([]));
    Promise.all([
      // `agents` is the pre-rename key; read either so this page also works
      // against a server that has not been upgraded yet.
      getJSON<{ models?: ModelCfg[]; agents?: ModelCfg[] }>("/api/models")
        .then((d) => setModels(d.models || d.agents || []))
        .catch(() => setModels([])),
      getJSON<{ stats: ModelStats[] }>("/api/models/stats")
        .then((d) => {
          const map: Record<string, ModelStats> = {};
          for (const s of d.stats || []) map[s.name] = s;
          setStats(map);
        })
        .catch(() => setStats({})),
    ]).finally(() => setLoading(false));
  }, []);

  useEffect(() => {
    refresh();
  }, [refresh]);

  const probe = useCallback(async (name: string) => {
    setProbes((p) => ({ ...p, [name]: { status: "probing" } }));
    try {
      const res = await postArgs<ProbeResult>("/api/models/probe", [name]);
      if (res.error) {
        setProbes((p) => ({ ...p, [name]: { status: "down", msg: res.error } }));
        return;
      }
      // A live "Respond with ok." run is the strongest availability signal;
      // fall back to /models reachability when the run was skipped.
      const up = res.execution_ok ?? res.model_available ?? false;
      setProbes((p) => ({
        ...p,
        [name]: {
          status: up ? "ok" : "down",
          latency_ms: res.latency_ms,
          msg: res.execution_message,
        },
      }));
    } catch {
      setProbes((p) => ({ ...p, [name]: { status: "down", msg: "probe failed" } }));
    }
  }, []);

  const probeAll = useCallback(() => {
    for (const m of models) void probe(m.name);
  }, [models, probe]);

  // Keep the modal's view of the model in sync after a save-triggered refresh so
  // it reflects the persisted record (and closes cleanly if the model was removed).
  useEffect(() => {
    if (!editing) return;
    const fresh = models.find((m) => m.name === editing.name);
    if (fresh && fresh !== editing) setEditing(fresh);
  }, [models, editing]);

  return (
    <div style={{ padding: 16, fontFamily: "system-ui", height: "100%", overflow: "auto" }}>
      <div style={{ display: "flex", alignItems: "center", gap: 10, marginBottom: 12 }}>
        <strong style={{ fontSize: 18 }}>Models</strong>
        <Badge label={`${models.length}`} variant="neutral" />
        <Button onClick={refresh} size="md" title="Reload the model list and run stats.">
          Refresh
        </Button>
        <Button onClick={probeAll} size="md" disabled={!models.length} title="Test live reachability of every configured model.">
          Probe all
        </Button>
        <Button
          variant="primary"
          size="md"
          onClick={() => setShowAdd((v) => !v)}
          title="Show or hide the form for adding a new model."
        >
          {showAdd ? "Close" : "+ Add model"}
        </Button>
        <Spinner loading={loading} text="loading…" />
        <InlineStatus status={status} />
      </div>

      {showAdd && (
        <AddModel
          onDone={(msg, ok) => {
            setStatus({ kind: ok ? "ok" : "err", msg });
            if (ok) {
              setShowAdd(false);
              refresh();
            }
          }}
        />
      )}

      <div style={{ display: "grid", gap: 10, marginTop: 12 }}>
        {models.length === 0 && !loading && (
          <div style={{ color: "#888", fontSize: 14 }}>
            No models configured. Add one to get started.
          </div>
        )}
        {models.map((m) => (
          <ModelCard
            key={m.name}
            cfg={m}
            stats={stats[m.name]}
            probe={probes[m.name]}
            onProbe={() => probe(m.name)}
            onEdit={() => setEditing(m)}
          />
        ))}
      </div>

      {editing && (
        <ModelEditModal
          cfg={editing}
          knownRoles={knownRoles}
          knownPersonas={knownPersonas}
          onClose={() => setEditing(null)}
          onSaved={refresh}
          onStatus={(msg, ok) => setStatus({ kind: ok ? "ok" : "err", msg })}
        />
      )}
    </div>
  );
}

/* ---- one model: READ-ONLY overview (config + availability + stats). All
   mutations live in the Edit modal, so no binding can change from this list. ---- */

function ModelCard({
  cfg,
  stats,
  probe,
  onProbe,
  onEdit,
}: {
  cfg: ModelCfg;
  stats?: ModelStats;
  probe?: ProbeState;
  onProbe: () => void;
  onEdit: () => void;
}) {
  const pstate = probe?.status || "idle";
  const dot =
    pstate === "ok"
      ? { c: "#22a06b", t: "available" }
      : pstate === "down"
        ? { c: "#d4564f", t: "unavailable" }
        : pstate === "probing"
          ? { c: "#e0a800", t: "probing…" }
          : { c: "#bbb", t: "unknown" };

  const successPct =
    stats && stats.total_calls > 0
      ? `${Math.round(stats.success_rate * 100)}%`
      : "—";

  return (
    <Panel title={cfg.name}>
      <div style={{ display: "flex", gap: 16, flexWrap: "wrap", alignItems: "flex-start" }}>
        {/* left: configuration (read-only) */}
        <div style={{ flex: "1 1 260px", minWidth: 240 }}>
          <div style={{ display: "flex", alignItems: "center", gap: 6, margin: "2px 0 4px" }}>
            <Badge
              label={cfg.enabled ? "enabled" : "disabled"}
              variant={cfg.enabled ? "success" : "neutral"}
            />
          </div>
          <KeyValue label="provider" value={cfg.provider || "—"} />
          <KeyValue label="model" value={cfg.model || "—"} />
          <KeyValue label="endpoint" value={cfg.endpoint || "(cli / none)"} mono />
          {typeof cfg.cost_tier === "number" && (
            <KeyValue label="cost tier" value={String(cfg.cost_tier)} />
          )}
          {typeof cfg.max_parallel === "number" && cfg.max_parallel > 0 && (
            <KeyValue label="max parallel" value={String(cfg.max_parallel)} />
          )}
          {typeof cfg.max_turns === "number" && cfg.max_turns >= 0 && (
            <KeyValue label="max turns" value={String(cfg.max_turns)} />
          )}
          {cfg.context_window ? (
            <KeyValue label="context" value={`${cfg.context_window.toLocaleString()} tok`} />
          ) : null}
          <KeyValue label="tools" value={cfg.tools_enabled ? "enabled" : "disabled"} />
          <StaticChips label="roles" values={cfg.roles || []} />
          <StaticChips label="personas" values={cfg.personas || []} emptyHint="(none = all)" />
        </div>

        {/* middle: availability */}
        <div style={{ flex: "0 0 180px" }}>
          <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
            <span
              style={{
                width: 9,
                height: 9,
                borderRadius: "50%",
                background: dot.c,
                display: "inline-block",
              }}
            />
            <span style={{ fontSize: 13, color: "#444" }}>{dot.t}</span>
          </div>
          {probe?.latency_ms != null && pstate === "ok" && (
            <div style={{ fontSize: 12, color: "#888", marginTop: 2 }}>
              {probe.latency_ms} ms
            </div>
          )}
          {probe?.msg && pstate === "down" && (
            <div
              style={{ fontSize: 11, color: "#c66", marginTop: 2, wordBreak: "break-word" }}
              title={probe.msg}
            >
              {probe.msg.slice(0, 80)}
            </div>
          )}
          <Button
            size="sm"
            onClick={onProbe}
            style={{ marginTop: 6 }}
            disabled={pstate === "probing"}
            title="Test whether this model is reachable right now."
          >
            {pstate === "probing" ? "probing…" : "Probe"}
          </Button>
        </div>

        {/* right: run stats */}
        <div style={{ flex: "1 1 220px", minWidth: 200 }}>
          <div style={{ fontSize: 12, color: "#999", marginBottom: 2 }}>run stats</div>
          {stats && stats.total_calls > 0 ? (
            <>
              <KeyValue label="runs" value={String(stats.total_calls)} />
              <KeyValue
                label="ok / failed"
                value={`${stats.successful_calls} / ${stats.failed_calls}`}
              />
              <KeyValue label="success" value={successPct} />
              <KeyValue label="avg latency" value={`${stats.avg_latency_ms} ms`} />
              <KeyValue
                label="tokens (in/out)"
                value={`${fmt(stats.prompt_tokens)} / ${fmt(stats.completion_tokens)}`}
              />
              {stats.estimated_cost_usd > 0 && (
                <KeyValue label="est. cost" value={`$${stats.estimated_cost_usd.toFixed(4)}`} />
              )}
            </>
          ) : (
            <EmptyState message="no runs recorded yet" inline />
          )}
        </div>
      </div>

      <div style={{ marginTop: 8, borderTop: "1px solid #eee", paddingTop: 8 }}>
        <Button
          variant="primary"
          size="sm"
          onClick={onEdit}
          title="Open the editor to change this model's config and role/persona bindings."
        >
          Edit
        </Button>
      </div>
    </Panel>
  );
}

/* ---- per-model Edit modal: edit ALL aspects; the one place binding changes are
   made. Saves via a single surgical POST /api/models/set. ---- */

function ModelEditModal({
  cfg,
  knownRoles,
  knownPersonas,
  onClose,
  onSaved,
  onStatus,
}: {
  cfg: ModelCfg;
  knownRoles: string[];
  knownPersonas: string[];
  onClose: () => void;
  onSaved: () => void;
  onStatus: (msg: string, ok: boolean) => void;
}) {
  const [provider, setProvider] = useState(cfg.provider || "openai");
  const [model, setModel] = useState(cfg.model || "");
  const [endpoint, setEndpoint] = useState(cfg.endpoint || "");
  const [costTier, setCostTier] = useState(String(cfg.cost_tier ?? 0));
  const [maxTurns, setMaxTurns] = useState(String(cfg.max_turns ?? -1));
  const [maxParallel, setMaxParallel] = useState(String(cfg.max_parallel ?? 0));
  const [contextWindow, setContextWindow] = useState(String(cfg.context_window ?? 0));
  const [tools, setTools] = useState(!!cfg.tools_enabled);
  const [enabled, setEnabled] = useState(!!cfg.enabled);
  const [primaryOnly, setPrimaryOnly] = useState(!!cfg.primary_only);
  const [roles, setRoles] = useState<string[]>(cfg.roles || []);
  const [personas, setPersonas] = useState<string[]>(cfg.personas || []);
  const [apiKey, setApiKey] = useState("");
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState("");

  const cliProvider = provider === "claude" || provider === "claude-code";

  const save = async () => {
    setBusy(true);
    setErr("");
    // One surgical set carrying every editable field. The server patches only what
    // is passed; an empty role selection remains empty, while personas empty resets
    // to "all".
    const args = [
      cfg.name,
      "--provider", provider,
      "--model", model.trim(),
      "--endpoint", endpoint.trim(),
      "--cost-tier", costTier || "0",
      "--max-turns", maxTurns || "-1",
      "--max-parallel", maxParallel || "0",
      "--context-window", contextWindow || "0",
      "--tools", tools ? "on" : "off",
      "--enabled", enabled ? "true" : "false",
      "--primary-only", primaryOnly ? "on" : "off",
      "--roles", roles.join(","),
      "--personas", personas.join(","),
    ];
    if (apiKey.trim()) args.push("--key", apiKey.trim());
    try {
      const res = await postArgs<{ error?: string }>("/api/models/set", args);
      if (res.error) setErr(res.error);
      else {
        onStatus(`${cfg.name} saved`, true);
        onSaved();
        onClose();
      }
    } catch {
      setErr("save failed");
    } finally {
      setBusy(false);
    }
  };

  const remove = async () => {
    if (!confirm(`Remove model “${cfg.name}”? This edits the model roster on disk.`)) return;
    setBusy(true);
    setErr("");
    try {
      const res = await postArgs<{ error?: string }>("/api/models/remove", [cfg.name]);
      if (res.error) setErr(res.error);
      else {
        onStatus(`removed ${cfg.name}`, true);
        onSaved();
        onClose();
      }
    } catch {
      setErr("remove failed");
    } finally {
      setBusy(false);
    }
  };

  return (
    <Modal
      open
      onClose={onClose}
      title="Edit model"
      headerExtra={<span style={{ fontSize: 13, color: "#667", fontFamily: "monospace" }}>{cfg.name}</span>}
      size="lg"
    >
      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 10 }}>
        <L label="provider" title="The backend provider used to run this model.">
          <select value={provider} onChange={(e) => setProvider(e.target.value)} style={inp} disabled={busy}>
            {(PROVIDERS.includes(provider) ? PROVIDERS : [provider, ...PROVIDERS]).map((p) => (
              <option key={p} value={p}>
                {p}
              </option>
            ))}
          </select>
        </L>
        <L label="model" title="The model identifier this entry calls.">
          <input value={model} onChange={(e) => setModel(e.target.value)} style={inp} disabled={busy} />
        </L>
        <L
          label={cliProvider ? "endpoint (optional for CLI)" : "endpoint"}
          title="API base URL for this model; optional for CLI providers."
        >
          <input
            value={endpoint}
            onChange={(e) => setEndpoint(e.target.value)}
            style={inp}
            placeholder="https://host:port/v1"
            disabled={busy}
          />
        </L>
        <L label="cost tier" title="Relative cost tier used when routing picks a model.">
          <input type="number" value={costTier} onChange={(e) => setCostTier(e.target.value)} style={inp} min={0} disabled={busy} />
        </L>
        <L label="max turns (-1 = default)" title="Cap on turns per run for this model; -1 uses the default.">
          <input type="number" value={maxTurns} onChange={(e) => setMaxTurns(e.target.value)} style={inp} disabled={busy} />
        </L>
        <L label="max parallel" title="Maximum number of concurrent runs of this model.">
          <input type="number" value={maxParallel} onChange={(e) => setMaxParallel(e.target.value)} style={inp} min={0} disabled={busy} />
        </L>
        <L label="context window (tok, 0 = auto)" title="Context window in tokens; 0 auto-detects.">
          <input type="number" value={contextWindow} onChange={(e) => setContextWindow(e.target.value)} style={inp} min={0} disabled={busy} />
        </L>
        <L label="API key (blank = keep current)" title="Set an API key or $ENV_VAR reference; leave blank to keep the current key.">
          <input
            type="password"
            value={apiKey}
            onChange={(e) => setApiKey(e.target.value)}
            style={inp}
            placeholder="sk-…  or  $ENV_VAR"
            disabled={busy}
          />
        </L>
      </div>

      <div style={{ display: "flex", gap: 20, marginTop: 10 }}>
        <label
          style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 13, color: "#444" }}
          title="Whether this model is available for routing."
        >
          <input type="checkbox" checked={enabled} onChange={(e) => setEnabled(e.target.checked)} disabled={busy} />
          enabled
        </label>
        <label
          style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 13, color: "#444" }}
          title="Whether this model is allowed to use tools."
        >
          <input type="checkbox" checked={tools} onChange={(e) => setTools(e.target.checked)} disabled={busy} />
          tools enabled
        </label>
        <label
          style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 13, color: "#444" }}
          title="When on, this model can only be the primary — it is never routed as a delegate. Recommended for a Claude subscription (ToS)."
        >
          <input type="checkbox" checked={primaryOnly} onChange={(e) => setPrimaryOnly(e.target.checked)} disabled={busy} />
          primary only
        </label>
      </div>

      <ChipSelect
        label="roles"
        selected={roles}
        options={knownRoles}
        onChange={setRoles}
        hint="Toggle whether this model serves this role."
      />
      <ChipSelect
        label="personas"
        selected={personas}
        options={knownPersonas}
        onChange={setPersonas}
        emptyHint="(none set = all)"
        hint="Toggle whether this model is bound to this persona (none set = all)."
      />

      {err && <div style={{ fontSize: 12, color: "#c00", marginTop: 8 }}>{err}</div>}

      <div style={{ display: "flex", alignItems: "center", gap: 10, marginTop: 14 }}>
        <Button
          variant="primary"
          size="md"
          onClick={() => void save()}
          disabled={busy}
          title="Save all changes to this model."
        >
          {busy ? "Saving…" : "Save"}
        </Button>
        <Button onClick={onClose} disabled={busy} size="md" title="Discard changes and close the editor.">
          Cancel
        </Button>
        <Button
          variant="danger"
          size="md"
          onClick={() => void remove()}
          disabled={busy}
          style={{ marginLeft: "auto" }}
          title="Remove this model from the roster."
        >
          Remove model
        </Button>
      </div>
    </Modal>
  );
}

/* ---- add model: the SAME chooser + flows as the wizard's add ----
 * One code path (PrimaryChooser) drives both surfaces; its 'delegate' mode only
 * collects a roster name + roles and skips the --default promotion. Fine-tuning
 * (cost tier, disable, endpoint tweaks) lives in the edit modal afterwards. */

function AddModel({ onDone }: { onDone: (msg: string, ok: boolean) => void }) {
  return (
    <Panel title="Add model">
      <div style={{ padding: "12px" }}>
        <PrimaryChooser
          mode="delegate"
          onConfigured={(provider) => onDone(`added ${provider} model`, true)}
        />
      </div>
    </Panel>
  );
}

/* ---- small presentational helpers ---- */

// Editable multi-select of tokens (roles or personas) as toggleable chips, fully
// controlled by the parent (the Edit modal saves the whole set on Save). The "all"
// wildcard is always offered; free selection keeps models matched to what personas
// declare.
function ChipSelect({
  label,
  selected,
  options,
  onChange,
  emptyHint,
  hint,
}: {
  label: string;
  selected: string[];
  options: string[];
  onChange: (v: string[]) => void;
  emptyHint?: string;
  hint?: string;
}) {
  const all = useMemo(() => {
    const s = new Set<string>(["all", ...options, ...selected]);
    return Array.from(s);
  }, [options.join(","), selected.join(",")]);
  const toggle = (r: string) =>
    onChange(selected.includes(r) ? selected.filter((x) => x !== r) : [...selected, r]);
  return (
    <div style={{ margin: "10px 0 2px" }}>
      <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
        <span style={{ color: "#888", fontSize: 12 }}>{label}</span>
        {selected.length === 0 && emptyHint && (
          <span style={{ color: "#aaa", fontSize: 11 }}>{emptyHint}</span>
        )}
      </div>
      <div style={{ display: "flex", flexWrap: "wrap", gap: 4, marginTop: 3 }}>
        {all.map((r) => {
          const on = selected.includes(r);
          return (
            <button
              key={r}
              onClick={() => toggle(r)}
              title={hint}
              style={{
                ...btnSmall,
                padding: "1px 7px",
                background: on ? (r === "all" ? "#a15" : "#1f7a3d") : "#fff",
                color: on ? "#fff" : "#555",
                fontWeight: r === "all" ? 600 : 400,
              }}
            >
              {r}
            </button>
          );
        })}
      </div>
    </div>
  );
}

// Read-only chips for the base card (roles/personas shown, not editable here).
function StaticChips({ label, values, emptyHint }: { label: string; values: string[]; emptyHint?: string }) {
  return (
    <div style={{ margin: "6px 0" }}>
      <span style={{ color: "#888", fontSize: 12 }}>{label}</span>
      <div style={{ display: "flex", flexWrap: "wrap", gap: 4, marginTop: 3 }}>
        {values.length === 0 ? (
          <span style={{ color: "#aaa", fontSize: 11 }}>{emptyHint || "—"}</span>
        ) : (
          values.map((r) => (
            <span
              key={r}
              style={{
                fontSize: 12,
                padding: "1px 7px",
                borderRadius: 4,
                border: "1px solid #dfe6ef",
                background: "#f4f7fb",
                color: "#556",
              }}
            >
              {r}
            </span>
          ))
        )}
      </div>
    </div>
  );
}


function L({ label, title, children }: { label: string; title?: string; children: React.ReactNode }) {
  return (
    <label style={{ display: "block", fontSize: 12 }} title={title}>
      <span style={{ color: "#888", display: "block", marginBottom: 2 }}>{label}</span>
      {children}
    </label>
  );
}

function fmt(n: number): string {
  if (n >= 1_000_000) return `${(n / 1_000_000).toFixed(1)}M`;
  if (n >= 1_000) return `${(n / 1_000).toFixed(1)}k`;
  return String(n);
}

/* ---- inline styles (match the Edit Workflows page) ---- */
const btn: React.CSSProperties = {
  fontSize: 13,
  padding: "4px 10px",
  border: "1px solid #ccc",
  borderRadius: 4,
  background: "#fff",
  cursor: "pointer",
};
const btnSmall: React.CSSProperties = { ...btn, padding: "2px 8px", fontSize: 12 };
const inp: React.CSSProperties = {
  fontSize: 13,
  padding: "4px 6px",
  border: "1px solid #ccc",
  borderRadius: 4,
  width: "100%",
  boxSizing: "border-box",
};
