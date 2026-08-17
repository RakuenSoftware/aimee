import { useCallback, useEffect, useMemo, useState } from "react";
import { Panel, Badge, InlineStatus, Button } from "@rakuensoftware/smoothgui";
import { FIELD_HELP, SECTION_HELP, RESTART_KEYS, OWNED_ELSEWHERE } from "./settingsHelp";
import { resetAll as resetTutorials } from "../help/tutorialState";
import { setDismissed as setSetupDismissed, requestOpenWizard } from "../setup/setupState";

/* Settings page: every typed Aimee config option (the config_fields allowlist,
 * e.g. typed_facts_enabled, kb_pdf_*, memory_*, autonomous). Values come from
 * GET /api/config (config.show); a change persists to aimee.yaml via POST
 * /api/config/set and takes effect on the next turn. The control is inferred
 * from the value's JSON type: boolean → toggle, number → number field, string →
 * text field.
 *
 * Keys the /api/settings allowlist declares as enums (kb_fusion_mode) render as
 * a dropdown instead of free text, and save through /api/settings so its per-key
 * validation still applies.
 *
 * This page lists the options NO other tab owns. Anything in OWNED_ELSEWHERE is
 * configured by the tab that has the context to set it safely (see the notes on
 * that map); the section header names that tab so the option is still findable.
 * The key stays fully settable from aimee.yaml and the CLI either way. */

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
    [/^(provider|openai|anthropic|model|delegate|agent|roundtable|default_persona|persona)/, "Providers & delegates"],
    [/^(autonomous|cross_verify|max_iterations|reasoning|verify|autopilot|trigger)/, "Agent behavior"],
    [/^(learning|intelligence|calibrat|bandit)/, "Learning & intelligence"],
    [/^(kb_curator|curator|synth|embed|extract|index)/, "Knowledge curation"],
  ];
  for (const [re, name] of rules) if (re.test(key)) return name;
  return "Other";
}

const input: React.CSSProperties = {
  fontFamily: "ui-monospace, monospace",
  fontSize: 12,
  padding: "3px 6px",
  borderRadius: 6,
  border: "1px solid var(--sg-border-medium)",
  boxSizing: "border-box",
};

