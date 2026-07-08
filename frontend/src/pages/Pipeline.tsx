import { useCallback, useEffect, useState } from "react";
import { FIELD_HELP } from "./settingsHelp";

/* Pipeline page: the curator pipeline as an ordered, resource-lane-grouped view.
 *
 * Option B (single source of truth): the stage list, order, lanes, and the
 * config key that toggles each stage come LIVE from the backend registry via
 * POST /api/curator/stages (curator.stages -> CURATOR_STAGES + the projection
 * sweeps in src/kb/kb_curator_drain.c). There is no hand-kept frontend mirror to
 * drift. Enable state comes from GET /api/config and toggles via POST
 * /api/config/set (persisted to aimee.yaml; the KB picks it up on next load). A
 * stage with a null config_key is embedder-gated (active whenever an embedder is
 * configured) and shown read-only. Descriptions come from the shared FIELD_HELP. */

type Val = boolean | number | string;
type Lane = "llm" | "index";
type Stage = {
  name: string;
  label: string;
  lane: Lane;
  budget: number;
  order: number;
  config_key: string | null;
};
type Preset = { name: string; description: string; enabled: string[] };

const csrf = () => ({ "X-CSRF-Token": window._csrf || "" });

async function postJSON(url: string, body: unknown): Promise<{ status: number; error?: string }> {
  const r = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json", ...csrf() },
    body: JSON.stringify(body),
  });
  let d: { error?: string } = {};
  try { d = await r.json(); } catch { /* empty */ }
  return { status: r.status, error: d.error };
}

const LANE_META: Record<Lane, { title: string; hint: string; color: string }> = {
  llm: { title: "LLM lane · GPU", hint: "One unit per pass — extraction/reasoning on the GPU model.", color: "#8cf" },
  index: { title: "Index lane · CPU", hint: "Drains its queue each pass — embedding + SQL, concurrent with the GPU lane.", color: "#7d7" },
};

const DEFAULT_DESC = "Active whenever an embedder is configured (no individual toggle).";

