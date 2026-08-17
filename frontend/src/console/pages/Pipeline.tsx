import { useCallback, useEffect, useState } from "react";
import { Button, InlineStatus, Switch } from "@rakuensoftware/smoothgui";
import { apiGet, apiSend } from "../api";
// Shared help text for the stage config keys. Data-only module (no components,
// no webchat imports), so the console reads the same descriptions the aimee
// Settings page shows rather than keeping a second copy in sync.
import { FIELD_HELP } from "../../pages/settingsHelp";

/* Pipeline page: the curator pipeline as an ordered, resource-lane-grouped view.
 * Lives in the kb console because the kb owns the curator — GET
 * /v1/console/pipeline serves the stage registry, the presets, and the current
 * config in one call, straight out of the kb process.
 *
 * Option B (single source of truth): the stage list, order, lanes, config key, and
 * dependency `requires` come LIVE from the backend registry (CURATOR_STAGES + the
 * projection sweeps in src/kb/kb_curator_drain.c). Toggles and the stage order
 * persist via POST /v1/console/pipeline/config, which allowlists the pipeline's
 * own keys and writes aimee.yaml (the curator picks it up on next config load).
 * A null config_key = embedder-gated (read-only).
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
  // Phase D — composed custom stages. `custom` marks a user-defined stage that
  // recomposes `base_op` (a built-in run()-backed stage) on that op's lane;
  // `enabled` is its own on/off in the custom_stages config. `base_op_eligible`
  // marks a built-in that may be used as a base_op (the two sweep pseudo-stages
  // are not).
  custom?: boolean;
  base_op?: string;
  enabled?: boolean;
  base_op_eligible?: boolean;
};
type Preset = { name: string; description: string; enabled: string[]; builtin?: boolean };
// One entry in the kb_curator_custom_stages JSON array (what the GUI writes).
type CustomStageCfg = { name: string; base_op: string; budget?: number; enabled?: boolean };

// Set one pipeline config key. Returns an error string, or "" on success — the
// callers below all report failure inline rather than throwing to the boundary.
async function setKey(key: string, value: Val): Promise<string> {
  try {
    await apiSend("POST", "/v1/console/pipeline/config", { key, value });
    return "";
  } catch (e) {
    return String(e);
  }
}

const LANE_META: Record<Lane, { title: string; hint: string; color: string }> = {
  llm: { title: "LLM lane · GPU", hint: "One unit per pass — extraction/reasoning on the GPU model.", color: "var(--sg-primary)" },
  index: { title: "Index lane · CPU", hint: "Drains its queue each pass — embedding + SQL, concurrent with the GPU lane.", color: "var(--sg-success)" },
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
    apiGet<{ stages?: Stage[]; presets?: Preset[]; config?: Record<string, Val> }>("/v1/console/pipeline")
      .then((d) => {
        const c = d.config || {};
        const orderStr = typeof c.kb_curator_stage_order === "string" ? c.kb_curator_stage_order : "";
        setStages(applyOrder(d.stages || [], orderStr));
        setPresets(d.presets || []);
        setCfg(c);
        setLoaded(true);
      })
      .catch((e) => {
        setStatus({ kind: "err", msg: `Failed to load the pipeline: ${e}` });
        setLoaded(true);
      });
  }, []);
  useEffect(() => { refresh(); }, [refresh]);

  const toggle = useCallback(async (key: string, next: boolean) => {
    setBusy(key);
    const err = await setKey(key, next);
    setBusy("");
    if (err) {
      setStatus({ kind: "err", msg: err });
      return;
    }
    setCfg((p) => ({ ...p, [key]: next }));
    setStatus({ kind: "ok", msg: `${key} ${next ? "enabled" : "disabled"}` });
  }, []);

  // Apply a preset: enable its listed config keys, disable every other toggleable
  // stage. One set per stage, in parallel; embedder-gated stages untouched.
  const applyPreset = useCallback(async (p: Preset) => {
    setBusy(`preset:${p.name}`);
    const on = new Set(p.enabled);
    const targets = stages.filter((s) => s.config_key);
    const errs = await Promise.all(
      targets.map((s) => setKey(s.config_key as string, on.has(s.config_key as string))),
    );
    setBusy("");
    setCfg((prev) => {
      const next = { ...prev };
      for (const s of targets) next[s.config_key as string] = on.has(s.config_key as string);
      return next;
    });
    const failed = errs.filter(Boolean).length;
    setStatus(
      failed
        ? { kind: "err", msg: `preset "${p.name}" applied with ${failed} error(s)` }
        : { kind: "ok", msg: `preset "${p.name}" applied` },
    );
  }, [stages]);

  // User presets are stored as a JSON array in the kb_curator_user_presets config
  // string; the GUI reads/edits it and persists it back (no bespoke op).
  const readUserPresets = useCallback((): Preset[] => {
    const raw = cfg.kb_curator_user_presets;
    if (typeof raw !== "string" || !raw) return [];
    try {
      const arr = JSON.parse(raw);
      return Array.isArray(arr) ? (arr as Preset[]) : [];
    } catch {
      return [];
    }
  }, [cfg]);

  const writeUserPresets = useCallback(async (list: Preset[], okMsg: string) => {
    setBusy("user-presets");
    const err = await setKey("kb_curator_user_presets", JSON.stringify(list));
    setBusy("");
    if (err) {
      setStatus({ kind: "err", msg: err });
      return;
    }
    setStatus({ kind: "ok", msg: okMsg });
    refresh();
  }, [refresh]);

  const saveCurrentAsPreset = useCallback(async () => {
    const name = window.prompt("Save the current stage toggles as a preset named:")?.trim();
    if (!name) return;
    const enabled = stages
      .filter((s) => s.config_key && (cfg[s.config_key] === true || cfg[s.config_key] === 1))
      .map((s) => s.config_key as string);
    const list = readUserPresets().filter((p) => p.name !== name);
    list.push({ name, description: "User preset", enabled, builtin: false });
    await writeUserPresets(list, `preset "${name}" saved`);
  }, [stages, cfg, readUserPresets, writeUserPresets]);

  const deletePreset = useCallback(async (name: string) => {
    const list = readUserPresets().filter((p) => p.name !== name);
    await writeUserPresets(list, `preset "${name}" deleted`);
  }, [readUserPresets, writeUserPresets]);

  // Composed custom stages (Phase D) live as a JSON array in the
  // kb_curator_custom_stages config string; like user presets, the GUI reads/edits
  // it and persists it back. The backend validates each entry (base_op must be a
  // built-in run() stage; runs on that op's lane) and surfaces the accepted ones
  // on /v1/console/pipeline with custom:true.
  const readCustomStages = useCallback((): CustomStageCfg[] => {
    const raw = cfg.kb_curator_custom_stages;
    if (typeof raw !== "string" || !raw) return [];
    try {
      const arr = JSON.parse(raw);
      return Array.isArray(arr) ? (arr as CustomStageCfg[]) : [];
    } catch {
      return [];
    }
  }, [cfg]);

  const writeCustomStages = useCallback(async (list: CustomStageCfg[], okMsg: string) => {
    setBusy("custom-stages");
    const err = await setKey("kb_curator_custom_stages", JSON.stringify(list));
    setBusy("");
    if (err) {
      setStatus({ kind: "err", msg: err });
      return;
    }
    setStatus({ kind: "ok", msg: okMsg });
    refresh();
  }, [refresh]);

  const addCustomStage = useCallback(async () => {
    // Valid base ops = built-in run()-backed stages the backend marks eligible.
    const baseNames = stages.filter((s) => !s.custom && s.base_op_eligible).map((s) => s.name);
    const name = window.prompt("New custom stage name (letters, digits, _ or -):")?.trim();
    if (!name) return;
    if (!/^[A-Za-z0-9_-]{1,63}$/.test(name)) {
      setStatus({ kind: "err", msg: "name must be 1–63 chars of [A-Za-z0-9_-]" });
      return;
    }
    if (stages.some((s) => s.name === name)) {
      setStatus({ kind: "err", msg: `"${name}" collides with an existing stage` });
      return;
    }
    const baseOp = window.prompt(`Base op to recompose — one of:\n${baseNames.join(", ")}`)?.trim();
    if (!baseOp) return;
    if (!baseNames.includes(baseOp)) {
      setStatus({ kind: "err", msg: `base op must be one of: ${baseNames.join(", ")}` });
      return;
    }
    const budgetStr = window.prompt("Per-pass budget (blank = base op default):")?.trim();
    const entry: CustomStageCfg = { name, base_op: baseOp, enabled: true };
    if (budgetStr) {
      const b = Number(budgetStr);
      if (!Number.isFinite(b) || b < 1) {
        setStatus({ kind: "err", msg: "budget must be a number ≥ 1" });
        return;
      }
      entry.budget = Math.min(Math.floor(b), 65536);
    }
    const list = readCustomStages().filter((c) => c.name !== name);
    list.push(entry);
    await writeCustomStages(list, `custom stage "${name}" added (runs on ${baseOp}'s lane)`);
  }, [stages, readCustomStages, writeCustomStages]);

  const toggleCustomStage = useCallback(async (name: string, next: boolean) => {
    const list = readCustomStages().map((c) => (c.name === name ? { ...c, enabled: next } : c));
    await writeCustomStages(list, `custom stage "${name}" ${next ? "enabled" : "disabled"}`);
  }, [readCustomStages, writeCustomStages]);

  const deleteCustomStage = useCallback(async (name: string) => {
    const list = readCustomStages().filter((c) => c.name !== name);
    await writeCustomStages(list, `custom stage "${name}" deleted`);
  }, [readCustomStages, writeCustomStages]);

  const persistOrder = useCallback(async (arr: Stage[]) => {
    // Only built-ins participate in stage_order; custom stages append after them.
    const order = arr.filter((s) => !s.custom).map((s) => s.name).join(",");
    setBusy("order");
    const err = await setKey("kb_curator_stage_order", order);
    setBusy("");
    if (err) {
      setStatus({ kind: "err", msg: err });
      return;
    }
    setCfg((p) => ({ ...p, kb_curator_stage_order: order }));
    setStatus({ kind: "ok", msg: "stage order saved" });
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

  /* Curator runtime knobs, migrated off the aimee Settings page. The tier is a
   * preset over the stage toggles (the backend's kb_curator_apply_tier rewrites
   * every stage enable flag from it), so a change here refetches the whole page
   * rather than patching one row. The worker counts are the extract stages'
   * concurrency. */
  const setRuntimeKey = useCallback(async (key: string, value: Val, msg: string) => {
    setBusy(key);
    const err = await setKey(key, value);
    setBusy("");
    if (err) {
      setStatus({ kind: "err", msg: err });
      return;
    }
    setStatus({ kind: "ok", msg });
    refresh();
  }, [refresh]);

  const tier = typeof cfg.kb_curator_tier === "string" ? cfg.kb_curator_tier : "full";
  const workerCount = (k: string) => (typeof cfg[k] === "number" ? (cfg[k] as number) : 1);

  const arrowBtn = (enabled: boolean, glyph: string, title: string, onClick: () => void) => (
    <button
      disabled={!enabled || busy === "order"}
      onClick={onClick}
      title={title}
      style={{
        width: 22, height: 20, fontSize: 11, lineHeight: "18px", padding: 0, borderRadius: 4,
        border: "1px solid var(--sg-border-medium)", cursor: enabled ? "pointer" : "default",
        background: "var(--sg-surface)", color: enabled ? "var(--sg-text-muted)" : "var(--sg-border-medium)",
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
        borderBottom: "1px solid var(--sg-border-light)", opacity: gated ? 0.75 : 1,
      }}>
        <span style={{ display: "flex", flexDirection: "column", gap: 2 }}>
          {arrowBtn(i > 0, "▲", "Move earlier in this lane", () => move(s.name, -1))}
          {arrowBtn(i < laneCount - 1, "▼", "Move later in this lane", () => move(s.name, 1))}
        </span>
        <span style={{ width: 18, color: "var(--sg-text-pale)", fontSize: 12, fontFamily: "ui-monospace, monospace" }}>{i + 1}</span>
        <div style={{ flex: 1, minWidth: 0 }}>
          <div style={{ fontWeight: 600, fontSize: 14 }}>{s.label}
            {gated && <span style={{ marginLeft: 8, fontSize: 11, color: "var(--sg-text-hint)" }}>(embedder-gated)</span>}
            {s.requires.length > 0 && (
              <span style={{ marginLeft: 8, fontSize: 11, color: "var(--sg-text-pale)" }}>after: {s.requires.join(", ")}</span>
            )}
          </div>
          <div style={{ fontSize: 12, color: "var(--sg-text-faint)" }}>{desc}</div>
        </div>
        {gated ? (
          <span style={{ fontSize: 12, color: "var(--sg-success)", whiteSpace: "nowrap" }}>● active</span>
        ) : (
          <span title={on ? "Disable stage" : "Enable stage"} style={{ display: "inline-flex" }}>
            <Switch
              checked={on}
              onChange={(next) => toggle(s.config_key as string, next)}
              disabled={busy === s.config_key}
              ariaLabel={on ? "Disable stage" : "Enable stage"}
            />
          </span>
        )}
      </div>
    );
  };

  if (!loaded) return <div style={{ padding: 24, color: "var(--sg-text-faint)" }}>Loading pipeline…</div>;

  return (
    <div style={{ padding: "18px 24px", maxWidth: 860, margin: "0 auto", fontFamily: "system-ui" }}>
      <h2 style={{ margin: "0 0 4px" }}>Curator pipeline</h2>
      <p style={{ color: "var(--sg-text-faint)", fontSize: 13, margin: "0 0 18px" }}>
        Ingested content flows through these stages into curated knowledge (claims, entities,
        contradictions, graph). Toggle a stage to include/exclude it, or reorder within a lane with ▲▼
        (dependencies are enforced). Changes persist to aimee.yaml and take effect on the KB's next
        config load. This view is generated live from the backend stage registry.
      </p>
      <div style={{ display: "flex", alignItems: "center", gap: 8, flexWrap: "wrap", marginBottom: 18 }}>
        <span style={{ fontSize: 13, fontWeight: 600, color: "var(--sg-text-muted)" }}>Presets:</span>
        {presets.map((p) => (
          <span key={p.name} style={{ display: "inline-flex", alignItems: "center" }}>
            <button
              disabled={busy === `preset:${p.name}`}
              onClick={() => applyPreset(p)}
              title={p.description}
              style={{
                padding: "5px 12px", fontSize: 13, cursor: "pointer", border: "1px solid var(--sg-border-medium)",
                borderRadius: p.builtin === false ? "14px 0 0 14px" : 14,
                background: busy === `preset:${p.name}` ? "var(--sg-surface-sunken)" : "var(--sg-surface-alt)",
              }}
            >
              {p.name}
            </button>
            {p.builtin === false && (
              <button
                disabled={busy === "user-presets"}
                onClick={() => deletePreset(p.name)}
                title={`Delete preset "${p.name}"`}
                style={{
                  padding: "5px 8px", fontSize: 13, borderRadius: "0 14px 14px 0", cursor: "pointer",
                  border: "1px solid var(--sg-border-medium)", borderLeft: "none", background: "var(--sg-surface-alt)", color: "var(--sg-danger-dark)",
                }}
              >
                ×
              </button>
            )}
          </span>
        ))}
        <button
          disabled={busy === "user-presets"}
          onClick={saveCurrentAsPreset}
          title="Save the current stage toggles as a named preset"
          style={{
            padding: "5px 12px", fontSize: 13, borderRadius: 14, cursor: "pointer",
            border: "1px dashed var(--sg-border-medium)", background: "var(--sg-surface)", color: "var(--sg-text-muted)",
          }}
        >
          ＋ Save current
        </button>
        <span style={{ fontSize: 12, color: "var(--sg-text-hint)" }}>apply a profile, save your own, then fine-tune below</span>
      </div>
      <div style={{
        display: "flex", alignItems: "center", gap: 14, flexWrap: "wrap", marginBottom: 18,
        padding: "10px 12px", border: "1px solid var(--sg-border)", borderRadius: 8, background: "var(--sg-surface-alt)",
      }}>
        <label style={{ fontSize: 13, display: "flex", alignItems: "center", gap: 6 }}
          title="Stage preset: 'full' runs every stage, 'lite' only the core extract/index stages, 'off' none. Selecting one rewrites every stage toggle below.">
          <span style={{ fontWeight: 600, color: "var(--sg-text-muted)" }}>Tier</span>
          <select
            value={tier}
            disabled={busy === "kb_curator_tier"}
            onChange={(e) => setRuntimeKey("kb_curator_tier", e.target.value, `tier set to ${e.target.value}`)}
            style={{ fontSize: 13, padding: "3px 6px", borderRadius: 6, border: "1px solid var(--sg-border-medium)" }}
          >
            {["full", "lite", "off"].map((t) => <option key={t} value={t}>{t}</option>)}
          </select>
        </label>
        <span style={{ fontSize: 12, color: "var(--sg-text-hint)" }}>rewrites every stage toggle below</span>
        {[
          { key: "kb_curator_extract_docs_workers", label: "Doc extract workers" },
          { key: "kb_curator_extract_code_workers", label: "Code extract workers" },
        ].map((w) => (
          <label key={w.key} style={{ fontSize: 13, display: "flex", alignItems: "center", gap: 6 }}
            title="Concurrent workers the drain runs for this extract stage. 1 = no extra workers.">
            <span style={{ fontWeight: 600, color: "var(--sg-text-muted)" }}>{w.label}</span>
            <input
              type="number"
              min={1}
              value={workerCount(w.key)}
              disabled={busy === w.key}
              onChange={(e) => {
                const n = Math.max(1, Math.floor(Number(e.target.value) || 1));
                setRuntimeKey(w.key, n, `${w.label.toLowerCase()} set to ${n}`);
              }}
              style={{ width: 64, fontSize: 13, padding: "3px 6px", borderRadius: 6, border: "1px solid var(--sg-border-medium)" }}
            />
          </label>
        ))}
      </div>
      {(["llm", "index"] as Lane[]).map((lane) => {
        const meta = LANE_META[lane];
        // Built-in stages only — custom stages append after the built-ins and are
        // managed in their own panel below (they don't participate in reorder).
        const laneStages = stages.filter((s) => s.lane === lane && !s.custom);
        if (!laneStages.length) return null;
        return (
          <div key={lane} style={{ marginBottom: 22, border: "1px solid var(--sg-border)", borderRadius: 8, overflow: "hidden" }}>
            <div style={{ padding: "8px 12px", background: "var(--sg-surface-alt)", borderBottom: "1px solid var(--sg-border)" }}>
              <span style={{ fontWeight: 700, color: meta.color }}>{meta.title}</span>
              <span style={{ marginLeft: 10, fontSize: 12, color: "var(--sg-text-hint)" }}>{meta.hint}</span>
            </div>
            {laneStages.map((s, i) => row(s, i, laneStages.length))}
          </div>
        );
      })}
      {(() => {
        const customs = stages.filter((s) => s.custom);
        return (
          <div style={{ marginBottom: 22, border: "1px dashed var(--sg-border-medium)", borderRadius: 8, overflow: "hidden" }}>
            <div style={{ padding: "8px 12px", background: "var(--sg-surface-alt)", borderBottom: "1px solid var(--sg-border)", display: "flex", alignItems: "center", gap: 10, flexWrap: "wrap" }}>
              <span style={{ fontWeight: 700, color: "var(--sg-purple)" }}>Custom stages</span>
              <span style={{ fontSize: 12, color: "var(--sg-text-hint)", flex: 1, minWidth: 160 }}>
                Recompose a built-in op under a new name/budget. Runs on the base op's lane (re-laning is not supported).
              </span>
              <button
                disabled={busy === "custom-stages"}
                onClick={addCustomStage}
                title="Add a custom stage that recomposes a built-in op"
                style={{
                  padding: "5px 12px", fontSize: 13, borderRadius: 14, cursor: "pointer",
                  border: "1px dashed var(--sg-border-medium)", background: "var(--sg-surface)", color: "var(--sg-text-muted)",
                }}
              >
                ＋ Add custom stage
              </button>
            </div>
            {customs.length === 0 ? (
              <div style={{ padding: "10px 12px", fontSize: 12, color: "var(--sg-text-hint)" }}>
                None yet — add one to run an existing op a second time (e.g. a larger-budget pass) under its own name.
              </div>
            ) : (
              customs.map((s) => {
                const on = s.enabled !== false;
                return (
                  <div key={s.name} style={{
                    display: "flex", alignItems: "center", gap: 12, padding: "8px 12px",
                    borderBottom: "1px solid var(--sg-border-light)",
                  }}>
                    <div style={{ flex: 1, minWidth: 0 }}>
                      <div style={{ fontWeight: 600, fontSize: 14 }}>{s.label}
                        <span style={{ marginLeft: 8, fontSize: 11, color: "var(--sg-purple)" }}>custom</span>
                        <span style={{ marginLeft: 8, fontSize: 11, color: "var(--sg-text-pale)" }}>
                          {s.base_op} · {s.lane} lane · budget {s.budget}
                        </span>
                      </div>
                      <div style={{ fontSize: 12, color: "var(--sg-text-faint)" }}>Recomposes {s.base_op} on the {s.lane} lane.</div>
                    </div>
                    <span title={on ? "Disable stage" : "Enable stage"} style={{ display: "inline-flex" }}>
                      <Switch
                        checked={on}
                        onChange={(next) => toggleCustomStage(s.name, next)}
                        disabled={busy === "custom-stages"}
                        ariaLabel={on ? "Disable stage" : "Enable stage"}
                      />
                    </span>
                    <Button
                      variant="danger"
                      size="md"
                      disabled={busy === "custom-stages"}
                      onClick={() => deleteCustomStage(s.name)}
                      title={`Delete custom stage "${s.name}"`}
                      style={{ padding: "5px 8px" }}
                    >
                      ×
                    </Button>
                  </div>
                );
              })
            )}
          </div>
        );
      })()}
      {!stages.length && (
        <div style={{ color: "var(--sg-text-faint)" }}>No stages reported (the kb has no curator registry, or the request failed).</div>
      )}
      {status && (
        <div style={{ marginTop: 8 }}>
          <InlineStatus status={status} />
        </div>
      )}
    </div>
  );
}