export default function Settings() {
  const [values, setValues] = useState<Record<string, Val>>({});
  const [draft, setDraft] = useState<Record<string, Val>>({});
  // Surface group per NON-runtime key ("deploy" | "advanced" | "dev"), as advertised
  // by config.show. Runtime keys are absent from this map. Drives the default-hidden
  // "advanced" surface so the everyday page shows only the operator-facing options.
  const [fieldGroups, setFieldGroups] = useState<Record<string, string>>({});
  // key -> allowed values, for the config keys /api/settings declares as enums.
  const [enumOptions, setEnumOptions] = useState<Record<string, string[]>>({});
  const [showAdvanced, setShowAdvanced] = useState(false);
  const [loaded, setLoaded] = useState(false);
  const [filter, setFilter] = useState("");
  const [status, setStatus] = useState<{ kind: "ok" | "err"; msg: string } | null>(null);

  const refresh = useCallback(() => {
    getJSON<{ config?: Record<string, Val>; groups?: Record<string, string> }>("/api/config")
      .then((d) => {
        setValues(d.config || {});
        setDraft(d.config || {});
        setFieldGroups(d.groups || {});
        setLoaded(true);
      })
      .catch(() => setLoaded(true));
    // Enum options are advisory chrome: on failure the affected rows just fall
    // back to a text input rather than blocking the page.
    getJSON<{ fields?: { key: string; type: string; options?: string[] }[] }>("/api/settings")
      .then((d) => {
        const opts: Record<string, string[]> = {};
        for (const f of d.fields || []) {
          if (f.type === "enum" && f.options?.length) opts[f.key] = f.options;
        }
        setEnumOptions(opts);
      })
      .catch(() => setEnumOptions({}));
  }, []);

  useEffect(() => {
    refresh();
  }, [refresh]);

  const save = useCallback(
    async (key: string) => {
      // Enum keys go through /api/settings so its per-key validation (e.g.
      // rejecting an unknown persona) still runs; both endpoints proxy the same
      // /v1/config/set and return the same shape.
      const url = enumOptions[key] ? "/api/settings" : "/api/config/set";
      const { status: st, data } = await postJSON<{ error?: string; notice?: string; value?: Val }>(
        url,
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
    [draft, enumOptions],
  );

  // Group + filter the fields for rendering.
  const groups = useMemo(() => {
    const q = filter.trim().toLowerCase();
    const keys = Object.keys(values)
      .filter((k) => {
        // Owned by another tab: never listed here, not even under "Show advanced"
        // or a search term — a second editable copy is exactly the problem this
        // removes. The section header points at the owning tab instead.
        if (OWNED_ELSEWHERE[k]) return false;
        // Everyday surface: hide deploy/advanced/dev keys unless the operator opts in.
        // A search term reveals matching off-surface keys regardless (so they stay findable).
        if (!showAdvanced && !q && fieldGroups[k]) return false;
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
  }, [values, filter, fieldGroups, showAdvanced]);

  // Counts describe what this page governs, so options another tab owns are out
  // of both (they are not "hidden advanced" — they are not this page's at all).
  const hiddenCount = useMemo(
    () =>
      showAdvanced
        ? 0
        : Object.keys(values).filter((k) => fieldGroups[k] && !OWNED_ELSEWHERE[k]).length,
    [values, fieldGroups, showAdvanced],
  );
  const runtimeCount = useMemo(
    () => Object.keys(values).filter((k) => !fieldGroups[k] && !OWNED_ELSEWHERE[k]).length,
    [values, fieldGroups],
  );

  // Where the options this page no longer lists actually live. Rendered as one
  // always-visible line (not per section) so it survives a section disappearing
  // entirely once every key in it moved to its owning tab.
  const movedOwners = useMemo(() => {
    const seen: string[] = [];
    for (const k of Object.keys(values)) {
      const owner = OWNED_ELSEWHERE[k];
      if (owner && !seen.includes(owner)) seen.push(owner);
    }
    return seen.sort();
  }, [values]);

  return (
    <div style={{ padding: 16, fontFamily: "system-ui", height: "100%", overflow: "auto" }}>
      <div style={{ display: "flex", alignItems: "center", gap: 10, marginBottom: 12, flexWrap: "wrap" }}>
        <strong style={{ fontSize: 18 }}>Settings</strong>
        <Badge
          label={showAdvanced ? `${Object.keys(values).length}` : `${runtimeCount}`}
          variant="neutral"
        />
        <input
          placeholder="filter settings…"
          value={filter}
          onChange={(e) => setFilter(e.target.value)}
          style={{ ...input, fontFamily: "system-ui", minWidth: 220 }}
        />
        {(showAdvanced || hiddenCount > 0) && (
          <Button
            size="sm"
            onClick={() => setShowAdvanced((v) => !v)}
            title="Deploy-time, advanced-tuning, and dev-only options are hidden by default. They are still settable — this reveals them."
          >
            {showAdvanced ? "Hide advanced" : `Show advanced (${hiddenCount})`}
          </Button>
        )}
        <Button size="sm" onClick={refresh}>
          Reload
        </Button>
        <Button
          size="sm"
          onClick={() => {
            resetTutorials();
            setStatus({ kind: "ok", msg: "Tab tutorials will show again on your next visit to each tab." });
          }}
          title="Show the per-tab tutorial overlays again"
        >
          Replay tab tutorials
        </Button>
        <Button
          size="sm"
          onClick={() => {
            setSetupDismissed(false);
            requestOpenWizard();
          }}
          title="Re-open the first-run setup wizard"
        >
          Re-run setup
        </Button>
        <InlineStatus status={status} />
      </div>
      <p style={{ fontSize: 12, color: "var(--sg-text-secondary)", margin: "0 0 12px" }}>
        Changes persist to <code>aimee.yaml</code> and take effect on the next turn, unless a row is
        marked <em>restart</em>. The everyday runtime options are shown by default; deploy-time,
        advanced-tuning, and dev-only options are hidden behind <em>Show advanced</em> (still
        settable, and any of them surfaces when you search). Each option is described below.
      </p>
      {movedOwners.length > 0 && (
        <p style={{ fontSize: 12, color: "var(--sg-text-secondary)", margin: "0 0 12px" }}>
          Options owned by another tab are configured there, not here:{" "}
          <strong>{movedOwners.join(" · ")}</strong>. Each option has one owner, so a value set in
          its own tab is never silently overwritten by an edit made here.
        </p>
      )}

      {!loaded && <div style={{ color: "var(--sg-text-faint)" }}>loading…</div>}
      {loaded && Object.keys(values).length === 0 && (
        <div style={{ color: "var(--sg-text-faint)" }}>No configurable options available (aimee-server unreachable?).</div>
      )}

      <div style={{ display: "grid", gap: 12 }}>
        {groups.map(([cat, keys]) => (
          <Panel key={cat} title={cat} count={keys.length}>
            {SECTION_HELP[cat] && (
              <p style={{ fontSize: 12, color: "var(--sg-text-faint)", margin: "0 0 10px", lineHeight: 1.4 }}>
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
                  group={fieldGroups[k]}
                  options={enumOptions[k]}
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
  group,
  options,
  onChange,
  onSave,
  onReset,
}: {
  fieldKey: string;
  value: Val;
  dirty: boolean;
  group?: string;
  options?: string[];
  onChange: (v: Val) => void;
  onSave: () => void;
  onReset: () => void;
}) {
  const help = FIELD_HELP[fieldKey];
  const needsRestart = RESTART_KEYS.has(fieldKey);
  // Off-surface classification badge (deploy/advanced/dev); runtime keys carry none.
  const groupTitle: Record<string, string> = {
    deploy: "Deploy-time: set once when standing up the stack; not tuned day-to-day.",
    advanced: "Advanced tuning: has a sensible default; rarely changed.",
    dev: "Dev-only: internal QA/dogfood knob.",
  };
  return (
    <div style={{ display: "flex", alignItems: "flex-start", gap: 8, flexWrap: "wrap" }}>
      <div style={{ flex: "1 1 300px", minWidth: 220 }}>
        <label style={{ fontSize: 13, display: "flex", alignItems: "center", gap: 6, flexWrap: "wrap" }} title={fieldKey}>
          {humanize(fieldKey)}
          <span style={{ color: "var(--sg-text-pale)", fontSize: 11 }}>{fieldKey}</span>
          {group && (
            <span
              style={{
                fontSize: 10,
                color: "var(--sg-text-muted)",
                background: "var(--sg-surface-sunken)",
                border: "1px solid var(--sg-border-medium)",
                borderRadius: 4,
                padding: "0 5px",
              }}
              title={groupTitle[group] || group}
            >
              {group}
            </span>
          )}
          {needsRestart && (
            <span
              style={{
                fontSize: 10,
                color: "var(--sg-warning-dark)",
                background: "var(--sg-warning-bg)",
                border: "1px solid var(--sg-warning-border)",
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
          <div style={{ fontSize: 12, color: "var(--sg-text-faint)", marginTop: 2, lineHeight: 1.4 }}>{help}</div>
        )}
      </div>
      <div style={{ flex: "0 0 auto", display: "flex", alignItems: "center", gap: 6, marginTop: 1 }}>
        {options ? (
          <select
            value={String(value ?? "")}
            onChange={(e) => onChange(e.target.value)}
            style={{ ...input, width: 220, fontFamily: "system-ui" }}
          >
            {/* Keep an out-of-list saved value selectable so the dropdown never
             * silently rewrites it to the first option. */}
            {(options.includes(String(value ?? "")) ? options : [String(value ?? ""), ...options]).map((o) => (
              <option key={o} value={o}>
                {o}
              </option>
            ))}
          </select>
        ) : typeof value === "boolean" ? (
          <Button
            size="sm"
            onClick={() => onChange(!value)}
            style={
              value
                ? { minWidth: 44, background: "var(--sg-success-dark)", color: "var(--sg-surface)", borderColor: "var(--sg-success-dark)" }
                : { minWidth: 44 }
            }
          >
            {value ? "on" : "off"}
          </Button>
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
            <Button variant="primary" size="sm" onClick={onSave}>
              save
            </Button>
            <Button size="sm" onClick={onReset} title="discard change">
              ↺
            </Button>
          </>
        )}
      </div>
    </div>
  );
}
