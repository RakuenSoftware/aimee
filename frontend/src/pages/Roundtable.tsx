import { useCallback, useEffect, useState } from "react";
import { Button, Panel, InlineStatus } from "@rakuensoftware/smoothgui";

/* Roundtable page: configure the named multi-model review panels ("roundtables")
 * aimee convenes. A preset captures required positive seat bindings (a model +
 * persona), optional chairman, guard/loop knobs, and
 * authoring-pipeline caps.
 * Runtime fills all remaining eligible agent capacity; bindings never form an
 * exclusion list. Presets are
 * stored server-side (roundtable_preset.{c,h}); making one "active" mirrors its
 * values into the live ensemble/roundtable config the runtime reads. Several
 * named presets can coexist; one is the active default. */

async function getJSON<T>(url: string): Promise<T> {
  const r = await fetch(url, { headers: { "X-CSRF-Token": window._csrf || "" } });
  return (await r.json()) as T;
}
async function sendJSON<T>(
  method: "PUT" | "DELETE" | "POST",
  url: string,
  body?: unknown,
): Promise<{ status: number; data: T }> {
  const r = await fetch(url, {
    method,
    headers: { "Content-Type": "application/json", "X-CSRF-Token": window._csrf || "" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  let data = {} as T;
  try {
    data = (await r.json()) as T;
  } catch {
    /* empty bodies are fine */
  }
  return { status: r.status, data };
}

type Seat = { model: string; persona: string };
type Pipeline = {
  done_bar: string;
  max_passes: number;
  max_attempts_per_pass: number;
  max_cost_usd: number;
  max_total_cost_usd: number;
  gate_ttl_h: number;
  parked_releases_slot: boolean;
  unknown_context_tokens: number;
};
type Preset = {
  name: string;
  description: string;
  seats: Seat[];
  chairman: string;
  chairman_enabled: boolean;
  min_successful: number;
  max_cost_usd: number;
  deadline_ms: number;
  discussion: boolean;
  pipeline: Pipeline;
};
type PresetSummary = { name: string; description?: string; active?: boolean; synthesized?: boolean };
type Status = { kind: "ok" | "err"; msg: string } | null;

const lbl: React.CSSProperties = { fontSize: 12, color: "#666", display: "block", marginBottom: 2 };
const input: React.CSSProperties = {
  width: "100%",
  fontSize: 13,
  padding: 6,
  borderRadius: 6,
  border: "1px solid #ccc",
  boxSizing: "border-box",
};
const num: React.CSSProperties = { ...input, width: 120 };
const nameOk = (s: string) => /^[a-z0-9][a-z0-9._-]*$/i.test(s);

const DEFAULT_PIPELINE: Pipeline = {
  done_bar: "zero_blocking",
  max_passes: 0,
  max_attempts_per_pass: 2,
  max_cost_usd: 0,
  max_total_cost_usd: 0,
  gate_ttl_h: 0,
  parked_releases_slot: true,
  unknown_context_tokens: 0,
};

function emptyPreset(name: string): Preset {
  return {
    name,
    description: "",
    seats: [{ model: "", persona: "" }],
    chairman: "",
    chairman_enabled: false,
    min_successful: 2,
    max_cost_usd: 0,
    deadline_ms: 600000,
    discussion: false,
    pipeline: { ...DEFAULT_PIPELINE },
  };
}

/* Normalize a server preset (which may omit fields) into a fully-populated form
 * so every control is controlled. */
function normalize(p: Partial<Preset> & { name: string }): Preset {
  const base = emptyPreset(p.name);
  return {
    ...base,
    ...p,
    seats: Array.isArray(p.seats) && p.seats.length ? p.seats.map((s) => ({ model: s.model || "", persona: s.persona || "" })) : base.seats,
    pipeline: { ...base.pipeline, ...(p.pipeline || {}) },
  };
}

/* Roundtable-wide flags (not preset fields — see the `globals` state below).
 * Help text is grounded in their readers in src/modules/roundtable/
 * delegate_ensemble.c and src/modules/workflows/wfe_live_panel.c. */
const GLOBAL_FLAGS: { key: string; label: string; help: string }[] = [
  {
    key: "roundtable.require_evidence",
    label: "Require evidence",
    help: "Panelists must cite evidence for their findings; unevidenced items are dropped when the panel is verified.",
  },
  {
    key: "roundtable.replay_verify_enabled",
    label: "Replay-verify findings",
    help: "After a panel completes, replay its findings to verify them before they are reported.",
  },
  {
    key: "roundtable.chair_synthesis",
    label: "Chair synthesis",
    help: "The chair writes a synthesis pass over the panel's output instead of returning the raw per-seat results.",
  },
];

const numField = (v: number, set: (n: number) => void, min = 0): React.ReactNode => (
  <input
    type="number"
    min={min}
    step={1}
    style={num}
    value={Number.isFinite(v) ? v : 0}
    onChange={(e) => {
      const n = parseFloat(e.target.value);
      set(Number.isFinite(n) ? n : 0);
    }}
  />
);

export default function Roundtable() {
  const [status, setStatus] = useState<Status>(null);
  const [presets, setPresets] = useState<PresetSummary[]>([]);
  const [active, setActive] = useState<string>("");
  const [sel, setSel] = useState<string | null>(null);
  const [form, setForm] = useState<Preset | null>(null);
  const [models, setModels] = useState<string[]>([]);
  const [personas, setPersonas] = useState<string[]>([]);
  const [showAdvanced, setShowAdvanced] = useState(false);
  /* Global roundtable behaviour, migrated off the Settings page. These three are
   * the roundtable config keys that are NOT preset fields (preset_overlay_config
   * in src/modules/roundtable/roundtable_preset.c never writes them), so they
   * apply to every panel regardless of which preset is active — but they had no
   * owner in the GUI. They live here, next to the presets they govern. */
  const [globals, setGlobals] = useState<Record<string, boolean>>({});
  const [globalsBusy, setGlobalsBusy] = useState("");

  const refresh = useCallback(() => {
    getJSON<{ roundtables?: PresetSummary[]; active?: string }>("/api/roundtables")
      .then((d) => {
        setPresets(d.roundtables || []);
        setActive(d.active || "");
      })
      .catch(() => setPresets([]));
  }, []);

  const openPreset = useCallback((name: string) => {
    setSel(name);
    setForm(null);
    getJSON<Partial<Preset> & { name?: string }>(`/api/roundtables/${encodeURIComponent(name)}`)
      .then((d) => setForm(normalize({ ...d, name })))
      .catch(() => setForm(emptyPreset(name)));
  }, []);

  useEffect(() => {
    refresh();
    getJSON<{ models?: { name: string }[]; agents?: { name: string }[] }>("/api/models")
      .then((d) => setModels((d.models || d.agents || []).map((a) => a.name).filter(Boolean).sort()))
      .catch(() => setModels([]));
    getJSON<{ personas?: { name: string }[] }>("/api/chat/personas")
      .then((d) => setPersonas((d.personas || []).map((p) => p.name).filter(Boolean).sort()))
      .catch(() => setPersonas([]));
    getJSON<{ config?: Record<string, unknown> }>("/api/config")
      .then((d) => {
        const c = d.config || {};
        const out: Record<string, boolean> = {};
        for (const k of GLOBAL_FLAGS.map((f) => f.key)) out[k] = c[k] === true || c[k] === 1;
        setGlobals(out);
      })
      .catch(() => setGlobals({}));
  }, [refresh]);

  const setGlobal = async (key: string, next: boolean) => {
    setGlobalsBusy(key);
    const prev = globals[key];
    setGlobals((g) => ({ ...g, [key]: next })); // optimistic
    const { status: st } = await sendJSON("POST", "/api/config/set", { key, value: next });
    setGlobalsBusy("");
    if (st >= 200 && st < 300) {
      setStatus({ kind: "ok", msg: `${key} ${next ? "enabled" : "disabled"}` });
    } else {
      setGlobals((g) => ({ ...g, [key]: prev }));
      setStatus({ kind: "err", msg: `save failed (${st})` });
    }
  };

  const patch = (p: Partial<Preset>) => setForm((f) => (f ? { ...f, ...p } : f));
  const patchPipeline = (p: Partial<Pipeline>) =>
    setForm((f) => (f ? { ...f, pipeline: { ...f.pipeline, ...p } } : f));

  const setSeat = (i: number, s: Partial<Seat>) =>
    setForm((f) => {
      if (!f) return f;
      const seats = f.seats.slice();
      seats[i] = { ...seats[i], ...s };
      return { ...f, seats };
    });
  const addSeat = () => setForm((f) => (f ? { ...f, seats: [...f.seats, { model: "", persona: "" }] } : f));
  const removeSeat = (i: number) =>
    setForm((f) => (f ? { ...f, seats: f.seats.filter((_, j) => j !== i) } : f));

  const newPreset = () => {
    const name = window.prompt("New roundtable preset name (letters, digits, . - _):")?.trim();
    if (!name) return;
    if (!nameOk(name)) {
      setStatus({ kind: "err", msg: "invalid preset name" });
      return;
    }
    setSel(name);
    setForm(emptyPreset(name));
  };

  const save = async () => {
    if (!form) return;
    if (form.chairman_enabled && !form.chairman.trim()) {
      setStatus({ kind: "err", msg: "select a chairman before enabling final review" });
      return;
    }
    const seats = form.seats.filter((s) => s.model.trim());
    const body = { ...form, seats };
    const { status: st } = await sendJSON("PUT", `/api/roundtables/${encodeURIComponent(form.name)}`, body);
    if (st >= 200 && st < 300) {
      setStatus({ kind: "ok", msg: `preset “${form.name}” saved` });
      refresh();
    } else setStatus({ kind: "err", msg: `save failed (${st})` });
  };

  const del = async () => {
    if (!sel) return;
    if (!window.confirm(`Delete roundtable preset “${sel}”?`)) return;
    const { status: st } = await sendJSON("DELETE", `/api/roundtables/${encodeURIComponent(sel)}`);
    if (st >= 200 && st < 300) {
      setStatus({ kind: "ok", msg: `preset “${sel}” deleted` });
      setSel(null);
      setForm(null);
      refresh();
    } else setStatus({ kind: "err", msg: `delete failed (${st})` });
  };

  const makeActive = async () => {
    if (!form) return;
    // Persist first so activation mirrors the saved values, then activate.
    await save();
    const { status: st } = await sendJSON("POST", "/api/roundtables/active", { name: form.name });
    if (st >= 200 && st < 300) {
      setStatus({ kind: "ok", msg: `“${form.name}” is now the active roundtable` });
      refresh();
    } else setStatus({ kind: "err", msg: `activate failed (${st})` });
  };

  // A datalist lets a seat pick a configured agent/persona while still allowing a
  // free-typed value the list may not include.
  const modelList = "rt-models";
  const personaList = "rt-personas";
  // Sentinel: a seat set to RANDOM_MODEL is filled at runtime with any agent that
  // can serve the review role (retried until one is accepted). Kept in sync with
  // the server (RT_SEAT_RANDOM in roundtable_seat_resolve.h). A specific model is
  // instead HONORED exactly — if it cannot be fulfilled the workflow run fails.
  const RANDOM_MODEL = "$random";

  return (
    <div style={{ padding: 16, fontFamily: "system-ui", height: "100%", overflow: "auto" }}>
      <datalist id={modelList}>
        <option value={RANDOM_MODEL}>Random — any review-capable agent</option>
        {models.map((m) => (
          <option key={m} value={m} />
        ))}
      </datalist>
      <datalist id={personaList}>
        {personas.map((p) => (
          <option key={p} value={p} />
        ))}
      </datalist>

      <div style={{ display: "flex", alignItems: "center", gap: 10, marginBottom: 12 }}>
        <strong style={{ fontSize: 18 }}>Roundtable</strong>
        {active && <span style={{ fontSize: 12, color: "#666" }}>active: <code>{active}</code></span>}
        <InlineStatus status={status} />
      </div>

      <div style={{ maxWidth: 760 }}>
        <Panel title="Roundtable behaviour">
          <p style={{ fontSize: 12, color: "#666", margin: "0 0 8px" }}>
            These apply to every panel, whichever preset is active. (Seats, limits, and the pipeline
            caps are per preset — set those below.)
          </p>
          <div style={{ display: "grid", gap: 8 }}>
            {GLOBAL_FLAGS.map((f) => (
              <label key={f.key} style={{ display: "flex", alignItems: "flex-start", gap: 8, fontSize: 13 }}>
                <input
                  type="checkbox"
                  checked={!!globals[f.key]}
                  disabled={globalsBusy === f.key}
                  onChange={(e) => setGlobal(f.key, e.target.checked)}
                  style={{ marginTop: 2 }}
                />
                <span>
                  {f.label} <code style={{ color: "#aaa", fontSize: 11 }}>{f.key}</code>
                  <div style={{ fontSize: 12, color: "#777", lineHeight: 1.4 }}>{f.help}</div>
                </span>
              </label>
            ))}
          </div>
        </Panel>

        <Panel title="Presets" count={presets.length}>
          <p style={{ fontSize: 12, color: "#666", margin: "0 0 8px" }}>
            A roundtable is a panel of models, each playing a persona, that review or draft together. Configure
            several named presets and pick one as the active default — the active preset drives what{" "}
            <code>aimee delegate roundtable</code> convenes.
          </p>
          <p style={{ fontSize: 12, color: "#666", margin: "0 0 8px" }}>
            Roundtable policy is read-only to agents and automation. Creating, editing, deleting, or selecting the
            default requires an authenticated appliance-administrator action from this UI.
          </p>
          <div style={{ display: "flex", flexWrap: "wrap", gap: 6, marginBottom: 8 }}>
            {presets.map((p) => (
              <Button
                key={p.name}
                size="md"
                onClick={() => openPreset(p.name)}
                title={p.description || ""}
                style={{
                  background: sel === p.name ? "#e8eef9" : "#fff",
                  fontWeight: p.active ? 700 : 400,
                }}
              >
                <span style={{ display: "inline-flex", alignItems: "center", gap: 4 }}>
                  {p.active && (
                    <span aria-label="Default roundtable" title="Default roundtable" style={{ color: "#b26a00" }}>
                      ★
                    </span>
                  )}
                  <span>{p.name}</span>
                </span>
              </Button>
            ))}
            <Button size="md" onClick={newPreset} style={{ borderStyle: "dashed" }} title="Create a new roundtable preset, prompting for its name.">
              + New
            </Button>
          </div>

          {form && (
            <div>
              <label style={lbl} title="A short note describing what this panel is for.">Description</label>
              <input
                style={input}
                value={form.description}
                onChange={(e) => patch({ description: e.target.value })}
                placeholder="what this panel is for"
              />

              {/* Positive pins: required participants, never an exclusion list. */}
              <div style={{ marginTop: 14, marginBottom: 4, fontSize: 13, fontWeight: 600 }}>
                Required seats / positive pins ({form.seats.filter((s) => s.model.trim()).length})
              </div>
              <p style={{ fontSize: 12, color: "#666", margin: "0 0 8px" }}>
                Every configured seat is filled. Assignment spreads seats across providers and
                distinct models first, then reuses eligible models up to their parallel capacity
                until the table is full. A specific seat is a positive must-use pin; it never
                excludes other enabled, eligible agents. A <strong>specific</strong> model is
                honored exactly — if it can't be reached, the
                workflow run fails (never silently swapped). <strong>Random</strong> lets any
                review-capable agent fill the seat, retrying a different one until one is accepted.
              </p>
              <p style={{ fontSize: 12, color: "#666", margin: "0 0 8px" }}>
                Reviews always include an <strong>original-request alignment</strong> assessment.
                Direction drift or an unclear/missing assessment fails workflow gates closed;
                refinements that still advance the request remain aligned.
              </p>
              {form.seats.map((seat, i) => {
                const isRandom = seat.model === RANDOM_MODEL;
                return (
                <div key={i} style={{ display: "flex", gap: 6, alignItems: "center", marginBottom: 6 }}>
                  <input
                    list={modelList}
                    style={{ ...input, flex: 1, ...(isRandom ? { color: "#0a58ca", fontStyle: "italic" } : {}) }}
                    placeholder="model / agent (e.g. codex)"
                    title="Model or agent for this seat; type a value or pick a configured agent."
                    value={isRandom ? "Random — any review-capable" : seat.model}
                    readOnly={isRandom}
                    onChange={(e) => setSeat(i, { model: e.target.value })}
                  />
                  <Button
                    size="md"
                    onClick={() => setSeat(i, { model: isRandom ? "" : RANDOM_MODEL })}
                    style={{ padding: "4px 8px", background: isRandom ? "#e8eef9" : "#fff", fontWeight: isRandom ? 700 : 400 }}
                    title={isRandom ? "switch to a specific pinned model" : "let any review-capable agent fill this seat (retried until one is accepted)"}
                  >
                    🎲 Random
                  </Button>
                  <input
                    list={personaList}
                    style={{ ...input, flex: 1 }}
                    placeholder="persona (blank = engine default)"
                    title="Persona this seat reviews as; blank uses the engine default."
                    value={seat.persona}
                    onChange={(e) => setSeat(i, { persona: e.target.value })}
                  />
                  <Button
                    variant="danger"
                    size="md"
                    onClick={() => removeSeat(i)}
                    style={{ padding: "4px 8px" }}
                    title="remove seat"
                  >
                    ×
                  </Button>
                </div>
                );
              })}
              <Button size="md" onClick={addSeat} style={{ borderStyle: "dashed", marginBottom: 8 }} title="Require another model/persona participant without excluding automatic seats.">
                + Add required seat
              </Button>

              <div style={{ display: "flex", flexWrap: "wrap", gap: 16, marginTop: 10 }}>
                <div style={{ flex: "1 1 240px" }} title="Optional chairman that reviews deterministic synthesis and submits the final feedback.">
                  <label style={lbl}>Chairman</label>
                  <input
                    list={modelList}
                    style={input}
                    value={form.chairman}
                    onChange={(e) => patch({ chairman: e.target.value })}
                    placeholder="agent used when enabled"
                  />
                  {form.chairman_enabled && form.chairman !== RANDOM_MODEL && models.length > 0 && !models.includes(form.chairman) && (
                    <span style={{ fontSize: 11, color: "#9a6700" }}>
                      This name is not in the current configured-agent list; acquisition will park until it is eligible.
                    </span>
                  )}
                </div>
                <label style={{ ...lbl, alignSelf: "center", marginBottom: 0 }} title="After deterministic synthesis, require this chairman to review and submit final feedback.">
                  <input
                    type="checkbox"
                    checked={form.chairman_enabled}
                    onChange={(e) => patch({ chairman_enabled: e.target.checked })}
                  />{" "}
                  Chairman final review
                </label>
                <div title="Minimum number of seats that must succeed for the round to count.">
                  <label style={lbl}>Min successful</label>
                  {numField(form.min_successful, (n) => patch({ min_successful: n }), 1)}
                </div>
                <div title="Per-round cost ceiling in USD; 0 means no limit.">
                  <label style={lbl}>Max cost (USD, 0 = none)</label>
                  {numField(form.max_cost_usd, (n) => patch({ max_cost_usd: n }))}
                </div>
                <label style={{ ...lbl, alignSelf: "center", marginBottom: 0 }} title="After independent analysis, let the seated agents compare reports. Ordinary issues always stop after one cycle; only a disputed foundational issue can extend discussion until a strict majority forms.">
                  <input
                    type="checkbox"
                    checked={form.discussion}
                    onChange={(e) => patch({ discussion: e.target.checked })}
                  />{" "}
                  Discussion mode
                </label>
              </div>

              {/* Execution guard. Independent analysis always runs once in parallel. */}
              <div style={{ display: "flex", flexWrap: "wrap", gap: 16, marginTop: 10 }}>
                <div title="Overall time budget for the roundtable in milliseconds.">
                  <label style={lbl}>Deadline (ms)</label>
                  {numField(form.deadline_ms, (n) => patch({ deadline_ms: n }))}
                </div>
              </div>

              {/* Advanced: authoring-pipeline knobs. */}
              <Button
                size="md"
                onClick={() => setShowAdvanced((v) => !v)}
                style={{ marginTop: 12, background: "#f5f5f5" }}
                title="Show or hide the authoring-pipeline settings."
              >
                {showAdvanced ? "▾" : "▸"} Advanced — authoring pipeline
              </Button>
              {showAdvanced && (
                <div style={{ marginTop: 8, padding: 10, border: "1px solid #eee", borderRadius: 8 }}>
                  <p style={{ fontSize: 12, color: "#666", margin: "0 0 8px" }}>
                    The outer REVIEW↔revise loop for roundtable authoring runs (done-bar and cost/pass
                    backstops). Leave at defaults unless you run authoring pipelines.
                  </p>
                  <div style={{ display: "flex", flexWrap: "wrap", gap: 16 }}>
                    <div style={{ flex: "1 1 220px" }} title="Condition that marks an authoring pass complete.">
                      <label style={lbl}>Done bar</label>
                      <select
                        style={input}
                        value={form.pipeline.done_bar}
                        onChange={(e) => patchPipeline({ done_bar: e.target.value })}
                      >
                        <option value="zero_blocking">zero_blocking</option>
                        <option value="zero_blocking_suggestions">zero_blocking_suggestions</option>
                        <option value="zero_blocking_questions_answered">
                          zero_blocking_questions_answered
                        </option>
                      </select>
                    </div>
                    <div title="Maximum authoring passes; 0 means unlimited.">
                      <label style={lbl}>Max passes (0 = ∞)</label>
                      {numField(form.pipeline.max_passes, (n) => patchPipeline({ max_passes: n }))}
                    </div>
                    <div title="Maximum revise attempts within a single pass.">
                      <label style={lbl}>Attempts / pass</label>
                      {numField(form.pipeline.max_attempts_per_pass, (n) =>
                        patchPipeline({ max_attempts_per_pass: n }), 1)}
                    </div>
                    <div title="Cost ceiling for one authoring phase in USD.">
                      <label style={lbl}>Per-phase cost (USD)</label>
                      {numField(form.pipeline.max_cost_usd, (n) => patchPipeline({ max_cost_usd: n }))}
                    </div>
                    <div title="Cost ceiling for the whole authoring run in USD.">
                      <label style={lbl}>Total cost (USD)</label>
                      {numField(form.pipeline.max_total_cost_usd, (n) =>
                        patchPipeline({ max_total_cost_usd: n }))}
                    </div>
                    <div title="How long a review gate stays valid, in hours; 0 means none.">
                      <label style={lbl}>Gate TTL (h, 0 = none)</label>
                      {numField(form.pipeline.gate_ttl_h, (n) => patchPipeline({ gate_ttl_h: n }))}
                    </div>
                    <div title="Assumed token count when a document's context size is unknown.">
                      <label style={lbl}>Unknown ctx tokens</label>
                      {numField(form.pipeline.unknown_context_tokens, (n) =>
                        patchPipeline({ unknown_context_tokens: n }))}
                    </div>
                  </div>
                  <label style={{ ...lbl, marginTop: 8 }} title="When a gate is parked, free its active slot for other work.">
                    <input
                      type="checkbox"
                      checked={form.pipeline.parked_releases_slot}
                      onChange={(e) => patchPipeline({ parked_releases_slot: e.target.checked })}
                    />{" "}
                    Parked gate releases the active slot
                  </label>
                </div>
              )}

              <div style={{ display: "flex", gap: 8, marginTop: 14 }}>
                <Button size="md" onClick={save} title="Save this preset's settings.">
                  Save preset
                </Button>
                <Button variant="primary" size="md" onClick={makeActive} style={{ fontWeight: 600 }} title="Save this preset and make it the active default roundtable.">
                  Save &amp; set as default
                </Button>
                <Button variant="danger" size="md" onClick={del} title="Delete this roundtable preset.">
                  Delete
                </Button>
              </div>
            </div>
          )}
        </Panel>
      </div>
    </div>
  );
}
