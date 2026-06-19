import { useEffect, useState, useCallback, useRef } from "react";
import { Panel, Badge, Spinner } from "@rakuensoftware/smoothgui";
import ProjectPicker from "../components/ProjectPicker";
import type { ProjectSelection } from "../components/ProjectPicker";
import { useSessions } from "../SessionContext";

/* ---- API types (mirror /v1/workflow/* envelopes) ---- */

interface BlockDef {
  name: string;
  produces: string;
  accepts: string[];
  custom: boolean;
  requires_input: boolean;
  executor?: string;
}
interface DefRow {
  name: string;
  valid: boolean;
  version: string;
}
interface Binding {
  input: string;
  producer: string;
  output: string;
}
interface GNode {
  id: string;
  block: string;
  custom: boolean;
  produces: string;
  in: Binding[];
  next?: string;
  on_pass?: string;
  on_fail?: string;
  params?: Record<string, unknown>;
}
interface GraphDef {
  name: string;
  start: string;
  nodes: GNode[];
}
interface DefResponse {
  valid: boolean;
  name: string;
  version: string;
  canonical: string;
  def: GraphDef;
  error?: string;
}
interface RunItem {
  id: string;
  workflow: string;
  version: string;
  stage: string;
  state: string;
  mode: string;
  pause_reason: string;
  repo: string;
}

/* ---- personas + delegates (the per-step assignment options) ---- */

interface PersonaInfo {
  name: string;
  description?: string;
  builtin?: boolean;
}
// Full persona definition (GET /api/chat/personas/<name>); used by the manager
// form. Fields mirror persona_to_json / persona_from_json on the server.
interface PersonaDef {
  name: string;
  description?: string;
  delegates?: string; // "full" | "readonly" | "none"
  roles?: string[];
  check_role?: string;
  check_marker?: string;
  persona?: string; // "## Persona" body
  principles?: string; // "## Principles" body
  brief?: string; // "## Brief" session hints
  builtin?: boolean;
}
// A registered/known delegate (GET /api/agents). Used only to populate the
// delegate picker's suggestions; the field also accepts free text.
interface AgentInfo {
  name: string;
}

// One step participant: a persona run on a delegate. A gate.roundtable step has
// several (e.g. 4 lenses); a single-action step (implement/author/…) has one.
interface Participant {
  persona: string;
  delegate: string;
}

const ROUNDTABLE_BLOCK = "gate.roundtable";

// Read the participants a node currently declares, from whichever param shape it
// uses: a roundtable's panel (required = delegates, personas = the lenses) or a
// single-action step's persona/delegate. Returns [] when none are set.
function readParticipants(node: GNode): Participant[] {
  const p = (node.params || {}) as Record<string, unknown>;
  const panel = (p.panel || {}) as Record<string, unknown>;
  const strs = (v: unknown): string[] =>
    Array.isArray(v) ? v.filter((x): x is string => typeof x === "string") : [];
  if (Array.isArray(panel.required) || Array.isArray(panel.personas)) {
    const dels = strs(panel.required);
    const pers = strs(panel.personas);
    const n = Math.max(dels.length, pers.length);
    const out: Participant[] = [];
    for (let i = 0; i < n; i++)
      out.push({ persona: pers[i] || "", delegate: dels[i] || "" });
    return out;
  }
  const persona = typeof p.persona === "string" ? p.persona : "";
  const delegate = typeof p.delegate === "string" ? p.delegate : "";
  if (persona || delegate) return [{ persona, delegate }];
  return [];
}

// The human label for a step: its title param if set, else the node id.
function nodeTitle(node: GNode): string {
  const t = (node.params as Record<string, unknown> | undefined)?.title;
  return typeof t === "string" && t.trim() ? t : node.id;
}

// A compact "persona@delegate, …" line for the node card, or "" when unset.
function participantSummary(node: GNode): string {
  const ps = readParticipants(node).filter((p) => p.persona || p.delegate);
  if (!ps.length) return "";
  const parts = ps
    .slice(0, 2)
    .map((p) =>
      p.persona && p.delegate
        ? `${p.persona}@${p.delegate}`
        : p.persona || p.delegate,
    );
  return parts.join(", ") + (ps.length > 2 ? ` +${ps.length - 2}` : "");
}

/* ---- block-style YAML emitter (aimee's yaml.c is block-style only: no flow
 *      sequences). The server canonicalizes + validates on save, so this only
 *      needs to round-trip through wfe_def_parse, not match canonical form. ---- */

// YAML 1.1 reserved words that a bare scalar would be misread as (aimee's yaml.c
// resolves true/false specially; quote anything that could collide).
const YAML_RESERVED = new Set([
  "true",
  "false",
  "null",
  "yes",
  "no",
  "on",
  "off",
  "y",
  "n",
  "~",
]);
function isBareSafe(s: string): boolean {
  return (
    /^[A-Za-z0-9_./]+$/.test(s) && // no leading '-' (would read as a sequence/scalar)
    !/^-?\d/.test(s) && // not a number
    !YAML_RESERVED.has(s.toLowerCase())
  );
}
function scalar(v: unknown): string {
  if (typeof v === "boolean") return v ? "true" : "false";
  if (typeof v === "number") return String(v);
  const s = String(v);
  return isBareSafe(s) ? s : JSON.stringify(s);
}
function emitYamlValue(v: unknown, indent: number, out: string[]): void {
  const pad = " ".repeat(indent);
  if (Array.isArray(v)) {
    for (const item of v) {
      if (item !== null && typeof item === "object") {
        out.push(`${pad}-`);
        emitYamlValue(item, indent + 2, out);
      } else {
        out.push(`${pad}- ${scalar(item)}`);
      }
    }
  } else if (v !== null && typeof v === "object") {
    // quote keys too: params are user-edited JSON, so a key with ':'/'#'/a
    // reserved word must not be emitted bare (would restructure the parse).
    for (const [k, val] of Object.entries(v as Record<string, unknown>)) {
      if (val !== null && typeof val === "object") {
        out.push(`${pad}${scalar(k)}:`);
        emitYamlValue(val, indent + 2, out);
      } else {
        out.push(`${pad}${scalar(k)}: ${scalar(val)}`);
      }
    }
  }
}

