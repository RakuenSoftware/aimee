import { useCallback, useEffect, useState } from "react";
import { FIELD_HELP } from "./settingsHelp";

/* Pipeline page: the curator pipeline as an ordered, resource-lane-grouped view.
 *
 * Option B (single source of truth): the stage list, order, lanes, config key, and
 * dependency `requires` come LIVE from the backend registry via POST
 * /api/curator/stages (curator.stages -> CURATOR_STAGES + the projection sweeps in
 * src/kb/kb_curator_drain.c). Enable state comes from GET /api/config; toggles and
 * the stage order persist via POST /api/config/set (aimee.yaml; the KB picks it up
 * on next load). A null config_key = embedder-gated (read-only). Descriptions come
 * from the shared FIELD_HELP.
 *
 * Reorder: ▲▼ moves a stage within its lane (cross-lane order is meaningless — the
 * lanes run concurrently). A move that would place a stage before a same-lane
 * prerequisite (or a prerequisite after its dependent) is refused client-side; the
 * backend independently validates kb_curator_stage_order and falls back to registry
 * order on anything invalid. The persisted order is the full stage sequence. */

type Val = boolean | number | string;
type Lane = "llm" | "index";
type Stage = {
  name: string;
  label: string;
  lane: Lane;
  budget: number;
  order: number;
  config_key: string | null;
  requires: string[];
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

// Display order: kb_curator_stage_order (listed first, in order) then any unlisted
// in registry order — mirrors the backend kb_curator_ordered_stages fail-safe.
function applyOrder(list: Stage[], orderStr: string): Stage[] {
  const byOrder = [...list].sort((a, b) => a.order - b.order);
  if (!orderStr) return byOrder;
  const names = orderStr.split(",").map((s) => s.trim()).filter(Boolean);
  const byName = new Map(byOrder.map((s) => [s.name, s]));
  const out: Stage[] = [];
  const used = new Set<string>();
  for (const n of names) {
    const s = byName.get(n);
    if (s && !used.has(n)) { out.push(s); used.add(n); }
  }
  for (const s of byOrder) if (!used.has(s.name)) out.push(s);
  return out;
}

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
      const orderStr = typeof c.kb_curator_stage_order === "string" ? c.kb_curator_stage_order : "";
      setStages(applyOrder(sp.stages, orderStr));
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
  // stage. One config.set per stage, in parallel; embedder-gated stages untouched.
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

  const persistOrder = useCallback(async (arr: Stage[]) => {
    const order = arr.map((s) => s.name).join(",");
    setBusy("order");
    const { status: st, error } = await postJSON("/api/config/set", { key: "kb_curator_stage_order", value: order });
    setBusy("");
    if (st >= 200 && st < 300 && !error) {
      setCfg((p) => ({ ...p, kb_curator_stage_order: order }));
      setStatus({ kind: "ok", msg: "stage order saved" });
    } else {
      setStatus({ kind: "err", msg: error || `save failed (${st})` });
    }
  }, []);

  // Move a stage one slot within its lane. Refused if it would cross a same-lane
  // dependency edge (using `requires` from the backend DAG).
  const move = useCallback((name: string, dir: -1 | 1) => {
    const cur = stages;
    const s = cur.find((x) => x.name === name);
    if (!s) return;
    const laneIdx = cur.map((x, i) => ({ x, i })).filter((o) => o.x.lane === s.lane).map((o) => o.i);
    const pos = laneIdx.findIndex((i) => cur[i].name === name);
    const tgt = pos + dir;
    if (tgt < 0 || tgt >= laneIdx.length) return;
    const iA = laneIdx[pos];
    const iB = laneIdx[tgt];
    const other = cur[iB];
    if (dir === -1 && s.requires.includes(other.name)) {
      setStatus({ kind: "err", msg: `${s.label} must stay after its prerequisite ${other.label}` });
      return;
    }
    if (dir === 1 && other.requires.includes(s.name)) {
      setStatus({ kind: "err", msg: `${s.label} must stay before ${other.label}, which depends on it` });
      return;
    }
    const next = [...cur];
    [next[iA], next[iB]] = [next[iB], next[iA]];
    setStages(next);
    persistOrder(next);
  }, [stages, persistOrder]);

  const isOn = (k: string) => cfg[k] === true || cfg[k] === 1;

  const arrowBtn = (enabled: boolean, glyph: string, title: string, onClick: () => void) => (
    <button
      disabled={!enabled || busy === "order"}
      onClick={onClick}
      title={title}
      style={{
        width: 22, height: 20, fontSize: 11, lineHeight: "18px", padding: 0, borderRadius: 4,
        border: "1px solid #ccc", cursor: enabled ? "pointer" : "default",
        background: "#fff", color: enabled ? "#555" : "#ccc",
      }}
    >
      {glyph}
    </button>
  );

  const row = (s: Stage, i: number, laneCount: number) => {
    const gated = !s.config_key;
    const on = gated ? true : isOn(s.config_key as string);
    const desc = (s.config_key && FIELD_HELP[s.config_key]) || DEFAULT_DESC;
    return (
      <div key={s.name} style={{
        display: "flex", alignItems: "center", gap: 12, padding: "8px 12px",
        borderBottom: "1px solid #eee", opacity: gated ? 0.75 : 1,
      }}>
        <span style={{ display: "flex", flexDirection: "column", gap: 2 }}>
          {arrowBtn(i > 0, "▲", "Move earlier in this lane", () => move(s.name, -1))}
          {arrowBtn(i < laneCount - 1, "▼", "Move later in this lane", () => move(s.name, 1))}
        </span>
        <span style={{ width: 18, color: "#aaa", fontSize: 12, fontFamily: "ui-monospace, monospace" }}>{i + 1}</span>
        <div style={{ flex: 1, minWidth: 0 }}>
          <div style={{ fontWeight: 600, fontSize: 14 }}>{s.label}
            {gated && <span style={{ marginLeft: 8, fontSize: 11, color: "#999" }}>(embedder-gated)</span>}
            {s.requires.length > 0 && (
              <span style={{ marginLeft: 8, fontSize: 11, color: "#aaa" }}>after: {s.requires.join(", ")}</span>
            )}
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
        contradictions, graph). Toggle a stage to include/exclude it, or reorder within a lane with ▲▼
        (dependencies are enforced). Changes persist to aimee.yaml and take effect on the KB's next
        config load. This view is generated live from the backend stage registry.
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
            {laneStages.map((s, i) => row(s, i, laneStages.length))}
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
