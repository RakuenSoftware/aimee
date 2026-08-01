import { useCallback, useEffect, useMemo, useState } from "react";
import { apiGet, apiSend } from "../api";

/* Settings page for the kb console: the config the KB OWNS.
 *
 * The kb runs the embedder and the synth tier, so those options
 * are configured here rather than on aimee-server's Settings page — one owner
 * per option. The field list, its grouping, and which fields need a kb restart
 * all come from the backend (GET /v1/console/settings, KB_SETTINGS in
 * src/kb/http/kb_http_console.c), so this page has no hand-kept mirror of the
 * config allowlist. A save goes to POST /v1/console/settings/config, which
 * allowlists the same keys and persists aimee.yaml.
 *
 * The control is inferred from the value's JSON type, matching the aimee
 * Settings page: boolean → on/off, number → number field, string → text. */

type Val = boolean | number | string;

interface Field {
  key: string;
  section: string;
  restart: boolean;
  value: Val;
}

// Friendly label from a snake_case key: "llm_embed_host" → "Llm embed host".
function humanize(key: string): string {
  const s = key.replace(/_/g, " ").trim();
  return s.charAt(0).toUpperCase() + s.slice(1);
}

export default function Settings() {
  const [fields, setFields] = useState<Field[]>([]);
  const [draft, setDraft] = useState<Record<string, Val>>({});
  const [loaded, setLoaded] = useState(false);
  const [busy, setBusy] = useState("");
  const [notice, setNotice] = useState("");
  const [err, setErr] = useState("");

  const refresh = useCallback(() => {
    apiGet<{ fields?: Field[] }>("/v1/console/settings")
      .then((d) => {
        const fs = d.fields || [];
        setFields(fs);
        setDraft(Object.fromEntries(fs.map((f) => [f.key, f.value])));
        setErr("");
        setLoaded(true);
      })
      .catch((e) => {
        setErr(`Failed to load settings: ${e}`);
        setLoaded(true);
      });
  }, []);

  useEffect(() => {
    refresh();
  }, [refresh]);

  const save = async (f: Field) => {
    setBusy(f.key);
    setNotice("");
    try {
      await apiSend("POST", "/v1/console/settings/config", { key: f.key, value: draft[f.key] });
      // Take the saved value from our own draft: the response echoes what the
      // backend stored, but refetching every field on each save would stomp any
      // other row the operator is mid-edit.
      setFields((prev) => prev.map((x) => (x.key === f.key ? { ...x, value: draft[f.key] } : x)));
      setNotice(
        f.restart
          ? `${f.key} saved — restart aimee-kb for this to take effect`
          : `${f.key} saved`,
      );
      setErr("");
    } catch (e) {
      setErr(`Save failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  // Preserve the backend's field order within each section, and the order the
  // sections first appear in — that order is deliberate (Embedder,
  // Synth, …) rather than alphabetical.
  const sections = useMemo(() => {
    const out: [string, Field[]][] = [];
    for (const f of fields) {
      const hit = out.find(([name]) => name === f.section);
      if (hit) hit[1].push(f);
      else out.push([f.section, [f]]);
    }
    return out;
  }, [fields]);

  if (!loaded) return <div style={{ padding: 24, color: "#888" }}>Loading settings…</div>;

  return (
    <div style={{ padding: "18px 24px", maxWidth: 860, margin: "0 auto", fontFamily: "system-ui" }}>
      <h2 style={{ margin: "0 0 4px" }}>Settings</h2>
      <p style={{ color: "#777", fontSize: 13, margin: "0 0 18px" }}>
        The options aimee-kb owns — the embedder, the synth tier, and the knowledge
        base itself. Changes persist to <code>aimee.yaml</code>; rows marked <em>restart</em> are
        bound when the kb starts and take effect on its next restart. Options aimee-server owns stay
        on its own Settings page.
      </p>
      {err && <p style={{ color: "#b00", fontSize: 13 }}>{err}</p>}
      {notice && <p style={{ color: "#1f7a3d", fontSize: 13 }}>{notice}</p>}
      {fields.length === 0 && !err && (
        <div style={{ color: "#888" }}>No settings reported by the kb.</div>
      )}

      {sections.map(([name, fs]) => (
        <div
          key={name}
          style={{ marginBottom: 22, border: "1px solid #e2e2e2", borderRadius: 8, overflow: "hidden" }}
        >
          <div style={{ padding: "8px 12px", background: "#fafafa", borderBottom: "1px solid #e2e2e2", fontWeight: 700, color: "#555" }}>
            {name}
          </div>
          {fs.map((f) => {
            const dirty = draft[f.key] !== f.value;
            return (
              <div
                key={f.key}
                style={{ display: "flex", alignItems: "center", gap: 12, padding: "8px 12px", borderBottom: "1px solid #eee", flexWrap: "wrap" }}
              >
                <div style={{ flex: "1 1 280px", minWidth: 200 }}>
                  <div style={{ fontSize: 13, fontWeight: 600 }}>
                    {humanize(f.key)}{" "}
                    <code style={{ color: "#aaa", fontSize: 11, fontWeight: 400 }}>{f.key}</code>
                    {f.restart && (
                      <span
                        title="Takes effect only after aimee-kb restarts."
                        style={{ marginLeft: 8, fontSize: 10, color: "#a60", background: "#fff6e6", border: "1px solid #f0d9a8", borderRadius: 4, padding: "0 5px" }}
                      >
                        restart
                      </span>
                    )}
                  </div>
                </div>
                <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
                  {typeof f.value === "boolean" ? (
                    <button
                      onClick={() => setDraft((p) => ({ ...p, [f.key]: !p[f.key] }))}
                      style={{
                        minWidth: 44, padding: "4px 10px", borderRadius: 6, cursor: "pointer",
                        border: "1px solid " + (draft[f.key] ? "#1f7a3d" : "#ccc"),
                        background: draft[f.key] ? "#1f7a3d" : "#fff",
                        color: draft[f.key] ? "#fff" : "#666",
                      }}
                    >
                      {draft[f.key] ? "on" : "off"}
                    </button>
                  ) : typeof f.value === "number" ? (
                    <input
                      type="number"
                      value={Number(draft[f.key] ?? 0)}
                      onChange={(e) => setDraft((p) => ({ ...p, [f.key]: Number(e.target.value) || 0 }))}
                      style={inputStyle}
                    />
                  ) : (
                    <input
                      value={String(draft[f.key] ?? "")}
                      onChange={(e) => setDraft((p) => ({ ...p, [f.key]: e.target.value }))}
                      style={{ ...inputStyle, width: 240 }}
                    />
                  )}
                  {dirty && (
                    <>
                      <button
                        disabled={busy === f.key}
                        onClick={() => save(f)}
                        style={{ padding: "4px 10px", borderRadius: 6, border: "1px solid #2563eb", background: "#2563eb", color: "#fff", cursor: "pointer" }}
                      >
                        save
                      </button>
                      <button
                        onClick={() => setDraft((p) => ({ ...p, [f.key]: f.value }))}
                        title="discard change"
                        style={{ padding: "4px 8px", borderRadius: 6, border: "1px solid #ccc", background: "#fff", cursor: "pointer" }}
                      >
                        ↺
                      </button>
                    </>
                  )}
                </div>
              </div>
            );
          })}
        </div>
      ))}
    </div>
  );
}

const inputStyle: React.CSSProperties = {
  fontFamily: "ui-monospace, monospace",
  fontSize: 12,
  padding: "3px 6px",
  borderRadius: 6,
  border: "1px solid #ccc",
  width: 120,
  boxSizing: "border-box",
};
