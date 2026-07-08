import { useCallback, useEffect, useState } from "react";
import { Panel } from "@rakuensoftware/smoothgui";

/* Roles page: edit the shared ROLE vocabulary — each role's name, what it does
 * (the delegate system-prompt template), and its per-role turn cap (max_turns,
 * -1 = infinite, the default). Roles are the routing key personas and agents are
 * matched on. Split out of the Personas tab so the turn cap reads as a role
 * setting, never a persona one — personas never carry a turn limit. */

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

/* Upsert `max_turns` into a role template's YAML frontmatter so the dedicated
 * field and the raw body stay consistent on save. -1 = infinite. */
function withMaxTurns(body: string, mt: number): string {
  const fm = /^---\r?\n([\s\S]*?)\r?\n---\r?\n?/;
  const m = body.match(fm);
  if (m) {
    let inner = m[1];
    inner = /^max_turns:.*$/m.test(inner)
      ? inner.replace(/^max_turns:.*$/m, `max_turns: ${mt}`)
      : `max_turns: ${mt}\n${inner}`;
    return body.replace(fm, `---\n${inner}\n---\n\n`);
  }
  return `---\nmax_turns: ${mt}\n---\n\n${body}`;
}

type Status = { kind: "ok" | "err"; msg: string } | null;

export default function Roles() {
  const [status, setStatus] = useState<Status>(null);
  const [roles, setRoles] = useState<string[]>([]);
  const [roleSel, setRoleSel] = useState<string | null>(null);
  const [roleBody, setRoleBody] = useState("");
  const [roleMaxTurns, setRoleMaxTurns] = useState<number>(-1); // -1 = infinite

  const refreshRoles = useCallback(() => {
    getJSON<{ role_templates?: string[] }>("/api/roles")
      .then((d) => setRoles((d.role_templates || []).slice().sort()))
      .catch(() => setRoles([]));
  }, []);

  const openRole = useCallback((name: string) => {
    setRoleSel(name);
    setRoleBody("");
    setRoleMaxTurns(-1);
    getJSON<{ content?: string; max_turns?: number }>(`/api/roles/${encodeURIComponent(name)}`)
      .then((d) => {
        setRoleBody(d.content || "");
        setRoleMaxTurns(typeof d.max_turns === "number" ? d.max_turns : -1);
      })
      .catch(() => setRoleBody(""));
  }, []);

  const saveRole = async () => {
    if (!roleSel) return;
    const { status: st } = await sendJSON("PUT", `/api/roles/${encodeURIComponent(roleSel)}`, {
      content: withMaxTurns(roleBody, roleMaxTurns),
    });
    if (st >= 200 && st < 300) {
      setStatus({ kind: "ok", msg: `role “${roleSel}” saved` });
      refreshRoles();
    } else setStatus({ kind: "err", msg: `save failed (${st})` });
  };

  const newRole = () => {
    const name = window.prompt("New role name (letters, digits, - or _):")?.trim();
    if (!name) return;
    if (!nameOk(name)) {
      setStatus({ kind: "err", msg: "invalid role name" });
      return;
    }
    setRoleSel(name);
    setRoleBody(`You are a ${name} delegate. Your mission is to …\n`);
    setRoleMaxTurns(-1);
  };

  const deleteRole = async () => {
    if (!roleSel) return;
    if (!window.confirm(`Delete role “${roleSel}”? (built-ins reset to default)`)) return;
    const { status: st } = await sendJSON("DELETE", `/api/roles/${encodeURIComponent(roleSel)}`);
    if (st >= 200 && st < 300) {
      setStatus({ kind: "ok", msg: `role “${roleSel}” deleted` });
      setRoleSel(null);
      setRoleBody("");
      refreshRoles();
    } else setStatus({ kind: "err", msg: `delete failed (${st})` });
  };

  useEffect(() => {
    refreshRoles();
  }, [refreshRoles]);

  return (
    <div style={{ padding: 16, fontFamily: "system-ui", height: "100%", overflow: "auto" }}>
      <div style={{ display: "flex", alignItems: "center", gap: 10, marginBottom: 12 }}>
        <strong style={{ fontSize: 18 }}>Roles</strong>
        {status && (
          <span style={{ fontSize: 13, color: status.kind === "err" ? "#b00" : "#080" }}>{status.msg}</span>
        )}
      </div>

      <div style={{ maxWidth: 720 }}>
        <Panel title="Roles" count={roles.length}>
          <p style={{ fontSize: 12, color: "#666", margin: "0 0 8px" }}>
            The shared vocabulary. A role’s body describes what it does; personas and agents are matched on
            these names (plus the <code>all</code> wildcard). The turn cap is a <strong>role</strong> setting —
            personas never carry one.
          </p>
          <div style={{ display: "flex", flexWrap: "wrap", gap: 6, marginBottom: 8 }}>
            {roles.map((r) => (
              <button
                key={r}
                onClick={() => openRole(r)}
                style={{ ...btn, background: roleSel === r ? "#e8eef9" : "#fff" }}
              >
                {r}
              </button>
            ))}
            <button onClick={newRole} style={{ ...btn, borderStyle: "dashed" }}>
              + New
            </button>
          </div>
          {roleSel && (
            <div>
              <label style={lbl}>
                {roleSel} — what it does (delegate system-prompt template)
              </label>
              <textarea rows={12} style={ta} value={roleBody} onChange={(e) => setRoleBody(e.target.value)} />
              <label style={{ ...lbl, marginTop: 8 }}>
                Max turns per delegate run (−1 = infinite, the default)
              </label>
              <input
                type="number"
                min={-1}
                step={1}
                style={{ ...ta, height: "auto" }}
                value={roleMaxTurns}
                onChange={(e) => {
                  const v = parseInt(e.target.value, 10);
                  setRoleMaxTurns(Number.isFinite(v) ? v : -1);
                }}
              />
              <div style={{ display: "flex", gap: 8, marginTop: 8 }}>
                <button onClick={saveRole} style={btn}>
                  Save role
                </button>
                <button onClick={deleteRole} style={{ ...btn, color: "#b00" }}>
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
