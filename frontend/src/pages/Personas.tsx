import { useCallback, useEffect, useMemo, useState } from "react";
import { Panel, Badge } from "@rakuensoftware/smoothgui";

/* Personas page: edit the PERSONA definitions (identity + the roles each persona
 * may use). Roles themselves — their bodies and per-role turn caps — live on the
 * separate Roles tab; this page only reads the role list to offer them as routing
 * chips. A persona never carries a turn limit. */

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

interface PersonaInfo {
  name: string;
  description?: string;
  builtin?: boolean;
}
interface PersonaDef {
  name: string;
  description?: string;
  delegates?: string; // full | readonly | none
  roles?: string[];
  check_role?: string;
  check_marker?: string;
  persona?: string;
  principles?: string;
  brief?: string;
  builtin?: boolean;
}

const btn: React.CSSProperties = {
  padding: "4px 10px",
  fontSize: 13,
  borderRadius: 6,
  border: "1px solid #ccc",
  background: "#fff",
  cursor: "pointer",
};
const lbl: React.CSSProperties = { fontSize: 12, color: "#666", display: "block", marginBottom: 2 };
const ta: React.CSSProperties = {
  width: "100%",
  fontFamily: "ui-monospace, monospace",
  fontSize: 12,
  padding: 6,
  borderRadius: 6,
  border: "1px solid #ccc",
  boxSizing: "border-box",
};
const nameOk = (s: string) => /^[a-z0-9][a-z0-9_-]*$/i.test(s);

type Status = { kind: "ok" | "err"; msg: string } | null;

