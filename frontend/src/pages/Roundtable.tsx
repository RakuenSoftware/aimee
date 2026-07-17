import { useCallback, useEffect, useState } from "react";
import { Panel, InlineStatus } from "@rakuensoftware/smoothgui";

/* Roundtable page: configure the named multi-model review panels ("roundtables")
 * aimee convenes. A preset captures the seats (a model + a persona per seat), the
 * aggregator, the guard/loop knobs, and the authoring-pipeline caps. Presets are
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
  aggregator: string;
  min_successful: number;
  max_cost_usd: number;
  max_rounds: number;
  converge_threshold: number;
  deadline_ms: number;
  turns: string;
  pipeline: Pipeline;
};
type PresetSummary = { name: string; description?: string; active?: boolean; synthesized?: boolean };
type Status = { kind: "ok" | "err"; msg: string } | null;

const btn: React.CSSProperties = {
  padding: "4px 10px",
  fontSize: 13,
  borderRadius: 6,
  border: "1px solid #ccc",
  background: "#fff",
  cursor: "pointer",
};
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
    aggregator: "",
    min_successful: 2,
    max_cost_usd: 0,
    max_rounds: 3,
    converge_threshold: 2,
    deadline_ms: 360000,
    turns: "parallel",
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
    getJSON<{ agents?: { name: string }[] }>("/api/agents")
      .then((d) => setModels((d.agents || []).map((a) => a.name).filter(Boolean).sort()))
      .catch(() => setModels([]));
    getJSON<{ personas?: { name: string }[] }>("/api/chat/personas")
      .then((d) => setPersonas((d.personas || []).map((p) => p.name).filter(Boolean).sort()))
      .catch(() => setPersonas([]));
  }, [refresh]);

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
        <Panel title="Presets" count={presets.length}>
          <p style={{ fontSize: 12, color: "#666", margin: "0 0 8px" }}>
            A roundtable is a panel of models, each playing a persona, that review or draft together. Configure
            several named presets and pick one as the active default — the active preset drives what{" "}
            <code>aimee delegate roundtable</code> convenes.
          </p>
          <div style={{ display: "flex", flexWrap: "wrap", gap: 6, marginBottom: 8 }}>
            {presets.map((p) => (
              <button
                key={p.name}
                onClick={() => openPreset(p.name)}
                title={p.description || ""}
                style={{
                  ...btn,
                  background: sel === p.name ? "#e8eef9" : "#fff",
                  fontWeight: p.active ? 700 : 400,
                }}
              >
                {p.name}
                {p.active ? " ★" : ""}
              </button>
            ))}
            <button onClick={newPreset} style={{ ...btn, borderStyle: "dashed" }} title="Create a new roundtable preset, prompting for its name.">
              + New
            </button>
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

              {/* Seats: who is at the table. */}
              <div style={{ marginTop: 14, marginBottom: 4, fontSize: 13, fontWeight: 600 }}>
                Seats ({form.seats.length}) — a model + the persona it reviews as
              </div>
              <p style={{ fontSize: 12, color: "#666", margin: "0 0 8px" }}>
                A <strong>specific</strong> model is honored exactly — if it can't be reached, the
                workflow run fails (never silently swapped). <strong>Random</strong> lets any
                review-capable agent fill the seat, retrying a different one until one is accepted.
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
                  <button
                    onClick={() => setSeat(i, { model: isRandom ? "" : RANDOM_MODEL })}
                    style={{ ...btn, padding: "4px 8px", background: isRandom ? "#e8eef9" : "#fff", fontWeight: isRandom ? 700 : 400 }}
                    title={isRandom ? "switch to a specific pinned model" : "let any review-capable agent fill this seat (retried until one is accepted)"}
                  >
                    🎲 Random
                  </button>
                  <input
                    list={personaList}
                    style={{ ...input, flex: 1 }}
                    placeholder="persona (blank = engine default)"
                    title="Persona this seat reviews as; blank uses the engine default."
                    value={seat.persona}
                    onChange={(e) => setSeat(i, { persona: e.target.value })}
                  />
                  <button
                    onClick={() => removeSeat(i)}
                    style={{ ...btn, color: "#b00", padding: "4px 8px" }}
                    title="remove seat"
                  >
                    ×
                  </button>
                </div>
                );
              })}
              <button onClick={addSeat} style={{ ...btn, borderStyle: "dashed", marginBottom: 8 }} title="Add another model/persona seat to the panel.">
                + Add seat
              </button>

              {/* Aggregator + guards. */}
              <div style={{ display: "flex", flexWrap: "wrap", gap: 16, marginTop: 10 }}>
                <div style={{ flex: "1 1 240px" }} title="Model that synthesizes the panel's outputs; blank uses the engine default.">
                  <label style={lbl}>Aggregator (synthesis model)</label>
                  <input
                    list={modelList}
                    style={input}
                    value={form.aggregator}
                    onChange={(e) => patch({ aggregator: e.target.value })}
                    placeholder="blank = engine default"
                  />
                </div>
                <div title="Minimum number of seats that must succeed for the round to count.">
                  <label style={lbl}>Min successful</label>
                  {numField(form.min_successful, (n) => patch({ min_successful: n }), 1)}
                </div>
                <div title="Per-round cost ceiling in USD; 0 means no limit.">
                  <label style={lbl}>Max cost (USD, 0 = none)</label>
                  {numField(form.max_cost_usd, (n) => patch({ max_cost_usd: n }))}
                </div>
              </div>

              {/* Loop knobs. */}
              <div style={{ display: "flex", flexWrap: "wrap", gap: 16, marginTop: 10 }}>
                <div title="Maximum number of review/revise rounds.">
                  <label style={lbl}>Max rounds</label>
                  {numField(form.max_rounds, (n) => patch({ max_rounds: n }))}
                </div>
                <div title="Agreement level required to stop looping early.">
                  <label style={lbl}>Converge threshold</label>
                  {numField(form.converge_threshold, (n) => patch({ converge_threshold: n }))}
                </div>
                <div title="Overall time budget for the roundtable in milliseconds.">
                  <label style={lbl}>Deadline (ms)</label>
                  {numField(form.deadline_ms, (n) => patch({ deadline_ms: n }))}
                </div>
                <div title="Whether seats run in parallel or one after another.">
                  <label style={lbl}>Turns</label>
                  <select
                    style={{ ...input, width: 140 }}
                    value={form.turns}
                    onChange={(e) => patch({ turns: e.target.value })}
                  >
                    <option value="parallel">parallel</option>
                    <option value="sequential">sequential</option>
                  </select>
                </div>
              </div>

              {/* Advanced: authoring-pipeline knobs. */}
              <button
                onClick={() => setShowAdvanced((v) => !v)}
                style={{ ...btn, marginTop: 12, background: "#f5f5f5" }}
                title="Show or hide the authoring-pipeline settings."
              >
                {showAdvanced ? "▾" : "▸"} Advanced — authoring pipeline
              </button>
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
                <button onClick={save} style={btn} title="Save this preset's settings.">
                  Save preset
                </button>
                <button onClick={makeActive} style={{ ...btn, background: "#e8f7e8", fontWeight: 600 }} title="Save this preset and make it the active default roundtable.">
                  Save &amp; set as default
                </button>
                <button onClick={del} style={{ ...btn, color: "#b00" }} title="Delete this roundtable preset.">
                  Delete
                </button>
              </div>
            </div>
          )}
        </Panel>
      </div>
    </div>
  );
}
