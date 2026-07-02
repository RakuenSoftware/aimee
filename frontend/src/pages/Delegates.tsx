import { useEffect, useState, useCallback } from "react";
import { Panel, Badge, Spinner } from "@rakuensoftware/smoothgui";

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

export default function Delegates() {
  const [agents, setAgents] = useState<AgentCfg[]>([]);
  const [stats, setStats] = useState<Record<string, AgentStats>>({});
  const [probes, setProbes] = useState<Record<string, ProbeState>>({});
  const [loading, setLoading] = useState(false);
  const [status, setStatus] = useState<{ kind: "ok" | "err"; msg: string } | null>(
    null,
  );
  const [showAdd, setShowAdd] = useState(false);

  const refresh = useCallback(() => {
    setLoading(true);
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

  const setEnabled = useCallback(
    async (name: string, enabled: boolean) => {
      setStatus(null);
      try {
        const res = await postArgs<{ error?: string }>(
          enabled ? "/api/agents/enable" : "/api/agents/disable",
          [name],
        );
        if (res.error) setStatus({ kind: "err", msg: res.error });
        else setStatus({ kind: "ok", msg: `${name} ${enabled ? "enabled" : "disabled"}` });
      } catch {
        setStatus({ kind: "err", msg: `failed to ${enabled ? "enable" : "disable"} ${name}` });
      }
      refresh();
    },
    [refresh],
  );

  const remove = useCallback(
    async (name: string) => {
      if (!confirm(`Remove delegate “${name}”? This edits agents.json.`)) return;
      setStatus(null);
      try {
        const res = await postArgs<{ error?: string }>("/api/agents/remove", [name]);
        if (res.error) setStatus({ kind: "err", msg: res.error });
        else setStatus({ kind: "ok", msg: `removed ${name}` });
      } catch {
        setStatus({ kind: "err", msg: `failed to remove ${name}` });
      }
      refresh();
    },
    [refresh],
  );

  return (
    <div style={{ padding: 16, fontFamily: "system-ui", height: "100%", overflow: "auto" }}>
      <div style={{ display: "flex", alignItems: "center", gap: 10, marginBottom: 12 }}>
        <strong style={{ fontSize: 18 }}>Delegates</strong>
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
          <DelegateCard
            key={a.name}
            agent={a}
            stats={stats[a.name]}
            probe={probes[a.name]}
            onProbe={() => probe(a.name)}
            onToggle={() => setEnabled(a.name, !a.enabled)}
            onRemove={() => remove(a.name)}
          />
        ))}
      </div>
    </div>
  );
}

/* ---- one delegate: config + availability + stats ---- */

function DelegateCard({
  agent,
  stats,
  probe,
  onProbe,
  onToggle,
  onRemove,
}: {
  agent: AgentCfg;
  stats?: AgentStats;
  probe?: ProbeState;
  onProbe: () => void;
  onToggle: () => void;
  onRemove: () => void;
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
        {/* left: configuration */}
        <div style={{ flex: "1 1 260px", minWidth: 240 }}>
          <Field k="provider" v={agent.provider || "—"} />
          <Field k="model" v={agent.model || "—"} />
          <Field k="endpoint" v={agent.endpoint || "(cli / none)"} mono />
          <div style={{ display: "flex", alignItems: "center", gap: 6, margin: "4px 0" }}>
            <span style={{ color: "#888", fontSize: 13 }}>enabled</span>
            <Badge
              label={agent.enabled ? "enabled" : "disabled"}
              variant={agent.enabled ? "success" : "neutral"}
            />
            <button onClick={onToggle} style={btnSmall}>
              {agent.enabled ? "disable" : "enable"}
            </button>
          </div>
          {typeof agent.cost_tier === "number" && (
            <Field k="cost tier" v={String(agent.cost_tier)} />
          )}
          {typeof agent.max_parallel === "number" && agent.max_parallel > 0 && (
            <Field k="max parallel" v={String(agent.max_parallel)} />
          )}
          {agent.context_window ? (
            <Field k="context" v={`${agent.context_window.toLocaleString()} tok`} />
          ) : null}
          {agent.roles && agent.roles.length > 0 && (
            <div style={{ margin: "4px 0", display: "flex", flexWrap: "wrap", gap: 4 }}>
              {agent.roles.map((r) => (
                <Badge key={r} label={r} variant="info" />
              ))}
            </div>
          )}
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
          onClick={onRemove}
          style={{ ...btnSmall, color: "#c00", borderColor: "#e0a0a0" }}
        >
          Remove delegate
        </button>
      </div>
    </Panel>
  );
}

