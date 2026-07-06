import { useCallback, useEffect, useMemo, useState } from "react";
import { Panel, Badge } from "@rakuensoftware/smoothgui";
import { FIELD_HELP, SECTION_HELP, RESTART_KEYS } from "./settingsHelp";

/* Settings page: every typed Aimee config option (the config_fields allowlist,
 * e.g. typed_facts_enabled, kb_pdf_*, memory_*, autonomous). Values come from
 * GET /api/config (config.show); a change persists to aimee.yaml via POST
 * /api/config/set and takes effect on the next turn. The control is inferred
 * from the value's JSON type: boolean → toggle, number → number field, string →
 * text field. */

type Val = boolean | number | string;

async function getJSON<T>(url: string): Promise<T> {
  const r = await fetch(url, { headers: { "X-CSRF-Token": window._csrf || "" } });
  return (await r.json()) as T;
}
async function postJSON<T>(url: string, body: unknown): Promise<{ status: number; data: T }> {
  const r = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json", "X-CSRF-Token": window._csrf || "" },
    body: JSON.stringify(body),
  });
  let data = {} as T;
  try {
    data = (await r.json()) as T;
  } catch {
    /* empty */
  }
  return { status: r.status, data };
}

// Friendly label from a snake_case key: "typed_facts_enabled" → "Typed facts enabled".
function humanize(key: string): string {
  const s = key.replace(/_/g, " ").trim();
  return s.charAt(0).toUpperCase() + s.slice(1);
}

// Group a key into a settings section. Heuristic prefix map + an "Other" catch-all;
// the search box covers anything the grouping misses.
function category(key: string): string {
  const rules: [RegExp, string][] = [
    [/^kb_pdf/, "Knowledge — PDF ingest"],
    [/^(kb_|typed_facts)/, "Knowledge base"],
    [/^memory/, "Memory"],
    [/^(ingress|gateway|tool_output|code_span|context|fold|compact)/, "Gateway & context"],
    [/^(audit|governance|decision|guardrail)/, "Audit & governance"],
    [/^(provider|openai|anthropic|model|delegate|agent|roundtable)/, "Providers & delegates"],
    [/^(autonomous|cross_verify|ecomode|max_iterations|reasoning|verify|autopilot|trigger)/, "Agent behavior"],
    [/^(learning|intelligence|calibrat|bandit)/, "Learning & intelligence"],
    [/^(kb_curator|curator|synth|embed|rerank|extract|index)/, "Knowledge curation"],
  ];
  for (const [re, name] of rules) if (re.test(key)) return name;
  return "Other";
}

const btn: React.CSSProperties = {
  padding: "3px 10px",
  fontSize: 12,
  borderRadius: 6,
  border: "1px solid #ccc",
  background: "#fff",
  cursor: "pointer",
};
const input: React.CSSProperties = {
  fontFamily: "ui-monospace, monospace",
  fontSize: 12,
  padding: "3px 6px",
  borderRadius: 6,
  border: "1px solid #ccc",
  boxSizing: "border-box",
};