function emitWorkflowYaml(g: GraphDef): string {
  const out: string[] = [];
  out.push(`name: ${scalar(g.name)}`);
  if (g.start) out.push(`start: ${scalar(g.start)}`);
  out.push("nodes:");
  for (const n of g.nodes) {
    out.push(`  - id: ${scalar(n.id)}`);
    out.push(`    block: ${scalar(n.block)}`);
    if (n.in.length) {
      out.push("    in:");
      // `in` is a MAP (input_name -> "producer.output"); quote each field so an
      // id/name with YAML-special chars can't break or restructure the parse.
      for (const b of n.in)
        out.push(
          `      ${scalar(b.input)}: ${scalar(`${b.producer}.${b.output || "out"}`)}`,
        );
    }
    if (n.params && Object.keys(n.params).length) {
      out.push("    params:");
      emitYamlValue(n.params, 6, out);
    }
    if (n.next) out.push(`    next: ${scalar(n.next)}`);
    if (n.on_pass) out.push(`    on_pass: ${scalar(n.on_pass)}`);
    if (n.on_fail) out.push(`    on_fail: ${scalar(n.on_fail)}`);
  }
  return out.join("\n") + "\n";
}

/* ---- layered auto-layout (the def carries no coordinates) ---- */

function autoLayout(g: GraphDef): Record<string, { x: number; y: number }> {
  const depth: Record<string, number> = {};
  const adj = (n: GNode): string[] =>
    [n.next, n.on_pass, n.on_fail].filter((x): x is string => !!x);
  const start = g.start || (g.nodes[0]?.id ?? "");
  const queue: string[] = start ? [start] : [];
  depth[start] = 0;
  while (queue.length) {
    const cur = queue.shift() as string;
    const node = g.nodes.find((n) => n.id === cur);
    if (!node) continue;
    for (const t of adj(node)) {
      if (!(t in depth)) {
        depth[t] = depth[cur] + 1;
        queue.push(t);
      }
    }
  }
  let maxd = 0;
  for (const n of g.nodes) {
    if (!(n.id in depth)) depth[n.id] = 0;
    maxd = Math.max(maxd, depth[n.id]);
  }
  const rowOf: Record<number, number> = {};
  const pos: Record<string, { x: number; y: number }> = {};
  for (const n of g.nodes) {
    const d = depth[n.id];
    const row = rowOf[d] || 0;
    rowOf[d] = row + 1;
    pos[n.id] = { x: 40 + d * 230, y: 30 + row * 120 };
  }
  return pos;
}

const NODE_W = 160;
const NODE_H = 56;

