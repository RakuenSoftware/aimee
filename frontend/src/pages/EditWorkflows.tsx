import { useEffect, useState, useCallback, useRef } from "react";
import { Panel, Badge, Spinner, InlineStatus } from "@rakuensoftware/smoothgui";
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
  // custom delegate blocks carry these (editable in the block editor):
  consumes?: string;
  persona?: string;
  prompt?: string;
}

// Artifact types a custom block may consume/produce (mirrors ARTIFACT_NAMES).
const ARTIFACT_TYPES = [
  "none",
  "proposal",
  "plan",
  "branch",
  "frozen_diff",
  "pr",
  "verdict",
  "approval",
  "intent",
];
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
/* ---- personas + delegates (the per-step assignment options) ---- */

interface PersonaInfo {
  name: string;
  description?: string;
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
const IMPLEMENT_BLOCK = "implement";
// foreach.workflow fans each packet out to a CHILD workflow (params.workflow) —
// on the canvas it renders as a callout to that workflow, not a normal block.
const FOREACH_BLOCK = "foreach.workflow";

// The child workflow a foreach.workflow node calls out to ("" when unset).
function childWorkflowName(node: GNode): string {
  const w = (node.params as Record<string, unknown> | undefined)?.workflow;
  return typeof w === "string" ? w : "";
}

// Read an implement node's TDD test-author participant (test_persona/test_delegate).
function readTddTest(node: GNode): Participant {
  const p = (node.params || {}) as Record<string, unknown>;
  return {
    persona: typeof p.test_persona === "string" ? p.test_persona : "",
    delegate: typeof p.test_delegate === "string" ? p.test_delegate : "",
  };
}

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
// implement nodes in TDD mode also list the test author and a "TDD" marker.
function participantSummary(node: GNode): string {
  const ps = readParticipants(node).filter((p) => p.persona || p.delegate);
  const tddOn =
    node.block === IMPLEMENT_BLOCK &&
    ((node.params as Record<string, unknown> | undefined)?.tdd === true ||
      (node.params as Record<string, unknown> | undefined)?.tdd === "true");
  if (tddOn) {
    const test = readTddTest(node);
    if (test.persona || test.delegate) ps.push(test);
  }
  if (!ps.length) return tddOn ? "TDD" : "";
  const parts = ps
    .slice(0, 2)
    .map((p) =>
      p.persona && p.delegate
        ? `${p.persona}@${p.delegate}`
        : p.persona || p.delegate,
    );
  const summary =
    parts.join(", ") + (ps.length > 2 ? ` +${ps.length - 2}` : "");
  return tddOn ? `${summary} · TDD` : summary;
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

// A custom delegate block being created/edited in the block editor.
interface BlockForm {
  name: string;
  consumes: string;
  produces: string;
  persona: string;
  prompt: string;
  isNew: boolean;
}

export default function EditWorkflows() {
  const [defs, setDefs] = useState<DefRow[]>([]);
  const [blocks, setBlocks] = useState<BlockDef[]>([]);
  const [graph, setGraph] = useState<GraphDef | null>(null);
  const [version, setVersion] = useState("");
  const [pos, setPos] = useState<Record<string, { x: number; y: number }>>({});
  const [selected, setSelected] = useState<string | null>(null);
  const [status, setStatus] = useState<{
    kind: "ok" | "err" | "info";
    msg: string;
  } | null>(null);
  const [loading, setLoading] = useState(false);
  const [personas, setPersonas] = useState<PersonaInfo[]>([]);
  const [editBlock, setEditBlock] = useState<BlockForm | null>(null);
  const [blockStatus, setBlockStatus] = useState<string>("");
  const [agents, setAgents] = useState<AgentInfo[]>([]);
  // This tab's selected project (per-tab project space). Held so the workflow
  // context can scope to it; execution-in-project flows through a chat session's
  // cwd today, so this primarily persists the selection + offers clone here.
  const { active: wfSession, patchSession: wfPatch } = useSessions();
  const drag = useRef<{ id: string; dx: number; dy: number } | null>(null);

  const refreshLists = useCallback(() => {
    getJSON<{ defs: DefRow[] }>("/api/workflow/defs")
      .then((d) => setDefs(d.defs || []))
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

  // ---- custom block editor (delegate blocks; same CRUD pattern as personas) ----
  const newBlock = useCallback(() => {
    setBlockStatus("");
    setEditBlock({
      name: "",
      consumes: "none",
      produces: "branch",
      persona: "",
      prompt: "",
      isNew: true,
    });
  }, []);

  const editExistingBlock = useCallback((b: BlockDef) => {
    setBlockStatus("");
    setEditBlock({
      name: b.name,
      consumes: b.consumes || "none",
      produces: b.produces || "branch",
      persona: b.persona || "",
      prompt: b.prompt || "",
      isNew: false,
    });
  }, []);

  const saveBlock = useCallback(async () => {
    if (!editBlock) return;
    const name = editBlock.name.trim();
    if (!/^[a-z0-9][a-z0-9_.-]*$/i.test(name)) {
      setBlockStatus("name must be alphanumeric, - _ or .");
      return;
    }
    if (!editBlock.persona.trim() || !editBlock.prompt.trim()) {
      setBlockStatus("a persona and a prompt are required");
      return;
    }
    const { status: st } = await sendJSON(
      "PUT",
      `/api/workflow/blocks/${encodeURIComponent(name)}`,
      {
        consumes: editBlock.consumes,
        produces: editBlock.produces,
        persona: editBlock.persona,
        prompt: editBlock.prompt,
      },
    );
    if (st >= 200 && st < 300) {
      setBlockStatus("");
      setEditBlock(null);
      refreshLists();
    } else {
      setBlockStatus(`save failed (${st})`);
    }
  }, [editBlock, refreshLists]);

  const deleteBlock = useCallback(async () => {
    if (!editBlock || editBlock.isNew) {
      setEditBlock(null);
      return;
    }
    if (!window.confirm(`Delete custom block “${editBlock.name}”?`)) return;
    const { status: st } = await sendJSON(
      "DELETE",
      `/api/workflow/blocks/${encodeURIComponent(editBlock.name)}`,
    );
    if (st >= 200 && st < 300) {
      setEditBlock(null);
      refreshLists();
    } else {
      setBlockStatus(`delete failed (${st})`);
    }
  }, [editBlock, refreshLists]);

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

  const center = (id: string) => {
    const p = pos[id];
    return p ? { x: p.x + NODE_W / 2, y: p.y + NODE_H / 2 } : { x: 0, y: 0 };
  };

  // Size the SVG to the actual node extents (plus a margin) so the overflow box
  // can scroll to every node. A fixed 1600×1000 canvas clipped the scroll region,
  // so deep workflows (x grows ~230px/depth) or nodes dragged right/down became
  // unreachable. Keep a floor so a small graph still fills the viewport.
  const CANVAS_PAD = 80;
  let canvasW = 1600;
  let canvasH = 1000;
  if (graph) {
    for (const n of graph.nodes) {
      const p = pos[n.id];
      if (!p) continue;
      canvasW = Math.max(canvasW, p.x + NODE_W + CANVAS_PAD);
      canvasH = Math.max(canvasH, p.y + NODE_H + CANVAS_PAD);
    }
  }

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
          <button onClick={newDef} style={btn} title="Create a new workflow definition and open it in the editor.">
            + New
          </button>
          <div style={{ marginTop: 6 }}>
            {defs.map((d) => (
              <div
                key={d.name}
                onClick={() => openDef(d.name)}
                title="Open this workflow definition for editing."
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
          <button onClick={newBlock} style={btn} title="Create a new custom delegate block.">
            + New
          </button>
          <div style={{ marginTop: 6 }}>
            {blocks.map((b) => (
              <div
                key={b.name}
                onClick={() => addNode(b)}
                title={`produces ${b.produces} — click to add to the canvas`}
                style={row}
              >
                <span>{b.name}</span>
                <span style={{ display: "flex", gap: 4, alignItems: "center" }}>
                  {b.custom && b.executor === "delegate" && (
                    <button
                      onClick={(e) => {
                        e.stopPropagation();
                        editExistingBlock(b);
                      }}
                      style={{ ...btnSmall, padding: "0 6px" }}
                      title="edit this custom block"
                    >
                      ✎
                    </button>
                  )}
                  <Badge
                    label={b.custom ? "custom" : b.produces}
                    variant={b.custom ? "info" : "neutral"}
                  />
                </span>
              </div>
            ))}
          </div>
        </Panel>
        {editBlock && (
          <Panel title={editBlock.isNew ? "New custom block" : `Edit block: ${editBlock.name}`}>
            <div style={{ display: "grid", gap: 6 }}>
              <label style={lbl} title="Identifier for this custom block (letters, digits, - _ .); fixed once created.">
                name
                <input
                  style={{ ...inp, width: "100%" }}
                  value={editBlock.name}
                  disabled={!editBlock.isNew}
                  onChange={(e) => setEditBlock({ ...editBlock, name: e.target.value })}
                />
              </label>
              <label style={lbl} title="Artifact type this block takes as input.">
                consumes
                <select
                  style={{ ...inp, width: "100%" }}
                  value={editBlock.consumes}
                  onChange={(e) => setEditBlock({ ...editBlock, consumes: e.target.value })}
                >
                  {ARTIFACT_TYPES.map((a) => (
                    <option key={a} value={a}>
                      {a}
                    </option>
                  ))}
                </select>
              </label>
              <label style={lbl} title="Artifact type this block emits (branch, or none).">
                produces
                <select
                  style={{ ...inp, width: "100%" }}
                  value={editBlock.produces}
                  onChange={(e) => setEditBlock({ ...editBlock, produces: e.target.value })}
                >
                  <option value="branch">branch</option>
                  <option value="none">none</option>
                </select>
              </label>
              <label style={lbl} title="Persona the delegate runs as when this block executes.">
                persona
                <input
                  style={{ ...inp, width: "100%" }}
                  list="wf-persona-opts"
                  value={editBlock.persona}
                  onChange={(e) => setEditBlock({ ...editBlock, persona: e.target.value })}
                />
              </label>
              <label style={lbl} title="Instructions given to the delegate each time this block runs.">
                prompt
                <textarea
                  style={{ ...inp, width: "100%", minHeight: 80, fontFamily: "ui-monospace, monospace" }}
                  value={editBlock.prompt}
                  onChange={(e) => setEditBlock({ ...editBlock, prompt: e.target.value })}
                />
              </label>
              <div style={{ display: "flex", gap: 6, alignItems: "center" }}>
                <button onClick={saveBlock} style={btn} title="Save this custom block definition.">
                  Save
                </button>
                <button
                  onClick={deleteBlock}
                  style={{ ...btn, color: "#b00" }}
                  title={editBlock.isNew ? "Discard this new block." : "Delete this custom block."}
                >
                  {editBlock.isNew ? "Cancel" : "Delete"}
                </button>
                {blockStatus && <span style={{ fontSize: 12, color: "#b00" }}>{blockStatus}</span>}
              </div>
            </div>
          </Panel>
        )}
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
          <button onClick={validate} disabled={!graph} style={btn} title="Check the current workflow for errors without saving.">
            Validate
          </button>
          <button
            onClick={save}
            disabled={!graph}
            style={{ ...btn, background: "#2563eb", color: "#fff" }}
            title="Save the workflow definition (fails if the on-disk version changed)."
          >
            Save
          </button>
          <InlineStatus status={status} />
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
          <svg width={canvasW} height={canvasH} style={{ display: "block" }}>
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
              const child = n.block === FOREACH_BLOCK ? childWorkflowName(n) : "";
              const isCallout = n.block === FOREACH_BLOCK;
              return (
                <g
                  key={n.id}
                  transform={`translate(${p.x},${p.y})`}
                  style={{ cursor: "grab" }}
                  onMouseDown={(e) => onNodeDown(n.id, e)}
                  onClick={(e) => e.stopPropagation()}
                >
                  {/* a callout renders as a stacked card: the child workflow behind it */}
                  {isCallout && (
                    <rect
                      x={4}
                      y={-4}
                      width={NODE_W}
                      height={NODE_H}
                      rx={6}
                      fill="#e0e7ff"
                      stroke="#a5b4fc"
                      strokeWidth={1}
                    />
                  )}
                  <rect
                    width={NODE_W}
                    height={NODE_H}
                    rx={6}
                    fill={isCallout ? "#eef2ff" : "#fff"}
                    stroke={isSel ? "#2563eb" : isCallout ? "#6366f1" : "#ccc"}
                    strokeWidth={isSel ? 2 : 1}
                    strokeDasharray={isCallout ? "6,3" : undefined}
                  />
                  <text x={8} y={20} fontSize={13} fontWeight={600} fill="#222">
                    {nodeTitle(n)}
                  </text>
                  <text x={8} y={38} fontSize={11} fill={isCallout ? "#4f46e5" : "#777"}>
                    {isCallout ? `↳ workflow: ${child || "(unset)"}` : n.block}
                    {n.custom ? " ⚙" : ""}
                  </text>
                  {isCallout && child ? (
                    <text
                      x={8}
                      y={50}
                      fontSize={10}
                      fill="#4f46e5"
                      style={{ cursor: "pointer", textDecoration: "underline" }}
                      onClick={(e) => {
                        e.stopPropagation();
                        openDef(child);
                      }}
                    >
                      open {child} ↗
                    </text>
                  ) : (
                    <text x={8} y={50} fontSize={10} fill="#aaa">
                      {participantSummary(n) || `→ ${n.produces}`}
                    </text>
                  )}
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
              workflows={defs.map((d) => d.name)}
              onOpenWorkflow={openDef}
            />
          )}
        </Panel>
      </div>

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
  workflows,
  onOpenWorkflow,
}: {
  node: GNode;
  graph: GraphDef;
  mutate: (fn: (g: GraphDef) => GraphDef) => void;
  onDelete: () => void;
  personas: PersonaInfo[];
  agents: AgentInfo[];
  workflows: string[];
  onOpenWorkflow: (name: string) => void;
}) {
  // A roundtable runs a panel of lenses (several persona+delegate participants);
  // every other action runs a single persona on a single delegate.
  const isMulti = node.block === ROUNDTABLE_BLOCK;
  // implement steps can opt into TDD: a second (test-author) agent writes the
  // failing tests before the implementer makes them pass.
  const isImplement = node.block === IMPLEMENT_BLOCK;

  const [title, setTitle] = useState("");
  const [task, setTask] = useState("");
  const [parts, setParts] = useState<Participant[]>([]);
  const [quorum, setQuorum] = useState(0);
  const [focus, setFocus] = useState("");
  const [tdd, setTdd] = useState(false);
  const [testPart, setTestPart] = useState<Participant>({
    persona: "",
    delegate: "",
  });
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
    setFocus(typeof p.focus === "string" ? p.focus : "");
    setTdd(p.tdd === true || p.tdd === "true");
    setTestPart(readTddTest(node));
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
    focus?: string;
    tdd?: boolean;
    testPart?: Participant;
  }) => {
    const t = next.title ?? title;
    const tk = next.task ?? task;
    const pr = next.parts ?? parts;
    const q = next.quorum ?? quorum;
    const fc = next.focus ?? focus;
    const td = next.tdd ?? tdd;
    const tp = next.testPart ?? testPart;
    if (next.title !== undefined) setTitle(next.title);
    if (next.task !== undefined) setTask(next.task);
    if (next.parts !== undefined) setParts(next.parts);
    if (next.quorum !== undefined) setQuorum(next.quorum);
    if (next.focus !== undefined) setFocus(next.focus);
    if (next.tdd !== undefined) setTdd(next.tdd);
    if (next.testPart !== undefined) setTestPart(next.testPart);
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
        // review lens: the acceptance/plan gates set this so the panel judges the
        // work AGAINST the ask (completion, quality, missing tests).
        if (fc.trim()) params.focus = fc;
        else delete params.focus;
      } else {
        delete params.panel;
        delete params.quorum;
        delete params.participants;
        const one = cleaned[0];
        if (one?.persona) params.persona = one.persona;
        else delete params.persona;
        if (one?.delegate) params.delegate = one.delegate;
        else delete params.delegate;
        // implement TDD: a second (test-author) agent + the tdd flag. The
        // implementer reuses persona/delegate above; the test author is
        // test_persona/test_delegate. All cleared when TDD is off or unset.
        if (isImplement && td) {
          params.tdd = true;
          const tperson = tp.persona.trim();
          const tdeleg = tp.delegate.trim();
          if (tperson) params.test_persona = tperson;
          else delete params.test_persona;
          if (tdeleg) params.test_delegate = tdeleg;
          else delete params.test_delegate;
        } else {
          delete params.tdd;
          delete params.test_persona;
          delete params.test_delegate;
        }
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
        {isMulti
          ? "Panel — personas & delegates"
          : isImplement && tdd
            ? "Implementation — persona & delegate"
            : "Persona & delegate"}
      </label>
      {parts.map((p, i) => (
        <div key={i} style={{ display: "flex", gap: 4, marginBottom: 4 }}>
          <input
            list="wf-persona-opts"
            value={p.persona}
            placeholder="persona"
            title="Persona this step's agent runs as."
            onChange={(e) => setPart(i, "persona", e.target.value)}
            style={{ ...inp, flex: 1, minWidth: 0 }}
          />
          <input
            list="wf-delegate-opts"
            value={p.delegate}
            placeholder="delegate"
            title="Delegate that carries out this step ($random picks one at run time)."
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
          <button onClick={addPart} style={btnSmall} title="Add another persona/delegate to the review panel.">
            + participant
          </button>
          <label style={{ ...lbl, margin: 0 }} title="How many panelists must pass; blank means all.">
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
      {isMulti && (
        <>
          <label style={{ ...lbl, marginTop: 8 }}>
            Review focus (what the panel judges the work against)
          </label>
          <textarea
            value={focus}
            placeholder="e.g. does this plan satisfy the proposal? / was the proposal completed — quality, missing tests?"
            onChange={(e) => commitStep({ focus: e.target.value })}
            rows={2}
            style={{ ...inp, width: "100%" }}
          />
        </>
      )}
      {isImplement && (
        <div style={{ marginTop: 8 }}>
          <label
            style={{
              ...lbl,
              display: "flex",
              alignItems: "center",
              gap: 6,
              cursor: "pointer",
              margin: "4px 0",
            }}
          >
            <input
              type="checkbox"
              checked={tdd}
              onChange={(e) => commitStep({ tdd: e.target.checked })}
            />
            TDD — a second agent writes the failing tests, then the implementer
            makes them pass
          </label>
          {tdd && (
            <>
              <label style={{ ...lbl, marginTop: 6 }}>
                Test author — persona & delegate
              </label>
              <div style={{ display: "flex", gap: 4, marginBottom: 4 }}>
                <input
                  list="wf-persona-opts"
                  value={testPart.persona}
                  placeholder="persona"
                  onChange={(e) =>
                    commitStep({
                      testPart: { ...testPart, persona: e.target.value },
                    })
                  }
                  style={{ ...inp, flex: 1, minWidth: 0 }}
                />
                <input
                  list="wf-delegate-opts"
                  value={testPart.delegate}
                  placeholder="delegate"
                  onChange={(e) =>
                    commitStep({
                      testPart: { ...testPart, delegate: e.target.value },
                    })
                  }
                  style={{ ...inp, flex: 1, minWidth: 0 }}
                />
              </div>
            </>
          )}
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

      {node.block === FOREACH_BLOCK && (
        <div style={{ margin: "8px 0" }}>
          <label style={lbl}>child workflow (each packet runs it)</label>
          <div style={{ display: "flex", gap: 6, alignItems: "center" }}>
            <select
              value={childWorkflowName(node)}
              title="Workflow each fan-out packet runs."
              onChange={(e) => {
                const v = e.target.value;
                mutate((g) => ({
                  ...g,
                  nodes: g.nodes.map((n) =>
                    n.id === node.id
                      ? { ...n, params: { ...(n.params || {}), workflow: v } }
                      : n,
                  ),
                }));
              }}
              style={{ ...inp, flex: 1 }}
            >
              <option value="">— pick a workflow —</option>
              {workflows
                .filter((w) => w !== graph.name)
                .map((w) => (
                  <option key={w} value={w}>
                    {w}
                  </option>
                ))}
            </select>
            <button
              onClick={() => onOpenWorkflow(childWorkflowName(node))}
              disabled={!childWorkflowName(node)}
              style={btnSmall}
              title="open the child workflow in this editor"
            >
              open ↗
            </button>
          </div>
        </div>
      )}

      <div style={{ borderTop: "1px solid #eee", margin: "10px 0 6px" }} />
      <Field k="block" v={node.block + (node.custom ? " (custom)" : "")} />
      <Field k="produces" v={node.produces} />
      <div style={{ margin: "6px 0" }}>
        <button
          onClick={setStart}
          disabled={graph.start === node.id}
          style={btn}
          title="Make this node the workflow's start node."
        >
          {graph.start === node.id ? "start node ✓" : "set as start"}
        </button>
      </div>

      <label style={lbl} title="Named outputs from other nodes fed into this step.">Inputs</label>
      {node.in.map((b, i) => (
        <div key={i} style={{ display: "flex", gap: 4, marginBottom: 4 }}>
          <input
            value={b.input}
            title="Name this step refers to the input by."
            onChange={(e) => setBinding(i, "input", e.target.value)}
            style={{ ...inp, width: 60 }}
          />
          <select
            value={b.producer}
            title="Which upstream node supplies this input."
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
          <button onClick={() => delBinding(i)} style={btnSmall} title="Remove this input binding.">
            ×
          </button>
        </div>
      ))}
      <button onClick={addBinding} style={btnSmall} title="Add an input binding from another node's output.">
        + input
      </button>

      {(["next", "on_pass", "on_fail"] as const).map((edge) => (
        <div key={edge} style={{ marginTop: 6 }}>
          <label style={lbl}>{edge}</label>
          <select
            value={node[edge] || ""}
            title="Node to go to on this transition."
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
          title="Show/hide the raw params JSON editor."
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
        title="Remove this node from the workflow."
      >
        Delete node
      </button>
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