export default function Personas() {
  const [status, setStatus] = useState<Status>(null);

  // Role names — read-only here, used only as routing chips. Roles are edited on
  // the Roles tab.
  const [roles, setRoles] = useState<string[]>([]);

  const refreshRoles = useCallback(() => {
    getJSON<{ role_templates?: string[] }>("/api/roles")
      .then((d) => setRoles((d.role_templates || []).slice().sort()))
      .catch(() => setRoles([]));
  }, []);

  // ---- personas ----
  const [personas, setPersonas] = useState<PersonaInfo[]>([]);
  const [form, setForm] = useState<PersonaDef | null>(null);

  const refreshPersonas = useCallback(() => {
    getJSON<{ personas?: PersonaInfo[] }>("/api/chat/personas")
      .then((d) => setPersonas(d.personas || []))
      .catch(() => setPersonas([]));
  }, []);

  const openPersona = useCallback((name: string) => {
    getJSON<PersonaDef>(`/api/chat/personas/${encodeURIComponent(name)}`)
      .then((d) => setForm({ ...d, name }))
      .catch(() => setStatus({ kind: "err", msg: "could not load persona" }));
  }, []);

  const newPersona = () =>
    setForm({ name: "", description: "", delegates: "full", roles: [], persona: "", principles: "", brief: "" });

  const upd = (p: Partial<PersonaDef>) => setForm((f) => (f ? { ...f, ...p } : f));

  const toggleRole = (r: string) =>
    setForm((f) => {
      if (!f) return f;
      const cur = f.roles || [];
      return { ...f, roles: cur.includes(r) ? cur.filter((x) => x !== r) : [...cur, r] };
    });

  const savePersona = async () => {
    if (!form) return;
    const name = form.name.trim();
    if (!nameOk(name)) {
      setStatus({ kind: "err", msg: "name must be alphanumeric, - or _" });
      return;
    }
    const body = {
      name,
      description: form.description || "",
      delegates: form.delegates || "full",
      roles: form.roles || [],
      check_role: form.check_role || "",
      check_marker: form.check_marker || "",
      persona: form.persona || "",
      principles: form.principles || "",
      brief: form.brief || "",
    };
    const { status: st } = await sendJSON("PUT", `/api/chat/personas/${encodeURIComponent(name)}`, body);
    if (st >= 200 && st < 300) {
      setStatus({ kind: "ok", msg: `persona “${name}” saved` });
      refreshPersonas();
    } else setStatus({ kind: "err", msg: `save failed (${st})` });
  };

  const deletePersona = async () => {
    if (!form?.name) return;
    if (!window.confirm(`Delete persona “${form.name}”? (built-ins reset to default)`)) return;
    const { status: st } = await sendJSON("DELETE", `/api/chat/personas/${encodeURIComponent(form.name)}`);
    if (st >= 200 && st < 300) {
      setStatus({ kind: "ok", msg: `persona “${form.name}” deleted` });
      setForm(null);
      refreshPersonas();
    } else setStatus({ kind: "err", msg: `delete failed (${st})` });
  };

  useEffect(() => {
    refreshRoles();
    refreshPersonas();
  }, [refreshRoles, refreshPersonas]);

  // Role chips a persona may select: the known roles + the "all" wildcard.
  const roleOptions = useMemo(() => {
    const set = new Set<string>(["all", ...roles]);
    (form?.roles || []).forEach((r) => set.add(r));
    return Array.from(set);
  }, [roles, form]);

  return (
    <div style={{ padding: 16, fontFamily: "system-ui", height: "100%", overflow: "auto" }}>
      <div style={{ display: "flex", alignItems: "center", gap: 10, marginBottom: 12 }}>
        <strong style={{ fontSize: 18 }}>Personas</strong>
        {status && (
          <span style={{ fontSize: 13, color: status.kind === "err" ? "#b00" : "#080" }}>{status.msg}</span>
        )}
      </div>

      <div style={{ maxWidth: 720 }}>
        <Panel title="Personas" count={personas.length}>
          <p style={{ fontSize: 12, color: "#666", margin: "0 0 8px" }}>
            A persona is a delegate identity plus the roles it may use. Roles (and their per-role turn caps) are
            edited on the <strong>Roles</strong> tab.
          </p>
          <div style={{ display: "flex", flexWrap: "wrap", gap: 6, marginBottom: 8 }}>
            {personas.map((p) => (
              <button
                key={p.name}
                onClick={() => openPersona(p.name)}
                style={{ ...btn, background: form?.name === p.name ? "#e8eef9" : "#fff" }}
                title={p.description || ""}
              >
                {p.name}
                {p.builtin ? " ·" : ""}
              </button>
            ))}
            <button onClick={newPersona} style={{ ...btn, borderStyle: "dashed" }}>
              + New
            </button>
          </div>

          {form && (
            <div style={{ display: "grid", gap: 8 }}>
              <div>
                <label style={lbl}>name</label>
                <input
                  style={{ ...ta, fontFamily: "system-ui" }}
                  value={form.name}
                  disabled={!!form.builtin}
                  onChange={(e) => upd({ name: e.target.value })}
                />
              </div>
              <div>
                <label style={lbl}>description</label>
                <input
                  style={{ ...ta, fontFamily: "system-ui" }}
                  value={form.description || ""}
                  onChange={(e) => upd({ description: e.target.value })}
                />
              </div>
              <div>
                <label style={lbl}>roles (routing key — click to toggle; edit roles on the Roles tab)</label>
                <div style={{ display: "flex", flexWrap: "wrap", gap: 6 }}>
                  {roleOptions.map((r) => {
                    const on = (form.roles || []).includes(r);
                    return (
                      <button
                        key={r}
                        onClick={() => toggleRole(r)}
                        style={{
                          ...btn,
                          padding: "2px 8px",
                          background: on ? "#1f7a3d" : "#fff",
                          color: on ? "#fff" : r === "all" ? "#a15" : "#333",
                          fontWeight: r === "all" ? 600 : 400,
                        }}
                      >
                        {r}
                      </button>
                    );
                  })}
                </div>
              </div>
              <div>
                <label style={lbl}>delegate policy</label>
                <select
                  style={{ ...ta, fontFamily: "system-ui" }}
                  value={form.delegates || "full"}
                  onChange={(e) => upd({ delegates: e.target.value })}
                >
                  <option value="full">full</option>
                  <option value="readonly">readonly</option>
                  <option value="none">none</option>
                </select>
              </div>
              <div>
                <label style={lbl}>persona (system prompt)</label>
                <textarea rows={5} style={ta} value={form.persona || ""} onChange={(e) => upd({ persona: e.target.value })} />
              </div>
              <div>
                <label style={lbl}>principles</label>
                <textarea rows={4} style={ta} value={form.principles || ""} onChange={(e) => upd({ principles: e.target.value })} />
              </div>
              <div>
                <label style={lbl}>brief (session hints)</label>
                <textarea rows={3} style={ta} value={form.brief || ""} onChange={(e) => upd({ brief: e.target.value })} />
              </div>
              <div style={{ display: "flex", gap: 8 }}>
                <button onClick={savePersona} style={btn}>
                  Save persona
                </button>
                <button onClick={deletePersona} style={{ ...btn, color: "#b00" }}>
                  Delete
                </button>
                {form.builtin && <Badge label="built-in" variant="neutral" />}
              </div>
            </div>
          )}
        </Panel>
      </div>
    </div>
  );
}