export default function Settings() {
  const [values, setValues] = useState<Record<string, Val>>({});
  const [draft, setDraft] = useState<Record<string, Val>>({});
  const [loaded, setLoaded] = useState(false);
  const [filter, setFilter] = useState("");
  const [status, setStatus] = useState<{ kind: "ok" | "err"; msg: string } | null>(null);

  const refresh = useCallback(() => {
    getJSON<{ config?: Record<string, Val> }>("/api/config")
      .then((d) => {
        setValues(d.config || {});
        setDraft(d.config || {});
        setLoaded(true);
      })
      .catch(() => setLoaded(true));
  }, []);

  useEffect(() => {
    refresh();
  }, [refresh]);

  const save = useCallback(
    async (key: string) => {
      const { status: st, data } = await postJSON<{ error?: string; notice?: string; value?: Val }>(
        "/api/config/set",
        { key, value: draft[key] },
      );
      if (st >= 200 && st < 300 && !data.error) {
        const v = data.value !== undefined ? data.value : draft[key];
        setValues((p) => ({ ...p, [key]: v }));
        setDraft((p) => ({ ...p, [key]: v }));
        setStatus({ kind: "ok", msg: data.notice ? data.notice : `${key} saved` });
      } else {
        setStatus({ kind: "err", msg: data.error || `save failed (${st})` });
      }
    },
    [draft],
  );

  // Group + filter the fields for rendering.
  const groups = useMemo(() => {
    const q = filter.trim().toLowerCase();
    const keys = Object.keys(values)
      .filter((k) => {
        if (!q) return true;
        const cat = category(k);
        // Match the key, its label, its help line, and its section (name + intro)
        // so a section-only term like "governance" or "curation" still finds rows.
        return (
          k.toLowerCase().includes(q) ||
          humanize(k).toLowerCase().includes(q) ||
          (FIELD_HELP[k] || "").toLowerCase().includes(q) ||
          cat.toLowerCase().includes(q) ||
          (SECTION_HELP[cat] || "").toLowerCase().includes(q)
        );
      })
      .sort();
    const byCat: Record<string, string[]> = {};
    for (const k of keys) (byCat[category(k)] ||= []).push(k);
    return Object.entries(byCat).sort(([a], [b]) => a.localeCompare(b));
  }, [values, filter]);

  return (
    <div style={{ padding: 16, fontFamily: "system-ui", height: "100%", overflow: "auto" }}>
      <div style={{ display: "flex", alignItems: "center", gap: 10, marginBottom: 12, flexWrap: "wrap" }}>
        <strong style={{ fontSize: 18 }}>Settings</strong>
        <Badge label={`${Object.keys(values).length}`} variant="neutral" />
        <input
          placeholder="filter settings…"
          value={filter}
          onChange={(e) => setFilter(e.target.value)}
          style={{ ...input, fontFamily: "system-ui", minWidth: 220 }}
        />
        <button onClick={refresh} style={btn}>
          Reload
        </button>
        {status && (
          <span
            style={{
              fontSize: 12,
              color: status.kind === "err" ? "#b00" : "#080",
              maxWidth: 620,
            }}
          >
            {status.msg}
          </span>
        )}
      </div>
      <p style={{ fontSize: 12, color: "#666", margin: "0 0 12px" }}>
        Changes persist to <code>aimee.yaml</code> and take effect on the next turn, unless a row is
        marked <em>restart</em>. Most options are off by default; each is described below.
      </p>

      {!loaded && <div style={{ color: "#888" }}>loading…</div>}
      {loaded && Object.keys(values).length === 0 && (
        <div style={{ color: "#888" }}>No configurable options available (aimee-server unreachable?).</div>
      )}

      <div style={{ display: "grid", gap: 12 }}>
        {groups.map(([cat, keys]) => (
          <Panel key={cat} title={cat} count={keys.length}>
            {SECTION_HELP[cat] && (
              <p style={{ fontSize: 12, color: "#777", margin: "0 0 10px", lineHeight: 1.4 }}>
                {SECTION_HELP[cat]}
              </p>
            )}
            <div style={{ display: "grid", gap: 10 }}>
              {keys.map((k) => (
                <SettingRow
                  key={k}
                  fieldKey={k}
                  value={draft[k]}
                  dirty={draft[k] !== values[k]}
                  onChange={(v) => setDraft((p) => ({ ...p, [k]: v }))}
                  onSave={() => save(k)}
                  onReset={() => setDraft((p) => ({ ...p, [k]: values[k] }))}
                />
              ))}
            </div>
          </Panel>
        ))}
      </div>
    </div>
  );
}

function SettingRow({
  fieldKey,
  value,
  dirty,
  onChange,
  onSave,
  onReset,
}: {
  fieldKey: string;
  value: Val;
  dirty: boolean;
  onChange: (v: Val) => void;
  onSave: () => void;
  onReset: () => void;
}) {
  const help = FIELD_HELP[fieldKey];
  const needsRestart = RESTART_KEYS.has(fieldKey);
  return (
    <div style={{ display: "flex", alignItems: "flex-start", gap: 8, flexWrap: "wrap" }}>
      <div style={{ flex: "1 1 300px", minWidth: 220 }}>
        <label style={{ fontSize: 13, display: "flex", alignItems: "center", gap: 6, flexWrap: "wrap" }} title={fieldKey}>
          {humanize(fieldKey)}
          <span style={{ color: "#aaa", fontSize: 11 }}>{fieldKey}</span>
          {needsRestart && (
            <span
              style={{
                fontSize: 10,
                color: "#a60",
                background: "#fff6e6",
                border: "1px solid #f0d9a8",
                borderRadius: 4,
                padding: "0 5px",
              }}
              title="Takes effect only after the server restarts."
            >
              restart
            </span>
          )}
        </label>
        {help && (
          <div style={{ fontSize: 12, color: "#777", marginTop: 2, lineHeight: 1.4 }}>{help}</div>
        )}
      </div>
      <div style={{ flex: "0 0 auto", display: "flex", alignItems: "center", gap: 6, marginTop: 1 }}>
        {typeof value === "boolean" ? (
          <button
            onClick={() => onChange(!value)}
            style={{
              ...btn,
              minWidth: 44,
              background: value ? "#1f7a3d" : "#fff",
              color: value ? "#fff" : "#555",
            }}
          >
            {value ? "on" : "off"}
          </button>
        ) : typeof value === "number" ? (
          <input
            type="number"
            value={value}
            onChange={(e) => onChange(e.target.value === "" ? 0 : Number(e.target.value))}
            style={{ ...input, width: 120 }}
          />
        ) : (
          <input
            value={value ?? ""}
            onChange={(e) => onChange(e.target.value)}
            style={{ ...input, width: 220, fontFamily: "system-ui" }}
          />
        )}
        {dirty && (
          <>
            <button onClick={onSave} style={{ ...btn, borderColor: "#2563eb", color: "#2563eb" }}>
              save
            </button>
            <button onClick={onReset} style={btn} title="discard change">
              ↺
            </button>
          </>
        )}
      </div>
    </div>
  );
}