/* ---- add-delegate form: builds the `aimee agent add` argv ---- */

function AddDelegate({ onDone }: { onDone: (msg: string, ok: boolean) => void }) {
  const [name, setName] = useState("");
  const [endpoint, setEndpoint] = useState("");
  const [model, setModel] = useState("");
  const [provider, setProvider] = useState("openai");
  const [roles, setRoles] = useState("summarize,format,draft");
  const [apiKey, setApiKey] = useState("");
  const [costTier, setCostTier] = useState("0");
  const [disabled, setDisabled] = useState(false);
  const [busy, setBusy] = useState(false);

  // CLI-backed providers (claude/claude-code) run a local CLI, not an HTTP
  // endpoint, so the endpoint field is optional for them (server ignores it).
  const cliProvider = provider === "claude" || provider === "claude-code";

  const submit = async () => {
    if (!name.trim() || !model.trim() || (!cliProvider && !endpoint.trim())) {
      onDone("name, endpoint and model are required", false);
      return;
    }
    // Mirror `aimee agent add <name> <endpoint> <model> [--flags]`. Endpoint is a
    // required positional; pass "-" as a placeholder for CLI providers.
    const args = [name.trim(), cliProvider ? "-" : endpoint.trim(), model.trim()];
    if (provider) args.push("--provider", provider);
    if (roles.trim()) args.push("--roles", roles.trim());
    if (apiKey.trim()) args.push("--key", apiKey.trim());
    if (costTier && costTier !== "0") args.push("--cost-tier", costTier);
    if (disabled) args.push("--disabled");

    setBusy(true);
    try {
      const res = await postArgs<{ error?: string; name?: string }>(
        "/api/agents/add",
        args,
      );
      if (res.error) onDone(res.error, false);
      else onDone(`added ${res.name || name}`, true);
    } catch {
      onDone("add failed", false);
    } finally {
      setBusy(false);
    }
  };

  return (
    <Panel title="Add delegate">
      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 8 }}>
        <L label="name">
          <input value={name} onChange={(e) => setName(e.target.value)} style={inp} placeholder="my-delegate" />
        </L>
        <L label="provider">
          <select value={provider} onChange={(e) => setProvider(e.target.value)} style={inp}>
            {PROVIDERS.map((p) => (
              <option key={p} value={p}>
                {p}
              </option>
            ))}
          </select>
        </L>
        <L label={cliProvider ? "endpoint (optional for CLI)" : "endpoint"}>
          <input
            value={endpoint}
            onChange={(e) => setEndpoint(e.target.value)}
            style={inp}
            placeholder="https://host:port/v1"
            disabled={cliProvider}
          />
        </L>
        <L label="model">
          <input value={model} onChange={(e) => setModel(e.target.value)} style={inp} placeholder="gpt-5" />
        </L>
        <L label="roles (comma-separated)">
          <input value={roles} onChange={(e) => setRoles(e.target.value)} style={inp} />
        </L>
        <L label="cost tier">
          <input
            type="number"
            value={costTier}
            onChange={(e) => setCostTier(e.target.value)}
            style={inp}
            min={0}
          />
        </L>
        <L label="API key (optional — stored in vault)">
          <input
            type="password"
            value={apiKey}
            onChange={(e) => setApiKey(e.target.value)}
            style={inp}
            placeholder="sk-…  or  $ENV_VAR"
          />
        </L>
        <L label="start disabled">
          <input type="checkbox" checked={disabled} onChange={(e) => setDisabled(e.target.checked)} />
        </L>
      </div>
      <div style={{ marginTop: 10 }}>
        <button
          onClick={submit}
          disabled={busy}
          style={{ ...btn, background: "#2563eb", color: "#fff", borderColor: "#2563eb" }}
        >
          {busy ? "Adding…" : "Add delegate"}
        </button>
      </div>
    </Panel>
  );
}

/* ---- small presentational helpers ---- */

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