async function getJSON<T>(url: string): Promise<T> {
  const r = await fetch(url, {
    headers: { "X-CSRF-Token": window._csrf || "" },
  });
  return (await r.json()) as T;
}
async function postJSON<T>(
  url: string,
  body: unknown,
): Promise<{ status: number; data: T }> {
  const r = await fetch(url, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "X-CSRF-Token": window._csrf || "",
    },
    body: JSON.stringify(body),
  });
  return { status: r.status, data: (await r.json()) as T };
}
async function sendJSON<T>(
  method: "PUT" | "DELETE",
  url: string,
  body?: unknown,
): Promise<{ status: number; data: T }> {
  const r = await fetch(url, {
    method,
    headers: {
      "Content-Type": "application/json",
      "X-CSRF-Token": window._csrf || "",
    },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  let data = {} as T;
  try {
    data = (await r.json()) as T;
  } catch {
    /* DELETE/empty bodies are fine */
  }
  return { status: r.status, data };
}

export default function Workflows() {
  const [defs, setDefs] = useState<DefRow[]>([]);
  const [blocks, setBlocks] = useState<BlockDef[]>([]);
  const [items, setItems] = useState<RunItem[]>([]);
  const [graph, setGraph] = useState<GraphDef | null>(null);
  const [version, setVersion] = useState("");
  const [pos, setPos] = useState<Record<string, { x: number; y: number }>>({});
  const [selected, setSelected] = useState<string | null>(null);
  const [status, setStatus] = useState<{
    kind: "ok" | "err" | "info";
    msg: string;
  } | null>(null);
  const [runItem, setRunItem] = useState<RunItem | null>(null);
  const [loading, setLoading] = useState(false);
  const [personas, setPersonas] = useState<PersonaInfo[]>([]);
  const [agents, setAgents] = useState<AgentInfo[]>([]);
  const [managePersonas, setManagePersonas] = useState(false);
  // This tab's selected project (per-tab project space). Held so the workflow
  // context can scope to it; execution-in-project flows through a chat session's
  // cwd today, so this primarily persists the selection + offers clone here.
  const { active: wfSession, patchSession: wfPatch } = useSessions();
  const drag = useRef<{ id: string; dx: number; dy: number } | null>(null);

  const refreshLists = useCallback(() => {
    getJSON<{ defs: DefRow[] }>("/api/workflow/defs")
      .then((d) => setDefs(d.defs || []))
      .catch(() => {});
    getJSON<{ items: RunItem[] }>("/api/workflow/items")
      .then((d) => setItems(d.items || []))
      .catch(() => {});
  }, []);

  // The persona list feeds both the per-step persona pickers and the manager;
  // refreshed after any persona create/edit/delete.
  const refreshPersonas = useCallback(() => {
    getJSON<{ personas: PersonaInfo[] }>("/api/chat/personas")
      .then((d) => setPersonas(d.personas || []))
      .catch(() => {});
  }, []);

  useEffect(() => {
    getJSON<{ blocks: BlockDef[] }>("/api/workflow/blocks")
      .then((d) => setBlocks(d.blocks || []))
      .catch(() => {});
    // Delegate suggestions: registered agents. Free text is also accepted, so an
    // empty list (no agents connected) never blocks assigning a delegate.
    getJSON<{ agents: AgentInfo[] }>("/api/agents")
      .then((d) => setAgents(d.agents || []))
      .catch(() => {});
    refreshPersonas();
    refreshLists();
  }, [refreshLists, refreshPersonas]);

  const openDef = useCallback((name: string) => {
    setLoading(true);
    getJSON<DefResponse>(`/api/workflow/defs/${encodeURIComponent(name)}`)
      .then((d) => {
        if (d.error && !d.def) {
          setStatus({ kind: "err", msg: d.error });
          return;
        }
        setGraph(d.def);
        setVersion(d.version);
        setPos(autoLayout(d.def));
        setSelected(null);
        setRunItem(null);
        setStatus(d.valid ? null : { kind: "err", msg: d.error || "invalid" });
      })
      .finally(() => setLoading(false));
  }, []);

  const newDef = useCallback(() => {
    const name = prompt("New workflow name (a-z, 0-9, -_ . ):", "my-workflow");
    if (!name) return;
    const g: GraphDef = {
      name,
      start: "draft",
      nodes: [
        {
          id: "draft",
          block: "author.proposal",
          custom: false,
          produces: "proposal",
          in: [],
        },
      ],
    };
    setGraph(g);
    setVersion("");
    setPos(autoLayout(g));
    setSelected("draft");
    setStatus({ kind: "info", msg: "new workflow (unsaved)" });
  }, []);

  const mutate = useCallback((fn: (g: GraphDef) => GraphDef) => {
    setGraph((g) => (g ? fn(structuredClone(g)) : g));
  }, []);

  const addNode = useCallback(
    (b: BlockDef) => {
      if (!graph) return;
      let i = 1;
      let id = b.name.replace(/[^a-z0-9]+/gi, "_");
      while (graph.nodes.some((n) => n.id === id))
        id = `${b.name.replace(/[^a-z0-9]+/gi, "_")}_${i++}`;
      const node: GNode = {
        id,
        block: b.name,
        custom: b.custom,
        produces: b.produces,
        in: [],
      };
      setPos((p) => ({ ...p, [id]: { x: 60, y: 60 } }));
      mutate((g) => {
        g.nodes.push(node);
        if (!g.start) g.start = id;
        return g;
      });
      setSelected(id);
    },
    [graph, mutate],
  );

  const deleteNode = useCallback(
    (id: string) => {
      mutate((g) => {
        g.nodes = g.nodes.filter((n) => n.id !== id);
        for (const n of g.nodes) {
          if (n.next === id) n.next = undefined;
          if (n.on_pass === id) n.on_pass = undefined;
          if (n.on_fail === id) n.on_fail = undefined;
          n.in = n.in.filter((b) => b.producer !== id);
        }
        if (g.start === id) g.start = g.nodes[0]?.id ?? "";
        return g;
      });
      setSelected(null);
    },
    [mutate],
  );

  const validate = useCallback(async () => {
    if (!graph) return;
    const res = await postJSON<DefResponse>("/api/workflow/validate", {
      yaml: emitWorkflowYaml(graph),
    });
    if (res.data.valid)
      setStatus({
        kind: "ok",
        msg: `valid · version ${res.data.version.slice(0, 12)}`,
      });
    else setStatus({ kind: "err", msg: res.data.error || "invalid" });
  }, [graph]);

  const save = useCallback(async () => {
    if (!graph) return;
    const res = await postJSON<{
      name?: string;
      version?: string;
      error?: string;
      current_version?: string;
    }>("/api/workflow/save", {
      name: graph.name,
      yaml: emitWorkflowYaml(graph),
      prev_version: version,
    });
    if (res.status === 200 && res.data.version) {
      setVersion(res.data.version);
      setStatus({
        kind: "ok",
        msg: `saved · version ${res.data.version.slice(0, 12)}`,
      });
      refreshLists();
    } else if (res.status === 409) {
      setStatus({
        kind: "err",
        msg: `conflict: on-disk version ${(res.data.current_version || "").slice(0, 12) || "differs"} — reload first`,
      });
    } else {
      setStatus({
        kind: "err",
        msg: res.data.error || `save failed (${res.status})`,
      });
    }
  }, [graph, version, refreshLists]);

  /* node dragging */
  const onNodeDown = (id: string, e: React.MouseEvent) => {
    e.stopPropagation();
    setSelected(id);
    const p = pos[id] || { x: 0, y: 0 };
    drag.current = { id, dx: e.clientX - p.x, dy: e.clientY - p.y };
  };
  useEffect(() => {
    const move = (e: MouseEvent) => {
      if (!drag.current) return;
      const { id, dx, dy } = drag.current;
      setPos((p) => ({
        ...p,
        [id]: {
          x: Math.max(0, e.clientX - dx),
          y: Math.max(0, e.clientY - dy),
        },
      }));
    };
    const up = () => {
      drag.current = null;
    };
    window.addEventListener("mousemove", move);
    window.addEventListener("mouseup", up);
    return () => {
      window.removeEventListener("mousemove", move);
      window.removeEventListener("mouseup", up);
    };
  }, []);

  const sel = graph?.nodes.find((n) => n.id === selected) || null;
  // The editor shows the CURRENT on-disk def. A run is pinned to a specific
  // version; only when that matches the open def does the graph truly depict
  // what the run is executing — so only then do we highlight the current stage.
  const runMatchesOpenDef = !!(
    runItem &&
    graph &&
    runItem.workflow === graph.name &&
    !!version &&
    runItem.version === version
  );
  const activeStage =
    runMatchesOpenDef && graph?.nodes.some((n) => n.id === runItem!.stage)
      ? runItem!.stage
      : null;

  const center = (id: string) => {
    const p = pos[id];
    return p ? { x: p.x + NODE_W / 2, y: p.y + NODE_H / 2 } : { x: 0, y: 0 };
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", height: "100%" }}>
      <ProjectPicker
        key={wfSession?.id}
        storageKey={`aimee_session_project_${wfSession?.id ?? ""}`}
        onChange={(sel: ProjectSelection | null) => {
          const r = sel ? `${sel.root}/${sel.project}` : "";
          if (wfSession) wfPatch(wfSession.id, { projectRoot: r, projectName: sel?.project ?? "" });
        }}
      />
      <div
        style={{
          display: "flex",
          gap: 12,
          // Fill the remaining height below the project bar. minWidth:0 on the
          // center keeps the 1600px canvas from collapsing the side rails.
          flex: 1,
          minHeight: 0,
          fontFamily: "system-ui",
        }}
      >
      {/* left rail: defs + palette + run items */}
      <div
        style={{
          width: 230,
          flexShrink: 0,
          overflowY: "auto",
          display: "flex",
          flexDirection: "column",
          gap: 10,
        }}
      >
        <Panel title="Workflows" count={defs.length}>
          <button onClick={newDef} style={btn}>
            + New
          </button>
          <div style={{ marginTop: 6 }}>
            {defs.map((d) => (
              <div
                key={d.name}
                onClick={() => openDef(d.name)}
                style={{
                  ...row,
                  fontWeight: graph?.name === d.name ? 600 : 400,
                }}
              >
                <span>{d.name}</span>
                <Badge
                  label={d.valid ? "ok" : "bad"}
                  variant={d.valid ? "success" : "error"}
                />
              </div>
            ))}
          </div>
        </Panel>
        <Panel title="Blocks" count={blocks.length}>
          {blocks.map((b) => (
            <div
              key={b.name}
              onClick={() => addNode(b)}
              title={`produces ${b.produces}`}
              style={row}
            >
              <span>{b.name}</span>
              <Badge
                label={b.custom ? "custom" : b.produces}
                variant={b.custom ? "info" : "neutral"}
              />
            </div>
          ))}
        </Panel>
        <Panel title="Runs" count={items.length}>
          {items.map((it) => (
            <div
              key={it.id}
              onClick={() => {
                setRunItem(it);
                if (it.workflow !== graph?.name) openDef(it.workflow);
              }}
              style={{ ...row, fontWeight: runItem?.id === it.id ? 600 : 400 }}
            >
              <span>
                {it.workflow}/{it.stage}
              </span>
              <Badge
                label={it.state}
                variant={
                  it.state === "accepted"
                    ? "success"
                    : it.state === "active"
                      ? "running"
                      : "neutral"
                }
              />
            </div>
          ))}
        </Panel>
        <Panel title="Personas" count={personas.length}>
          <button onClick={() => setManagePersonas(true)} style={btn}>
            Manage personas
          </button>
          <div style={{ marginTop: 6 }}>
            {personas.map((p) => (
              <div
                key={p.name}
                style={row}
                title={p.description || p.name}
                onClick={() => setManagePersonas(true)}
              >
                <span>{p.name}</span>
                {p.builtin && <Badge label="builtin" variant="neutral" />}
              </div>
            ))}
          </div>
        </Panel>
      </div>

      {/* center: canvas. minWidth:0 lets this flex item shrink below the 1600px
          SVG canvas's intrinsic width (the canvas scrolls inside its overflow
          box); without it the center refused to shrink and squeezed the side
          rails to zero width — the page looked like just the canvas + buttons. */}
      <div style={{ flex: 1, minWidth: 0, display: "flex", flexDirection: "column" }}>
        <div
          style={{
            display: "flex",
            alignItems: "center",
            gap: 8,
            marginBottom: 6,
          }}
        >
          <strong>{graph ? graph.name : "no workflow open"}</strong>
          {version && (
            <Badge label={`v ${version.slice(0, 8)}`} variant="neutral" />
          )}
          <button onClick={validate} disabled={!graph} style={btn}>
            Validate
          </button>
          <button
            onClick={save}
            disabled={!graph}
            style={{ ...btn, background: "#2563eb", color: "#fff" }}
          >
            Save
          </button>
          {status && (
            <span
              style={{
                color:
                  status.kind === "err"
                    ? "#c00"
                    : status.kind === "ok"
                      ? "#070"
                      : "#666",
                fontSize: 13,
              }}
            >
              {status.msg}
            </span>
          )}
          <Spinner loading={loading} text="loading…" />
        </div>
        <div
          style={{
            flex: 1,
            border: "1px solid #ddd",
            borderRadius: 6,
            overflow: "auto",
            background: "#fafafa",
          }}
          onClick={() => setSelected(null)}
        >
          <svg width={1600} height={1000} style={{ display: "block" }}>
            <defs>
              <marker
                id="arrow"
                markerWidth="8"
                markerHeight="8"
                refX="7"
                refY="3"
                orient="auto"
              >
                <path d="M0,0 L7,3 L0,6 Z" fill="#888" />
              </marker>
            </defs>
            {graph?.nodes.map((n) =>
              (["next", "on_pass", "on_fail"] as const).map((edge) => {
                const t = n[edge];
                if (!t || !pos[t]) return null;
                const a = center(n.id),
                  b = center(t);
                const color =
                  edge === "on_pass"
                    ? "#22a06b"
                    : edge === "on_fail"
                      ? "#d4564f"
                      : "#888";
                return (
                  <line
                    key={`${n.id}-${edge}`}
                    x1={a.x}
                    y1={a.y}
                    x2={b.x}
                    y2={b.y}
                    stroke={color}
                    strokeWidth={1.5}
                    markerEnd="url(#arrow)"
                    strokeDasharray={edge === "next" ? undefined : "5,3"}
                  />
                );
              }),
            )}
            {graph?.nodes.flatMap((n) =>
              n.in.map((b) => {
                if (!pos[b.producer]) return null;
                const a = center(b.producer),
                  c = center(n.id);
                return (
                  <line
                    key={`${n.id}-in-${b.input}`}
                    x1={a.x}
                    y1={a.y}
                    x2={c.x}
                    y2={c.y}
                    stroke="#bbb"
                    strokeWidth={1}
                    strokeDasharray="2,3"
                  />
                );
              }),
            )}
            {graph?.nodes.map((n) => {
              const p = pos[n.id] || { x: 0, y: 0 };
              const isSel = n.id === selected;
              const isActive = n.id === activeStage;
              return (
                <g
                  key={n.id}
                  transform={`translate(${p.x},${p.y})`}
                  style={{ cursor: "grab" }}
                  onMouseDown={(e) => onNodeDown(n.id, e)}
                  onClick={(e) => e.stopPropagation()}
                >
                  <rect
                    width={NODE_W}
                    height={NODE_H}
                    rx={6}
                    fill={isActive ? "#fff7e0" : "#fff"}
                    stroke={isSel ? "#2563eb" : isActive ? "#e0a800" : "#ccc"}
                    strokeWidth={isSel || isActive ? 2 : 1}
                  />
                  <text x={8} y={20} fontSize={13} fontWeight={600} fill="#222">
                    {nodeTitle(n)}
                  </text>
                  <text x={8} y={38} fontSize={11} fill="#777">
                    {n.block}
                    {n.custom ? " ⚙" : ""}
                  </text>
                  <text x={8} y={50} fontSize={10} fill="#aaa">
                    {participantSummary(n) || `→ ${n.produces}`}
                  </text>
                  {n.id === graph?.start && (
                    <circle cx={NODE_W - 10} cy={10} r={4} fill="#22a06b" />
                  )}
                </g>
              );
            })}
          </svg>
        </div>
      </div>

      {/* right: inspector */}
      <div style={{ width: 270, flexShrink: 0, overflowY: "auto" }}>
        <Panel title={sel ? `Node · ${sel.id}` : "Inspector"}>
          {!sel && (
            <div style={{ color: "#888", fontSize: 13 }}>
              Select a node, or click a block to add one.
            </div>
          )}
          {sel && graph && (
            <NodeInspector
              node={sel}
              graph={graph}
              mutate={mutate}
              onDelete={() => deleteNode(sel.id)}
              personas={personas}
              agents={agents}
            />
          )}
        </Panel>
        {runItem && (
          <Panel title="Run state">
            <Field k="workflow" v={runItem.workflow} />
            <Field k="stage" v={runItem.stage} />
            <Field k="state" v={runItem.state} />
            <Field k="pause" v={runItem.pause_reason || "—"} />
            <Field k="version" v={runItem.version.slice(0, 12)} />
            {graph &&
              runItem.workflow === graph.name &&
              runItem.version &&
              version &&
              runItem.version !== version && (
                <div style={{ color: "#c80", fontSize: 12, marginTop: 6 }}>
                  ⚠ This run is pinned to version {runItem.version.slice(0, 12)}
                  , but the editor is showing the <b>current</b> def (
                  {version.slice(0, 12)}). The graph may not match what the run
                  is executing, so the stage highlight is hidden until they
                  match.
                </div>
              )}
          </Panel>
        )}
      </div>

      {managePersonas && (
        <PersonaManager
          personas={personas}
          onClose={() => setManagePersonas(false)}
          onChanged={refreshPersonas}
        />
      )}
      </div>
    </div>
  );
}

function Field({ k, v }: { k: string; v: string }) {
  return (
    <div
      style={{
        display: "flex",
        justifyContent: "space-between",
        fontSize: 13,
        padding: "2px 0",
      }}
    >
      <span style={{ color: "#888" }}>{k}</span>
      <span style={{ fontFamily: "monospace" }}>{v}</span>
    </div>
  );
}

function NodeInspector({
  node,
  graph,
  mutate,
  onDelete,
  personas,
  agents,
}: {
  node: GNode;
  graph: GraphDef;
  mutate: (fn: (g: GraphDef) => GraphDef) => void;
  onDelete: () => void;
  personas: PersonaInfo[];
  agents: AgentInfo[];
}) {
  // A roundtable runs a panel of lenses (several persona+delegate participants);
  // every other action runs a single persona on a single delegate.
  const isMulti = node.block === ROUNDTABLE_BLOCK;

  const [title, setTitle] = useState("");
  const [task, setTask] = useState("");
  const [parts, setParts] = useState<Participant[]>([]);
  const [quorum, setQuorum] = useState(0);
  const [showAdv, setShowAdv] = useState(false);
  const [paramsText, setParamsText] = useState("");
  const [paramsErr, setParamsErr] = useState("");

  // Re-hydrate the structured fields from the node whenever the selection changes.
  useEffect(() => {
    const p = (node.params || {}) as Record<string, unknown>;
    setTitle(typeof p.title === "string" ? p.title : "");
    setTask(typeof p.task === "string" ? p.task : "");
    let ps = readParticipants(node);
    if (isMulti) {
      while (ps.length < 2) ps = [...ps, { persona: "", delegate: "" }];
    } else {
      ps = ps.length ? [ps[0]] : [{ persona: "", delegate: "" }];
    }
    setParts(ps);
    setQuorum(typeof p.quorum === "number" ? p.quorum : 0);
    setParamsText(node.params ? JSON.stringify(node.params, null, 2) : "");
    setParamsErr("");
    setShowAdv(false);
  }, [node.id]); // eslint-disable-line react-hooks/exhaustive-deps

  // Serialize the structured fields back into the node's params, preserving any
  // params the form doesn't manage (e.g. freeze base_branch, gate.human policy).
  //   roundtable -> panel.required = delegates (engine-consumed, validator needs
  //                 >=2), panel.personas = the lenses, quorum
  //   single     -> persona + delegate (forward params; honored once the
  //                 author/implement executors read them)
  const commitStep = (next: {
    title?: string;
    task?: string;
    parts?: Participant[];
    quorum?: number;
  }) => {
    const t = next.title ?? title;
    const tk = next.task ?? task;
    const pr = next.parts ?? parts;
    const q = next.quorum ?? quorum;
    if (next.title !== undefined) setTitle(next.title);
    if (next.task !== undefined) setTask(next.task);
    if (next.parts !== undefined) setParts(next.parts);
    if (next.quorum !== undefined) setQuorum(next.quorum);
    mutate((g) => {
      const n = g.nodes.find((x) => x.id === node.id);
      if (!n) return g;
      const params: Record<string, unknown> = { ...(n.params || {}) };
      if (t.trim()) params.title = t;
      else delete params.title;
      if (tk.trim()) params.task = tk;
      else delete params.task;
      const cleaned = pr
        .map((x) => ({ persona: x.persona.trim(), delegate: x.delegate.trim() }))
        .filter((x) => x.persona || x.delegate);
      if (isMulti) {
        delete params.persona;
        delete params.delegate;
        delete params.participants;
        const panel: Record<string, unknown> = {
          ...((params.panel as Record<string, unknown>) || {}),
        };
        const required = cleaned
          .map((x) => x.delegate || x.persona)
          .filter(Boolean);
        const lenses = cleaned.map((x) => x.persona).filter(Boolean);
        if (required.length) panel.required = required;
        else delete panel.required;
        if (lenses.length) panel.personas = lenses;
        else delete panel.personas;
        if (Object.keys(panel).length) params.panel = panel;
        else delete params.panel;
        if (q > 0) params.quorum = q;
        else delete params.quorum;
      } else {
        delete params.panel;
        delete params.quorum;
        delete params.participants;
        const one = cleaned[0];
        if (one?.persona) params.persona = one.persona;
        else delete params.persona;
        if (one?.delegate) params.delegate = one.delegate;
        else delete params.delegate;
      }
      n.params = Object.keys(params).length ? params : undefined;
      return g;
    });
  };

  const setPart = (i: number, field: keyof Participant, v: string) =>
    commitStep({
      parts: parts.map((p, idx) => (idx === i ? { ...p, [field]: v } : p)),
    });
  const addPart = () =>
    commitStep({ parts: [...parts, { persona: "", delegate: "" }] });
  const delPart = (i: number) =>
    commitStep({ parts: parts.filter((_, idx) => idx !== i) });

  const setEdge = (edge: "next" | "on_pass" | "on_fail", v: string) =>
    mutate((g) => {
      const n = g.nodes.find((x) => x.id === node.id);
      if (n) n[edge] = v || undefined;
      return g;
    });
  const setStart = () =>
    mutate((g) => {
      g.start = node.id;
      return g;
    });
  const others = graph.nodes.filter((n) => n.id !== node.id);

  const commitParams = (text: string) => {
    setParamsText(text);
    if (!text.trim()) {
      setParamsErr("");
      mutate((g) => {
        const n = g.nodes.find((x) => x.id === node.id);
        if (n) n.params = undefined;
        return g;
      });
      return;
    }
    try {
      const obj = JSON.parse(text) as Record<string, unknown>;
      setParamsErr("");
      mutate((g) => {
        const n = g.nodes.find((x) => x.id === node.id);
        if (n) n.params = obj;
        return g;
      });
    } catch {
      setParamsErr("invalid JSON");
    }
  };

  const addBinding = () =>
    mutate((g) => {
      const n = g.nodes.find((x) => x.id === node.id);
      if (n)
        n.in.push({
          input: "src",
          producer: others[0]?.id || "",
          output: "out",
        });
      return g;
    });
  const setBinding = (i: number, field: keyof Binding, v: string) =>
    mutate((g) => {
      const n = g.nodes.find((x) => x.id === node.id);
      if (n) n.in[i] = { ...n.in[i], [field]: v };
      return g;
    });
  const delBinding = (i: number) =>
    mutate((g) => {
      const n = g.nodes.find((x) => x.id === node.id);
      if (n) n.in.splice(i, 1);
      return g;
    });

  return (
    <div style={{ fontSize: 13 }}>
      <label style={lbl}>Step title</label>
      <input
        value={title}
        placeholder={node.id}
        onChange={(e) => commitStep({ title: e.target.value })}
        style={{ ...inp, width: "100%" }}
      />

      <label style={{ ...lbl, marginTop: 8 }}>What this step should do</label>
      <textarea
        value={task}
        placeholder="Describe the task for this step…"
        onChange={(e) => commitStep({ task: e.target.value })}
        rows={4}
        style={{ ...inp, width: "100%" }}
      />

      <label style={{ ...lbl, marginTop: 8 }}>
        {isMulti ? "Panel — personas & delegates" : "Persona & delegate"}
      </label>
      {parts.map((p, i) => (
        <div key={i} style={{ display: "flex", gap: 4, marginBottom: 4 }}>
          <input
            list="wf-persona-opts"
            value={p.persona}
            placeholder="persona"
            onChange={(e) => setPart(i, "persona", e.target.value)}
            style={{ ...inp, flex: 1, minWidth: 0 }}
          />
          <input
            list="wf-delegate-opts"
            value={p.delegate}
            placeholder="delegate"
            onChange={(e) => setPart(i, "delegate", e.target.value)}
            style={{ ...inp, flex: 1, minWidth: 0 }}
          />
          {(isMulti || parts.length > 1) && (
            <button onClick={() => delPart(i)} style={btnSmall} title="remove">
              ×
            </button>
          )}
        </div>
      ))}
      {isMulti && (
        <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
          <button onClick={addPart} style={btnSmall}>
            + participant
          </button>
          <label style={{ ...lbl, margin: 0 }}>
            quorum&nbsp;
            <input
              type="number"
              min={0}
              value={quorum || ""}
              placeholder="all"
              onChange={(e) =>
                commitStep({ quorum: Number(e.target.value) || 0 })
              }
              style={{ ...inp, width: 56 }}
            />
          </label>
        </div>
      )}
      <datalist id="wf-persona-opts">
        {personas.map((p) => (
          <option key={p.name} value={p.name} />
        ))}
      </datalist>
      <datalist id="wf-delegate-opts">
        {/* "$random" picks a random enabled roster agent at run time. */}
        <option value="$random">Random (pick a delegate at random)</option>
        {agents.map((a) => (
          <option key={a.name} value={a.name} />
        ))}
      </datalist>

      <div style={{ borderTop: "1px solid #eee", margin: "10px 0 6px" }} />
      <Field k="block" v={node.block + (node.custom ? " (custom)" : "")} />
      <Field k="produces" v={node.produces} />
      <div style={{ margin: "6px 0" }}>
        <button
          onClick={setStart}
          disabled={graph.start === node.id}
          style={btn}
        >
          {graph.start === node.id ? "start node ✓" : "set as start"}
        </button>
      </div>

      <label style={lbl}>Inputs</label>
      {node.in.map((b, i) => (
        <div key={i} style={{ display: "flex", gap: 4, marginBottom: 4 }}>
          <input
            value={b.input}
            onChange={(e) => setBinding(i, "input", e.target.value)}
            style={{ ...inp, width: 60 }}
          />
          <select
            value={b.producer}
            onChange={(e) => setBinding(i, "producer", e.target.value)}
            style={inp}
          >
            <option value="">—</option>
            {others.map((o) => (
              <option key={o.id} value={o.id}>
                {o.id}
              </option>
            ))}
          </select>
          <button onClick={() => delBinding(i)} style={btnSmall}>
            ×
          </button>
        </div>
      ))}
      <button onClick={addBinding} style={btnSmall}>
        + input
      </button>

      {(["next", "on_pass", "on_fail"] as const).map((edge) => (
        <div key={edge} style={{ marginTop: 6 }}>
          <label style={lbl}>{edge}</label>
          <select
            value={node[edge] || ""}
            onChange={(e) => setEdge(edge, e.target.value)}
            style={{ ...inp, width: "100%" }}
          >
            <option value="">—</option>
            {others.map((o) => (
              <option key={o.id} value={o.id}>
                {o.id}
              </option>
            ))}
          </select>
        </div>
      ))}

      <div style={{ marginTop: 10 }}>
        <button
          onClick={() => {
            const nv = !showAdv;
            setShowAdv(nv);
            if (nv) {
              setParamsText(node.params ? JSON.stringify(node.params, null, 2) : "");
              setParamsErr("");
            }
          }}
          style={btnSmall}
        >
          {showAdv ? "▾ Advanced (raw params)" : "▸ Advanced (raw params)"}
        </button>
      </div>
      {showAdv && (
        <>
          <label style={{ ...lbl, marginTop: 6 }}>params (JSON)</label>
          <textarea
            value={paramsText}
            onChange={(e) => commitParams(e.target.value)}
            rows={6}
            style={{
              ...inp,
              width: "100%",
              fontFamily: "monospace",
              fontSize: 12,
            }}
          />
          {paramsErr && (
            <div style={{ color: "#c00", fontSize: 12 }}>{paramsErr}</div>
          )}
          <div style={{ color: "#999", fontSize: 11, marginTop: 2 }}>
            Editing raw params? Reselect the step to refresh the fields above.
          </div>
        </>
      )}

      <button
        onClick={onDelete}
        style={{ ...btn, marginTop: 10, color: "#c00", borderColor: "#e0a0a0" }}
      >
        Delete node
      </button>
    </div>
  );
}

/* ---- persona manager (list + create/edit/delete over /api/chat/personas) ---- */

const DELEGATE_POLICIES = ["full", "readonly", "none"];

function PersonaManager({
  personas,
  onClose,
  onChanged,
}: {
  personas: PersonaInfo[];
  onClose: () => void;
  onChanged: () => void;
}) {
  // sel: null = nothing selected, "" = composing a new persona, else a name.
  const [sel, setSel] = useState<string | null>(null);
  const [form, setForm] = useState<PersonaDef | null>(null);
  const [status, setStatus] = useState<{ kind: "ok" | "err"; msg: string } | null>(
    null,
  );
  const [busy, setBusy] = useState(false);

  const load = useCallback((name: string) => {
    setBusy(true);
    setStatus(null);
    getJSON<PersonaDef & { error?: string }>(
      `/api/chat/personas/${encodeURIComponent(name)}`,
    )
      .then((d) => {
        if (d.error) {
          setStatus({ kind: "err", msg: d.error });
          return;
        }
        setForm({ ...d, roles: d.roles || [] });
        setSel(name);
      })
      .catch(() => setStatus({ kind: "err", msg: "load failed" }))
      .finally(() => setBusy(false));
  }, []);

  const newPersona = () => {
    setSel("");
    setStatus(null);
    setForm({
      name: "",
      description: "",
      delegates: "full",
      roles: [],
      check_role: "",
      check_marker: "",
      persona: "",
      principles: "",
      brief: "",
    });
  };

  const upd = (patch: Partial<PersonaDef>) =>
    setForm((f) => (f ? { ...f, ...patch } : f));

  const save = async () => {
    if (!form) return;
    const name = form.name.trim();
    if (!/^[a-z0-9][a-z0-9_-]*$/i.test(name)) {
      setStatus({ kind: "err", msg: "name must be alphanumeric, - or _" });
      return;
    }
    setBusy(true);
    // Send every field: the server rebuilds the persona from the body, so omitting
    // a field would drop it. check_role/check_marker round-trip from the load.
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
    const res = await sendJSON<{ name?: string; error?: string }>(
      "PUT",
      `/api/chat/personas/${encodeURIComponent(name)}`,
      body,
    );
    setBusy(false);
    if (res.status === 200 && res.data.name) {
      setStatus({ kind: "ok", msg: `saved “${res.data.name}”` });
      onChanged();
      setSel(name);
    } else {
      setStatus({
        kind: "err",
        msg: res.data.error || `save failed (${res.status})`,
      });
    }
  };

  const del = async () => {
    if (!form || sel === null || sel === "") return;
    if (!confirm(`Delete persona “${form.name}”? Built-ins reset to default.`))
      return;
    setBusy(true);
    const res = await sendJSON<{ deleted?: boolean; error?: string }>(
      "DELETE",
      `/api/chat/personas/${encodeURIComponent(form.name)}`,
    );
    setBusy(false);
    if (res.status === 200) {
      setStatus({ kind: "ok", msg: `removed “${form.name}”` });
      onChanged();
      setForm(null);
      setSel(null);
    } else {
      setStatus({
        kind: "err",
        msg: res.data.error || `delete failed (${res.status})`,
      });
    }
  };

  return (
    <div style={modalBackdrop} onClick={onClose}>
      <div style={modalCard} onClick={(e) => e.stopPropagation()}>
        <div style={{ display: "flex", alignItems: "center", marginBottom: 10 }}>
          <strong style={{ fontSize: 15 }}>Personas</strong>
          <button onClick={onClose} style={{ ...btnSmall, marginLeft: "auto" }}>
            ✕ Close
          </button>
        </div>
        <div style={{ display: "flex", gap: 14, minHeight: 380 }}>
          <div
            style={{
              width: 170,
              borderRight: "1px solid #eee",
              paddingRight: 10,
              overflowY: "auto",
            }}
          >
            <button onClick={newPersona} style={{ ...btn, width: "100%" }}>
              + New persona
            </button>
            <div style={{ marginTop: 6 }}>
              {personas.map((p) => (
                <div
                  key={p.name}
                  onClick={() => load(p.name)}
                  style={{ ...row, fontWeight: sel === p.name ? 600 : 400 }}
                  title={p.description || p.name}
                >
                  <span>{p.name}</span>
                  {p.builtin && <Badge label="builtin" variant="neutral" />}
                </div>
              ))}
            </div>
          </div>
          <div style={{ flex: 1, overflowY: "auto", paddingRight: 4 }}>
            {!form && (
              <div style={{ color: "#888", fontSize: 13 }}>
                Select a persona to edit, or create a new one.
              </div>
            )}
            {form && (
              <div style={{ fontSize: 13 }}>
                <label style={lbl}>name</label>
                <input
                  value={form.name}
                  disabled={sel !== ""}
                  placeholder="my-persona"
                  onChange={(e) => upd({ name: e.target.value })}
                  style={{ ...inp, width: "100%" }}
                />
                {form.builtin && (
                  <div style={{ color: "#a60", fontSize: 11, marginTop: 2 }}>
                    Built-in — saving writes an override; removing resets to the
                    default.
                  </div>
                )}

                <label style={{ ...lbl, marginTop: 8 }}>description</label>
                <input
                  value={form.description || ""}
                  onChange={(e) => upd({ description: e.target.value })}
                  style={{ ...inp, width: "100%" }}
                />

                <label style={{ ...lbl, marginTop: 8 }}>delegate policy</label>
                <select
                  value={form.delegates || "full"}
                  onChange={(e) => upd({ delegates: e.target.value })}
                  style={{ ...inp, width: "100%" }}
                >
                  {DELEGATE_POLICIES.map((d) => (
                    <option key={d} value={d}>
                      {d}
                    </option>
                  ))}
                </select>

                <label style={{ ...lbl, marginTop: 8 }}>
                  roles (comma-separated)
                </label>
                <input
                  value={(form.roles || []).join(", ")}
                  onChange={(e) =>
                    upd({
                      roles: e.target.value
                        .split(",")
                        .map((s) => s.trim())
                        .filter(Boolean),
                    })
                  }
                  style={{ ...inp, width: "100%" }}
                />

                <label style={{ ...lbl, marginTop: 8 }}>
                  persona (system prompt)
                </label>
                <textarea
                  value={form.persona || ""}
                  onChange={(e) => upd({ persona: e.target.value })}
                  rows={6}
                  style={{ ...inp, width: "100%" }}
                />

                <label style={{ ...lbl, marginTop: 8 }}>principles</label>
                <textarea
                  value={form.principles || ""}
                  onChange={(e) => upd({ principles: e.target.value })}
                  rows={4}
                  style={{ ...inp, width: "100%" }}
                />

                <label style={{ ...lbl, marginTop: 8 }}>brief (session hints)</label>
                <textarea
                  value={form.brief || ""}
                  onChange={(e) => upd({ brief: e.target.value })}
                  rows={3}
                  style={{ ...inp, width: "100%" }}
                />

                <div
                  style={{
                    display: "flex",
                    gap: 8,
                    marginTop: 12,
                    alignItems: "center",
                  }}
                >
                  <button
                    onClick={save}
                    disabled={busy}
                    style={{ ...btn, background: "#2563eb", color: "#fff" }}
                  >
                    Save
                  </button>
                  {sel !== "" && (
                    <button
                      onClick={del}
                      disabled={busy}
                      style={{ ...btn, color: "#c00", borderColor: "#e0a0a0" }}
                    >
                      {form.builtin ? "Reset to default" : "Delete"}
                    </button>
                  )}
                  {status && (
                    <span
                      style={{
                        fontSize: 12,
                        color: status.kind === "err" ? "#c00" : "#070",
                      }}
                    >
                      {status.msg}
                    </span>
                  )}
                </div>
              </div>
            )}
          </div>
        </div>
      </div>
    </div>
  );
}

/* ---- inline styles ---- */
const btn: React.CSSProperties = {
  fontSize: 13,
  padding: "4px 10px",
  border: "1px solid #ccc",
  borderRadius: 4,
  background: "#fff",
  cursor: "pointer",
};
const btnSmall: React.CSSProperties = {
  ...btn,
  padding: "2px 6px",
  fontSize: 12,
};
const row: React.CSSProperties = {
  display: "flex",
  justifyContent: "space-between",
  alignItems: "center",
  padding: "4px 2px",
  cursor: "pointer",
  fontSize: 13,
};
const lbl: React.CSSProperties = {
  display: "block",
  color: "#888",
  fontSize: 12,
  margin: "4px 0 2px",
};
const inp: React.CSSProperties = {
  fontSize: 13,
  padding: "3px 5px",
  border: "1px solid #ccc",
  borderRadius: 4,
};
const modalBackdrop: React.CSSProperties = {
  position: "fixed",
  inset: 0,
  background: "rgba(0,0,0,0.35)",
  display: "flex",
  alignItems: "center",
  justifyContent: "center",
  zIndex: 1000,
};
const modalCard: React.CSSProperties = {
  background: "#fff",
  borderRadius: 8,
  padding: 18,
  width: 720,
  maxWidth: "92vw",
  maxHeight: "88vh",
  overflow: "hidden",
  boxShadow: "0 10px 40px rgba(0,0,0,0.25)",
  fontFamily: "system-ui",
};