export default function Pipeline() {
  const [stages, setStages] = useState<Stage[]>([]);
  const [presets, setPresets] = useState<Preset[]>([]);
  const [cfg, setCfg] = useState<Record<string, Val>>({});
  const [loaded, setLoaded] = useState(false);
  const [busy, setBusy] = useState<string>("");
  const [status, setStatus] = useState<{ kind: "ok" | "err"; msg: string } | null>(null);

  const refresh = useCallback(() => {
    Promise.all([
      fetch("/api/curator/stages", { method: "POST", headers: { "Content-Type": "application/json", ...csrf() }, body: "{}" })
        .then((r) => r.json())
        .then((d: { stages?: Stage[]; presets?: Preset[] }) => ({ stages: d.stages || [], presets: d.presets || [] }))
        .catch(() => ({ stages: [] as Stage[], presets: [] as Preset[] })),
      fetch("/api/config", { headers: csrf() })
        .then((r) => r.json())
        .then((d: { config?: Record<string, Val> }) => d.config || {})
        .catch(() => ({} as Record<string, Val>)),
    ]).then(([sp, c]) => {
      setStages([...sp.stages].sort((a, b) => a.order - b.order));
      setPresets(sp.presets);
      setCfg(c);
      setLoaded(true);
    });
  }, []);
  useEffect(() => { refresh(); }, [refresh]);

  const toggle = useCallback(async (key: string, next: boolean) => {
    setBusy(key);
    const { status: st, error } = await postJSON("/api/config/set", { key, value: next });
    setBusy("");
    if (st >= 200 && st < 300 && !error) {
      setCfg((p) => ({ ...p, [key]: next }));
      setStatus({ kind: "ok", msg: `${key} ${next ? "enabled" : "disabled"}` });
    } else {
      setStatus({ kind: "err", msg: error || `save failed (${st})` });
    }
  }, []);

  // Apply a preset: enable its listed config keys, disable every other toggleable
  // stage. One config.set per stage, in parallel; embedder-gated stages (no
  // config_key) are untouched.
  const applyPreset = useCallback(async (p: Preset) => {
    setBusy(`preset:${p.name}`);
    const on = new Set(p.enabled);
    const targets = stages.filter((s) => s.config_key);
    const results = await Promise.all(
      targets.map((s) => postJSON("/api/config/set", { key: s.config_key, value: on.has(s.config_key as string) })),
    );
    setBusy("");
    setCfg((prev) => {
      const next = { ...prev };
      for (const s of targets) next[s.config_key as string] = on.has(s.config_key as string);
      return next;
    });
    const failed = results.filter((r) => r.error || r.status < 200 || r.status >= 300).length;
    setStatus(
      failed
        ? { kind: "err", msg: `preset "${p.name}" applied with ${failed} error(s)` }
        : { kind: "ok", msg: `preset "${p.name}" applied` },
    );
  }, [stages]);

  const isOn = (k: string) => cfg[k] === true || cfg[k] === 1;

  const row = (s: Stage, i: number) => {
    const gated = !s.config_key;
    const on = gated ? true : isOn(s.config_key as string);
    const desc = (s.config_key && FIELD_HELP[s.config_key]) || DEFAULT_DESC;
    return (
      <div key={s.name} style={{
        display: "flex", alignItems: "center", gap: 12, padding: "8px 12px",
        borderBottom: "1px solid #eee", opacity: gated ? 0.75 : 1,
      }}>
        <span style={{ width: 22, color: "#aaa", fontSize: 12, fontFamily: "ui-monospace, monospace" }}>{i + 1}</span>
        <div style={{ flex: 1, minWidth: 0 }}>
          <div style={{ fontWeight: 600, fontSize: 14 }}>{s.label}
            {gated && <span style={{ marginLeft: 8, fontSize: 11, color: "#999" }}>(embedder-gated)</span>}
          </div>
          <div style={{ fontSize: 12, color: "#777" }}>{desc}</div>
        </div>
        {gated ? (
          <span style={{ fontSize: 12, color: "#7d7", whiteSpace: "nowrap" }}>● active</span>
        ) : (
          <button
            disabled={busy === s.config_key}
            onClick={() => toggle(s.config_key as string, !on)}
            title={on ? "Disable stage" : "Enable stage"}
            style={{
              width: 52, height: 26, borderRadius: 13, border: "1px solid #ccc", cursor: "pointer",
              background: on ? "#7d7" : "#ddd", position: "relative", transition: "background .15s",
            }}
          >
            <span style={{
              position: "absolute", top: 2, left: on ? 28 : 2, width: 20, height: 20,
              borderRadius: "50%", background: "#fff", transition: "left .15s",
            }} />
          </button>
        )}
      </div>
    );
  };

  if (!loaded) return <div style={{ padding: 24, color: "#888" }}>Loading pipeline…</div>;

  return (
    <div style={{ padding: "18px 24px", maxWidth: 860, margin: "0 auto", fontFamily: "system-ui" }}>
      <h2 style={{ margin: "0 0 4px" }}>Curator pipeline</h2>
      <p style={{ color: "#777", fontSize: 13, margin: "0 0 18px" }}>
        Ingested content flows through these stages into curated knowledge (claims, entities,
        contradictions, graph). Toggle a stage to include/exclude it; changes persist to aimee.yaml and
        take effect on the KB's next config load. Stages run in two resource lanes concurrently, and this
        view is generated live from the backend stage registry.
      </p>
      {presets.length > 0 && (
        <div style={{ display: "flex", alignItems: "center", gap: 8, flexWrap: "wrap", marginBottom: 18 }}>
          <span style={{ fontSize: 13, fontWeight: 600, color: "#555" }}>Presets:</span>
          {presets.map((p) => (
            <button
              key={p.name}
              disabled={busy === `preset:${p.name}`}
              onClick={() => applyPreset(p)}
              title={p.description}
              style={{
                padding: "5px 12px", fontSize: 13, borderRadius: 14, cursor: "pointer",
                border: "1px solid #cbd5e1", background: busy === `preset:${p.name}` ? "#eef" : "#f8fafc",
              }}
            >
              {p.name}
            </button>
          ))}
          <span style={{ fontSize: 12, color: "#999" }}>apply a profile, then fine-tune below</span>
        </div>
      )}
      {(["llm", "index"] as Lane[]).map((lane) => {
        const meta = LANE_META[lane];
        const laneStages = stages.filter((s) => s.lane === lane);
        if (!laneStages.length) return null;
        return (
          <div key={lane} style={{ marginBottom: 22, border: "1px solid #e2e2e2", borderRadius: 8, overflow: "hidden" }}>
            <div style={{ padding: "8px 12px", background: "#fafafa", borderBottom: "1px solid #e2e2e2" }}>
              <span style={{ fontWeight: 700, color: meta.color }}>{meta.title}</span>
              <span style={{ marginLeft: 10, fontSize: 12, color: "#999" }}>{meta.hint}</span>
            </div>
            {laneStages.map((s, i) => row(s, i))}
          </div>
        );
      })}
      {!stages.length && (
        <div style={{ color: "#888" }}>No stages reported (aimee-server unreachable, or the KB has no curator registry).</div>
      )}
      {status && (
        <div style={{ fontSize: 13, color: status.kind === "ok" ? "#2a2" : "#c33", marginTop: 8 }}>{status.msg}</div>
      )}
    </div>
  );
}
