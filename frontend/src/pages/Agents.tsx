import { useEffect, useState, useCallback, useMemo } from "react";
import { Panel, Badge, Spinner } from "@rakuensoftware/smoothgui";
import PrimaryChooser from "../setup/PrimaryChooser";

/* ---- API types ---- */

// GET /api/agents -> one entry per configured delegate (server_agent_to_json).
interface AgentCfg {
  name: string;
  endpoint: string;
  model: string;
  auth_type?: string;
  provider: string;
  cost_tier?: number;
  enabled: boolean;
  tools_enabled?: boolean;
  max_turns?: number;
  max_parallel?: number;
  context_window?: number;
  roles?: string[];
  personas?: string[];
}

// GET /api/agents/stats -> per-delegate run stats (agent_log JOIN token_audit).
interface AgentStats {
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

// POST /api/agents/probe -> live reachability of one delegate.
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

export default function Agents() {
  const [agents, setAgents] = useState<AgentCfg[]>([]);
  const [stats, setStats] = useState<Record<string, AgentStats>>({});
  const [probes, setProbes] = useState<Record<string, ProbeState>>({});
  const [loading, setLoading] = useState(false);
  const [status, setStatus] = useState<{ kind: "ok" | "err"; msg: string } | null>(
    null,
  );
  const [showAdd, setShowAdd] = useState(false);
  // The agent currently open in the Edit modal (null = closed). ALL mutations —
  // config fields, roles/personas binding, enable, remove — happen there; the base
  // list is read-only so a binding can never change from a stray click.
  const [editing, setEditing] = useState<AgentCfg | null>(null);
  // The known role + persona vocabularies, offered (with "all") when assigning
  // an agent's roles/personas so they match what personas declare.
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
      getJSON<{ agents: AgentCfg[] }>("/api/agents")
        .then((d) => setAgents(d.agents || []))
        .catch(() => setAgents([])),
      getJSON<{ stats: AgentStats[] }>("/api/agents/stats")
        .then((d) => {
          const map: Record<string, AgentStats> = {};
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
      const res = await postArgs<ProbeResult>("/api/agents/probe", [name]);
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
    for (const a of agents) void probe(a.name);
  }, [agents, probe]);

  // Keep the modal's view of the agent in sync after a save-triggered refresh so
  // it reflects the persisted record (and closes cleanly if the agent was removed).
  useEffect(() => {
    if (!editing) return;
    const fresh = agents.find((a) => a.name === editing.name);
    if (fresh && fresh !== editing) setEditing(fresh);
  }, [agents, editing]);

  return (
    <div style={{ padding: 16, fontFamily: "system-ui", height: "100%", overflow: "auto" }}>
      <div style={{ display: "flex", alignItems: "center", gap: 10, marginBottom: 12 }}>
        <strong style={{ fontSize: 18 }}>Agents</strong>
        <Badge label={`${agents.length}`} variant="neutral" />
        <button onClick={refresh} style={btn}>
          Refresh
        </button>
        <button onClick={probeAll} style={btn} disabled={!agents.length}>
          Probe all
        </button>
        <button
          onClick={() => setShowAdd((v) => !v)}
          style={{ ...btn, background: "#2563eb", color: "#fff", borderColor: "#2563eb" }}
        >
          {showAdd ? "Close" : "+ Add delegate"}
        </button>
        <Spinner loading={loading} text="loading…" />
        {status && (
          <span
            style={{
              fontSize: 13,
              color: status.kind === "err" ? "#c00" : "#070",
            }}
          >
            {status.msg}
          </span>
        )}
      </div>

      {showAdd && (
        <AddDelegate
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
        {agents.length === 0 && !loading && (
          <div style={{ color: "#888", fontSize: 14 }}>
            No delegates configured. Add one to get started.
          </div>
        )}
        {agents.map((a) => (
          <AgentCard
            key={a.name}
            agent={a}
            stats={stats[a.name]}
            probe={probes[a.name]}
            onProbe={() => probe(a.name)}
            onEdit={() => setEditing(a)}
          />
        ))}
      </div>

      {editing && (
        <AgentEditModal
          agent={editing}
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

/* ---- one delegate: READ-ONLY overview (config + availability + stats). All
   mutations live in the Edit modal, so no binding can change from this list. ---- */

function AgentCard({
  agent,
  stats,
  probe,
  onProbe,
  onEdit,
}: {
  agent: AgentCfg;
  stats?: AgentStats;
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
    <Panel title={agent.name}>
      <div style={{ display: "flex", gap: 16, flexWrap: "wrap", alignItems: "flex-start" }}>
        {/* left: configuration (read-only) */}
        <div style={{ flex: "1 1 260px", minWidth: 240 }}>
          <div style={{ display: "flex", alignItems: "center", gap: 6, margin: "2px 0 4px" }}>
            <Badge
              label={agent.enabled ? "enabled" : "disabled"}
              variant={agent.enabled ? "success" : "neutral"}
            />
          </div>
          <Field k="provider" v={agent.provider || "—"} />
          <Field k="model" v={agent.model || "—"} />
          <Field k="endpoint" v={agent.endpoint || "(cli / none)"} mono />
          {typeof agent.cost_tier === "number" && (
            <Field k="cost tier" v={String(agent.cost_tier)} />
          )}
          {typeof agent.max_parallel === "number" && agent.max_parallel > 0 && (
            <Field k="max parallel" v={String(agent.max_parallel)} />
          )}
          {typeof agent.max_turns === "number" && agent.max_turns >= 0 && (
            <Field k="max turns" v={String(agent.max_turns)} />
          )}
          {agent.context_window ? (
            <Field k="context" v={`${agent.context_window.toLocaleString()} tok`} />
          ) : null}
          <Field k="tools" v={agent.tools_enabled ? "enabled" : "disabled"} />
          <StaticChips label="roles" values={agent.roles || []} />
          <StaticChips label="personas" values={agent.personas || []} emptyHint="(none = all)" />
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
          <button
            onClick={onProbe}
            style={{ ...btnSmall, marginTop: 6 }}
            disabled={pstate === "probing"}
          >
            {pstate === "probing" ? "probing…" : "Probe"}
          </button>
        </div>

        {/* right: run stats */}
        <div style={{ flex: "1 1 220px", minWidth: 200 }}>
          <div style={{ fontSize: 12, color: "#999", marginBottom: 2 }}>run stats</div>
          {stats && stats.total_calls > 0 ? (
            <>
              <Field k="runs" v={String(stats.total_calls)} />
              <Field
                k="ok / failed"
                v={`${stats.successful_calls} / ${stats.failed_calls}`}
              />
              <Field k="success" v={successPct} />
              <Field k="avg latency" v={`${stats.avg_latency_ms} ms`} />
              <Field
                k="tokens (in/out)"
                v={`${fmt(stats.prompt_tokens)} / ${fmt(stats.completion_tokens)}`}
              />
              {stats.estimated_cost_usd > 0 && (
                <Field k="est. cost" v={`$${stats.estimated_cost_usd.toFixed(4)}`} />
              )}
            </>
          ) : (
            <div style={{ fontSize: 13, color: "#aaa" }}>no runs recorded yet</div>
          )}
        </div>
      </div>

      <div style={{ marginTop: 8, borderTop: "1px solid #eee", paddingTop: 8 }}>
        <button
          onClick={onEdit}
          style={{ ...btnSmall, background: "#2563eb", color: "#fff", borderColor: "#2563eb" }}
        >
          Edit
        </button>
      </div>
    </Panel>
  );
}

/* ---- per-agent Edit modal: edit ALL aspects; the one place binding changes are
   made. Saves via a single surgical POST /api/agents/set. ---- */

function AgentEditModal({
  agent,
  knownRoles,
  knownPersonas,
  onClose,
  onSaved,
  onStatus,
}: {
  agent: AgentCfg;
  knownRoles: string[];
  knownPersonas: string[];
  onClose: () => void;
  onSaved: () => void;
  onStatus: (msg: string, ok: boolean) => void;
}) {
  const [provider, setProvider] = useState(agent.provider || "openai");
  const [model, setModel] = useState(agent.model || "");
  const [endpoint, setEndpoint] = useState(agent.endpoint || "");
  const [costTier, setCostTier] = useState(String(agent.cost_tier ?? 0));
  const [maxTurns, setMaxTurns] = useState(String(agent.max_turns ?? -1));
  const [maxParallel, setMaxParallel] = useState(String(agent.max_parallel ?? 0));
  const [contextWindow, setContextWindow] = useState(String(agent.context_window ?? 0));
  const [tools, setTools] = useState(!!agent.tools_enabled);
  const [enabled, setEnabled] = useState(!!agent.enabled);
  const [roles, setRoles] = useState<string[]>(agent.roles || []);
  const [personas, setPersonas] = useState<string[]>(agent.personas || []);
  const [apiKey, setApiKey] = useState("");
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState("");

  const cliProvider = provider === "claude" || provider === "claude-code";

  // Esc closes the modal.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);

  const save = async () => {
    setBusy(true);
    setErr("");
    // One surgical set carrying every editable field. The server patches only what
    // is passed; roles empty resets to the default set, personas empty resets to "all".
    const args = [
      agent.name,
      "--provider", provider,
      "--model", model.trim(),
      "--endpoint", endpoint.trim(),
      "--cost-tier", costTier || "0",
      "--max-turns", maxTurns || "-1",
      "--max-parallel", maxParallel || "0",
      "--context-window", contextWindow || "0",
      "--tools", tools ? "on" : "off",
      "--enabled", enabled ? "true" : "false",
      "--roles", roles.join(","),
      "--personas", personas.join(","),
    ];
    if (apiKey.trim()) args.push("--key", apiKey.trim());
    try {
      const res = await postArgs<{ error?: string }>("/api/agents/set", args);
      if (res.error) setErr(res.error);
      else {
        onStatus(`${agent.name} saved`, true);
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
    if (!confirm(`Remove delegate “${agent.name}”? This edits agents.json.`)) return;
    setBusy(true);
    setErr("");
    try {
      const res = await postArgs<{ error?: string }>("/api/agents/remove", [agent.name]);
      if (res.error) setErr(res.error);
      else {
        onStatus(`removed ${agent.name}`, true);
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
    <div
      onClick={onClose}
      style={{
        position: "fixed",
        inset: 0,
        zIndex: 30,
        background: "rgba(0,0,0,0.35)",
        display: "flex",
        alignItems: "flex-start",
        justifyContent: "center",
        padding: 24,
        overflow: "auto",
      }}
    >
      <div
        onClick={(e) => e.stopPropagation()}
        style={{
          background: "#fff",
          borderRadius: 8,
          maxWidth: 640,
          width: "100%",
          boxShadow: "0 8px 32px rgba(0,0,0,0.25)",
        }}
      >
        <div
          style={{
            display: "flex",
            alignItems: "center",
            gap: 10,
            padding: "12px 16px",
            borderBottom: "1px solid #eee",
            position: "sticky",
            top: 0,
            background: "#fff",
            borderRadius: "8px 8px 0 0",
          }}
        >
          <strong style={{ fontSize: 15 }}>Edit delegate</strong>
          <span style={{ fontSize: 13, color: "#667", fontFamily: "monospace" }}>{agent.name}</span>
          <button onClick={onClose} style={{ ...btn, marginLeft: "auto" }}>
            Close
          </button>
        </div>

        <div style={{ padding: 16 }}>
          <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 10 }}>
            <L label="provider">
              <select value={provider} onChange={(e) => setProvider(e.target.value)} style={inp} disabled={busy}>
                {(PROVIDERS.includes(provider) ? PROVIDERS : [provider, ...PROVIDERS]).map((p) => (
                  <option key={p} value={p}>
                    {p}
                  </option>
                ))}
              </select>
            </L>
            <L label="model">
              <input value={model} onChange={(e) => setModel(e.target.value)} style={inp} disabled={busy} />
            </L>
            <L label={cliProvider ? "endpoint (optional for CLI)" : "endpoint"}>
              <input
                value={endpoint}
                onChange={(e) => setEndpoint(e.target.value)}
                style={inp}
                placeholder="https://host:port/v1"
                disabled={busy}
              />
            </L>
            <L label="cost tier">
              <input type="number" value={costTier} onChange={(e) => setCostTier(e.target.value)} style={inp} min={0} disabled={busy} />
            </L>
            <L label="max turns (-1 = default)">
              <input type="number" value={maxTurns} onChange={(e) => setMaxTurns(e.target.value)} style={inp} disabled={busy} />
            </L>
            <L label="max parallel">
              <input type="number" value={maxParallel} onChange={(e) => setMaxParallel(e.target.value)} style={inp} min={0} disabled={busy} />
            </L>
            <L label="context window (tok, 0 = auto)">
              <input type="number" value={contextWindow} onChange={(e) => setContextWindow(e.target.value)} style={inp} min={0} disabled={busy} />
            </L>
            <L label="API key (blank = keep current)">
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
            <label style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 13, color: "#444" }}>
              <input type="checkbox" checked={enabled} onChange={(e) => setEnabled(e.target.checked)} disabled={busy} />
              enabled
            </label>
            <label style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 13, color: "#444" }}>
              <input type="checkbox" checked={tools} onChange={(e) => setTools(e.target.checked)} disabled={busy} />
              tools enabled
            </label>
          </div>

          <ChipSelect label="roles" selected={roles} options={knownRoles} onChange={setRoles} />
          <ChipSelect
            label="personas"
            selected={personas}
            options={knownPersonas}
            onChange={setPersonas}
            emptyHint="(none set = all)"
          />

          {err && <div style={{ fontSize: 12, color: "#c00", marginTop: 8 }}>{err}</div>}

          <div style={{ display: "flex", alignItems: "center", gap: 10, marginTop: 14 }}>
            <button
              onClick={() => void save()}
              disabled={busy}
              style={{ ...btn, background: "#2563eb", color: "#fff", borderColor: "#2563eb" }}
            >
              {busy ? "Saving…" : "Save"}
            </button>
            <button onClick={onClose} disabled={busy} style={btn}>
              Cancel
            </button>
            <button
              onClick={() => void remove()}
              disabled={busy}
              style={{ ...btn, marginLeft: "auto", background: "#fff5f5", color: "#c00", borderColor: "#e6b3b3" }}
            >
              Remove delegate
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}

/* ---- add delegate: the SAME chooser + flows as the wizard's add agent ----
 * One code path (PrimaryChooser) drives both surfaces; 'delegate' mode only
 * collects a roster name + roles and skips the --default promotion. Fine-tuning
 * (cost tier, disable, endpoint tweaks) lives in the edit modal afterwards. */

function AddDelegate({ onDone }: { onDone: (msg: string, ok: boolean) => void }) {
  return (
    <Panel title="Add delegate">
      <div style={{ padding: "12px" }}>
        <PrimaryChooser
          mode="delegate"
          onConfigured={(provider) => onDone(`added ${provider} delegate`, true)}
        />
      </div>
    </Panel>
  );
}

/* ---- small presentational helpers ---- */

// Editable multi-select of tokens (roles or personas) as toggleable chips, fully
// controlled by the parent (the Edit modal saves the whole set on Save). The "all"
// wildcard is always offered; free selection keeps agents matched to what personas
// declare.
function ChipSelect({
  label,
  selected,
  options,
  onChange,
  emptyHint,
}: {
  label: string;
  selected: string[];
  options: string[];
  onChange: (v: string[]) => void;
  emptyHint?: string;
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

function Field({ k, v, mono }: { k: string; v: string; mono?: boolean }) {
  return (
    <div style={{ display: "flex", justifyContent: "space-between", gap: 8, fontSize: 13, padding: "2px 0" }}>
      <span style={{ color: "#888" }}>{k}</span>
      <span
        style={{
          fontFamily: mono ? "monospace" : undefined,
          textAlign: "right",
          wordBreak: "break-all",
        }}
      >
        {v}
      </span>
    </div>
  );
}

function L({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <label style={{ display: "block", fontSize: 12 }}>
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
