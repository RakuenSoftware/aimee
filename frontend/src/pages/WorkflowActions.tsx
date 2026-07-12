import { useCallback, useEffect, useRef, useState } from "react";
import { Panel, Badge, Spinner } from "@rakuensoftware/smoothgui";
import type { BadgeVariant } from "@rakuensoftware/smoothgui";
import { renderMd } from "./chat/markdown";

/* ---- API types (mirror the enriched /api/workflow/items* envelopes) ---- */

interface Item {
  id: string;
  workflow: string;
  version: string;
  stage: string;
  state: string; // active | accepted | rejected | abandoned
  mode: string;
  pause_reason: string;
  repo: string;
  proposal_name?: string;
  pr_ref?: string;
  submitter?: string;
  cum_cost_usd?: number;
  work_item_max_cost_usd?: number;
  override_count?: number;
}
interface WfEvent {
  id: number;
  stage: string;
  kind: string;
  actor: string;
  detail: string;
  cost_usd: number;
  created_at: string;
}

const POLL_MS = 4000;
const DRAFT_KEY = "aimee_proposal_draft"; // cleared on logout (App.tsx)
const SCAFFOLD = `## Goal

## Motivation

## Approach

## Risks

## Tests
`;

interface Draft {
  title: string;
  body: string;
  workflow: string;
  repo: string;
}
const emptyDraft = (): Draft => ({ title: "", body: SCAFFOLD, workflow: "build", repo: "" });
function loadDraft(): Draft {
  try {
    const raw = localStorage.getItem(DRAFT_KEY);
    if (raw) return { ...emptyDraft(), ...(JSON.parse(raw) as Partial<Draft>) };
  } catch {
    /* corrupt/absent draft → start fresh */
  }
  return emptyDraft();
}

async function getJSON<T>(url: string): Promise<T> {
  const r = await fetch(url, { headers: { "X-CSRF-Token": window._csrf || "" } });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
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
    /* empty body ok */
  }
  return { status: r.status, data };
}

// A bare method call (POST/DELETE) with no body — used for the lifecycle actions.
// Returns the status + parsed body (best effort) like postJSON.
async function sendAction<T>(url: string, method: "POST" | "DELETE"): Promise<{ status: number; data: T }> {
  const r = await fetch(url, {
    method,
    headers: { "X-CSRF-Token": window._csrf || "" },
  });
  let data = {} as T;
  try {
    data = (await r.json()) as T;
  } catch {
    /* empty body ok */
  }
  return { status: r.status, data };
}

const isTerminal = (s: string) => s === "accepted" || s === "rejected" || s === "abandoned";

// A human status label + tone from the row's state + pause_reason + stage. Derived
// strictly from the documented enums — no invented "drafting" state.
function statusOf(it: Item): { label: string; variant: BadgeVariant } {
  if (it.state === "accepted") return { label: "merged (accepted)", variant: "success" };
  if (it.state === "rejected") return { label: "rejected", variant: "error" };
  if (it.state === "abandoned") return { label: "abandoned", variant: "neutral" };
  // active:
  switch (it.pause_reason) {
    case "operator_paused":
      return { label: `paused · ${it.stage}`, variant: "warning" };
    case "pending_human":
      return { label: `awaiting approval · ${it.stage}`, variant: "warning" };
    case "ci_pending":
      return { label: "CI running", variant: "running" };
    case "merge_pending":
      return { label: "merging", variant: "running" };
    case "panel_degraded":
    case "panel_unreachable":
      return { label: `parked · ${it.pause_reason}`, variant: "warning" };
    case "budget_exceeded":
      return { label: "parked · budget exceeded", variant: "error" };
    case "failed":
      return { label: "parked · failed", variant: "error" };
    case "max_attempts":
      return { label: "parked · max attempts", variant: "error" };
    case "stuck":
      // The engine can't advance this stage (unresolvable stage / no executor);
      // it needs a human to fix + resume or abandon. Never shown as "running".
      return { label: `stuck at ${it.stage}`, variant: "error" };
    default:
      // Any other non-empty pause_reason still means parked — never render a
      // paused item as "running". Only a truly un-paused active item is running.
      if (it.pause_reason) return { label: `parked · ${it.pause_reason}`, variant: "warning" };
      return { label: `running · ${it.stage}`, variant: "running" };
  }
}

// A lifecycle event kind → a compact human verb for the timeline.
const KIND_LABEL: Record<string, string> = {
  create: "created",
  advance: "advanced",
  loop: "looped back",
  pause: "paused",
  failed: "failed",
  terminal: "completed",
  resume: "resumed",
  approve: "approved",
  reject: "rejected",
  reject_retry: "rejected (retry)",
  override: "overridden",
  rejected: "rejected",
  abandon: "stopped",
};

// pr_ref is opaque (a PR number or a URL). Render a link when it looks like one.
function prHref(pr: string): string | null {
  if (/^https?:\/\//.test(pr)) return pr;
  return null;
}

export default function WorkflowActions() {
  const [items, setItems] = useState<Item[]>([]);
  const [showAll, setShowAll] = useState(false);
  const [selId, setSelId] = useState<string | null>(null);
  const [detail, setDetail] = useState<Item | null>(null);
  const [events, setEvents] = useState<WfEvent[]>([]);
  const [proposalMd, setProposalMd] = useState<string>("");
  const [proposalTrunc, setProposalTrunc] = useState(false);
  const [loading, setLoading] = useState(false);
  const [status, setStatus] = useState<{ kind: "ok" | "err"; msg: string } | null>(null);
  const [gateMsg, setGateMsg] = useState("");
  const [composing, setComposing] = useState(false);
  const [draft, setDraft] = useState<Draft>(loadDraft);
  const [defs, setDefs] = useState<string[]>([]);
  const [submitMsg, setSubmitMsg] = useState("");
  const [submitting, setSubmitting] = useState(false);
  const afterRef = useRef(0); // events pagination cursor for the open proposal
  const reqRef = useRef(0); // monotonic open token: stale async loads (older selection) no-op

  const refreshList = useCallback(() => {
    getJSON<{ items: Item[] }>(showAll ? "/api/workflow/items/all" : "/api/workflow/items")
      .then((d) => setItems(d.items || []))
      .catch((e) =>
        setStatus({ kind: "err", msg: showAll ? `list all failed: ${e.message}` : "list failed" }),
      );
  }, [showAll]);

  useEffect(() => {
    refreshList();
    // Workflow choices for the composer's picker (falls back to "build").
    getJSON<{ defs: { name: string }[] }>("/api/workflow/defs")
      .then((d) => setDefs((d.defs || []).map((x) => x.name)))
      .catch(() => setDefs([]));
  }, [refreshList]);

  // Persist the in-progress draft locally (device-local; cleared on logout).
  useEffect(() => {
    try {
      localStorage.setItem(DRAFT_KEY, JSON.stringify(draft));
    } catch {
      /* quota/full — drafting still works in-memory */
    }
  }, [draft]);

  const startNew = useCallback(() => {
    setComposing(true);
    setSelId(null);
    setDetail(null);
    setSubmitMsg("");
  }, []);
  const updateDraft = useCallback(
    (patch: Partial<Draft>) => setDraft((d) => ({ ...d, ...patch })),
    [],
  );

  // Load a proposal's full detail: the item row, its source markdown, and the head
  // of its event timeline (resetting the pagination cursor).
  const openProposal = useCallback((id: string) => {
    const myReq = ++reqRef.current; // invalidates any still-in-flight prior load
    const live = () => myReq === reqRef.current;
    setComposing(false);
    setSelId(id);
    setLoading(true);
    setEvents([]);
    afterRef.current = 0;
    setGateMsg("");
    Promise.all([
      getJSON<Item>(`/api/workflow/items/${encodeURIComponent(id)}`)
        .then((d) => live() && setDetail(d))
        .catch(() => live() && setDetail(null)),
      getJSON<{ proposal_md: string; truncated: boolean }>(
        `/api/workflow/items/${encodeURIComponent(id)}/proposal`,
      )
        .then((d) => {
          if (!live()) return;
          setProposalMd(d.proposal_md || "");
          setProposalTrunc(!!d.truncated);
        })
        .catch(() => {
          if (!live()) return;
          setProposalMd("");
          setProposalTrunc(false);
        }),
      getJSON<{ events: WfEvent[]; next_after: number }>(
        `/api/workflow/items/${encodeURIComponent(id)}/events?limit=200`,
      )
        .then((d) => {
          if (!live()) return;
          setEvents(d.events || []);
          afterRef.current = d.next_after || 0;
        })
        .catch(() => {}),
    ]).finally(() => {
      if (live()) setLoading(false);
    });
  }, []);

  // Submit the composed proposal for autonomous execution. proposal_md is the H1
  // title + body; on success the draft is cleared and we open the new run's detail.
  const submitProposal = useCallback(async () => {
    const title = draft.title.trim();
    const body = draft.body.trim();
    if (!body) {
      setSubmitMsg("Proposal body is empty.");
      return;
    }
    const md = title ? `# ${title}\n\n${body}` : body;
    setSubmitting(true);
    setSubmitMsg("");
    try {
      const { status: st, data } = await postJSON<{ work_item_id?: string; error?: string }>(
        "/api/dev/submit",
        { proposal_md: md, workflow: draft.workflow || "build", repo: draft.repo || "" },
      );
      if (st >= 200 && st < 300 && data.work_item_id) {
        try {
          localStorage.removeItem(DRAFT_KEY);
        } catch {
          /* ignore */
        }
        setDraft(emptyDraft());
        setComposing(false);
        refreshList();
        openProposal(data.work_item_id); // flow straight into watching it run
      } else {
        setSubmitMsg(data.error || `submit failed (HTTP ${st})`);
      }
    } catch {
      setSubmitMsg("submit failed");
    } finally {
      setSubmitting(false);
    }
  }, [draft, refreshList, openProposal]);

  // Poll the open proposal while it's active: refresh the row and append only new
  // events (after=cursor, so no re-fetch/dup). Keyed on selId alone (not detail) so
  // it isn't torn down every tick; the interval self-stops once the item reaches a
  // terminal state, and cleans up on unmount / selection change.
  useEffect(() => {
    if (!selId) return;
    let cancelled = false;
    const iv = window.setInterval(async () => {
      try {
        const it = await getJSON<Item>(`/api/workflow/items/${encodeURIComponent(selId)}`);
        if (cancelled) return;
        setDetail(it);
        setItems((prev) => prev.map((p) => (p.id === it.id ? it : p)));
        const d = await getJSON<{ events: WfEvent[]; next_after: number }>(
          `/api/workflow/items/${encodeURIComponent(selId)}/events?after=${afterRef.current}&limit=200`,
        );
        if (cancelled) return;
        if (d.events && d.events.length) {
          // Idempotent append: keep only events past the last one we hold, so no
          // race (re-open, overlapping tick) can duplicate a row.
          setEvents((prev) => {
            const lastId = prev.length ? prev[prev.length - 1].id : 0;
            const fresh = d.events.filter((e) => e.id > lastId);
            return fresh.length ? [...prev, ...fresh] : prev;
          });
          afterRef.current = d.next_after || afterRef.current;
        }
        if (isTerminal(it.state)) window.clearInterval(iv);
      } catch {
        /* transient; next tick retries */
      }
    }, POLL_MS);
    return () => {
      cancelled = true;
      window.clearInterval(iv);
    };
  }, [selId]);

  const decideGate = useCallback(
    async (decision: "approve" | "reject") => {
      if (!selId) return;
      setGateMsg("");
      const { status: st, data } = await postJSON<{ error?: string }>(
        `/api/workflow/items/${encodeURIComponent(selId)}/gate`,
        { decision },
      );
      if (st >= 200 && st < 300) {
        setGateMsg(decision === "approve" ? "Approved — resuming." : "Rejected.");
        openProposal(selId); // refresh detail + events after the decision
      } else {
        setGateMsg(data.error || `failed (HTTP ${st})`);
      }
    },
    [selId, openProposal],
  );

  // Lifecycle control (pause / resume / stop / delete). On success we refresh the
  // open proposal (or clear the selection after a delete).
  const [actMsg, setActMsg] = useState("");
  const [acting, setActing] = useState(false);
  const doLifecycle = useCallback(
    async (action: "pause" | "resume" | "stop" | "delete") => {
      if (!selId || acting) return;
      if (action === "stop" && !window.confirm("Stop this run? It will be abandoned and cannot be resumed."))
        return;
      if (action === "delete" && !window.confirm("Delete this run permanently? Its history and proposal file will be removed."))
        return;
      setActMsg("");
      setActing(true);
      try {
        const isDel = action === "delete";
        const { status: st, data } = await sendAction<{ error?: string }>(
          isDel
            ? `/api/workflow/items/${encodeURIComponent(selId)}`
            : `/api/workflow/items/${encodeURIComponent(selId)}/${action}`,
          isDel ? "DELETE" : "POST",
        );
        if (st >= 200 && st < 300) {
          if (isDel) {
            setSelId(null);
            setDetail(null);
            refreshList();
          } else {
            openProposal(selId); // refresh detail + events after the transition
          }
        } else {
          setActMsg(data.error || `failed (HTTP ${st})`);
        }
      } catch {
        setActMsg("action failed");
      } finally {
        setActing(false);
      }
    },
    [selId, acting, refreshList, openProposal],
  );

  const canDecide = !!detail && detail.pause_reason === "pending_human";
  // Which lifecycle controls apply to the current item.
  const term = !!detail && isTerminal(detail.state);
  const paused = !!detail && !term && !!detail.pause_reason;
  const canPause = !!detail && !term && !detail.pause_reason;
  const canResume = paused && detail!.pause_reason !== "pending_human";
  const canStop = !!detail && !term;

  return (
    <div style={{ display: "flex", height: "100%", fontFamily: "system-ui" }}>
      {/* left rail: proposal list */}
      <div
        style={{
          width: 300,
          flexShrink: 0,
          borderRight: "1px solid #eee",
          overflowY: "auto",
          padding: 12,
        }}
      >
        <div style={{ display: "flex", alignItems: "center", gap: 8, marginBottom: 8 }}>
          <strong style={{ fontSize: 16 }}>Workflows</strong>
          <Badge label={`${items.length}`} variant="neutral" />
          <button onClick={refreshList} style={btn}>
            Refresh
          </button>
        </div>
        <button
          onClick={startNew}
          style={{
            ...btn,
            width: "100%",
            background: composing ? "#eef4ff" : "#2563eb",
            color: composing ? "#2563eb" : "#fff",
            borderColor: "#2563eb",
            marginBottom: 8,
          }}
        >
          + New proposal
        </button>
        <label style={{ fontSize: 12, color: "#666", display: "flex", alignItems: "center", gap: 6 }}>
          <input type="checkbox" checked={showAll} onChange={(e) => setShowAll(e.target.checked)} />
          Show all (operator)
        </label>
        {status && (
          <div style={{ fontSize: 12, color: status.kind === "err" ? "#c00" : "#070", marginTop: 6 }}>
            {status.msg}
          </div>
        )}
        <div style={{ marginTop: 8 }}>
          {items.length === 0 && (
            <div style={{ color: "#999", fontSize: 13 }}>No proposals yet.</div>
          )}
          {items.map((it) => {
            const s = statusOf(it);
            return (
              <div
                key={it.id}
                onClick={() => openProposal(it.id)}
                style={{
                  padding: "8px 8px",
                  borderRadius: 6,
                  cursor: "pointer",
                  marginBottom: 4,
                  background: selId === it.id ? "#eef4ff" : "transparent",
                  border: "1px solid",
                  borderColor: selId === it.id ? "#bcd4ff" : "#f0f0f0",
                }}
              >
                <div style={{ display: "flex", justifyContent: "space-between", gap: 6 }}>
                  <span style={{ fontWeight: 600, fontSize: 13, wordBreak: "break-all" }}>
                    {it.proposal_name || it.id}
                  </span>
                </div>
                <div style={{ display: "flex", alignItems: "center", gap: 6, marginTop: 4 }}>
                  <Badge label={s.label} variant={s.variant} />
                  <span style={{ fontSize: 11, color: "#999" }}>{it.workflow}</span>
                </div>
              </div>
            );
          })}
        </div>
      </div>

      {/* main: composer (new proposal) OR selected proposal detail */}
      <div style={{ flex: 1, minWidth: 0, overflowY: "auto", padding: 16 }}>
        {composing && (
          <Composer
            draft={draft}
            defs={defs}
            update={updateDraft}
            onSubmit={() => void submitProposal()}
            submitting={submitting}
            submitMsg={submitMsg}
          />
        )}
        {!composing && !detail && (
          <div style={{ color: "#888", fontSize: 14, marginTop: 20 }}>
            Select a proposal to see its status and history, or start a new one.
            <Spinner loading={loading} text="loading…" />
          </div>
        )}
        {!composing && detail && (
          <>
            <div style={{ display: "flex", alignItems: "center", gap: 10, marginBottom: 4 }}>
              <strong style={{ fontSize: 18 }}>{detail.proposal_name || detail.id}</strong>
              <Badge label={statusOf(detail).label} variant={statusOf(detail).variant} />
              <Spinner loading={loading} text="" />
            </div>
            <StatusHeader item={detail} />

            {/* Lifecycle controls: start (resume) / pause / stop / delete. Shown by
                the item's current state; a run at a human gate uses Approve/Reject
                (below) rather than resume. */}
            <div style={{ display: "flex", alignItems: "center", gap: 8, flexWrap: "wrap", margin: "4px 0 10px" }}>
              {canResume && (
                <button onClick={() => void doLifecycle("resume")} disabled={acting} style={btn}>
                  ▶ Start
                </button>
              )}
              {canPause && (
                <button onClick={() => void doLifecycle("pause")} disabled={acting} style={btn}>
                  ⏸ Pause
                </button>
              )}
              {canStop && (
                <button
                  onClick={() => void doLifecycle("stop")}
                  disabled={acting}
                  style={{ ...btn, background: "#fbeaea", color: "#a33", borderColor: "#e0a0a0" }}
                >
                  ⏹ Stop
                </button>
              )}
              <button
                onClick={() => void doLifecycle("delete")}
                disabled={acting}
                style={{ ...btn, background: "#fff5f5", color: "#c00", borderColor: "#e6b3b3", marginLeft: "auto" }}
              >
                🗑 Delete
              </button>
              {actMsg && <span style={{ fontSize: 12, color: "#c00", flexBasis: "100%" }}>{actMsg}</span>}
            </div>

            {canDecide && (
              <Panel title={`Human gate · ${detail.stage}`}>
                <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
                  <button onClick={() => void decideGate("approve")} style={btn}>
                    Approve
                  </button>
                  <button
                    onClick={() => void decideGate("reject")}
                    style={{ ...btn, background: "#fbeaea", color: "#a33", borderColor: "#e0a0a0" }}
                  >
                    Reject
                  </button>
                  {gateMsg && <span style={{ fontSize: 12, color: "#667" }}>{gateMsg}</span>}
                </div>
              </Panel>
            )}

            <Panel title={`History · ${events.length} events`}>
              <Timeline events={events} />
            </Panel>

            <Panel title="Proposal">
              {proposalTrunc && (
                <div style={{ fontSize: 12, color: "#a60", marginBottom: 6 }}>
                  ⚠ Proposal truncated (over the size cap); showing the first part.
                </div>
              )}
              {proposalMd ? (
                <div
                  style={{ fontSize: 13, lineHeight: 1.5, overflowWrap: "anywhere" }}
                  dangerouslySetInnerHTML={{ __html: renderMd(proposalMd) }}
                />
              ) : (
                <div style={{ color: "#999", fontSize: 13 }}>No proposal markdown.</div>
              )}
            </Panel>
          </>
        )}
      </div>
    </div>
  );
}

function Composer({
  draft,
  defs,
  update,
  onSubmit,
  submitting,
  submitMsg,
}: {
  draft: Draft;
  defs: string[];
  update: (patch: Partial<Draft>) => void;
  onSubmit: () => void;
  submitting: boolean;
  submitMsg: string;
}) {
  // Always include the draft's stored workflow as an option, even if it isn't in the
  // fetched defs (a stale/renamed def), so the select never renders blank.
  const base = defs.length ? defs : ["build"];
  const workflows = base.includes(draft.workflow) ? base : [draft.workflow, ...base];

  // "Draft with a delegate": one tool-free LLM completion (server-side
  // /api/proposal/draft) turns the title + notes into proposal markdown, shown as a
  // PREVIEW the user explicitly accepts — so it never silently clobbers their notes.
  const [drafting, setDrafting] = useState(false);
  const [preview, setPreview] = useState<string | null>(null);
  const [draftErr, setDraftErr] = useState("");
  const busy = drafting || submitting;

  // "Load a proposal from the project": a read-only, path-confined browser over
  // the server's local checkout. Picking a .md file previews its contents into the
  // same accept-first preview panel used by the delegate draft, so it never
  // silently clobbers the body.
  const [browsing, setBrowsing] = useState(false);
  const [browsePath, setBrowsePath] = useState("");
  const [entries, setEntries] = useState<{ name: string; type: "dir" | "file" }[]>([]);
  const [browseErr, setBrowseErr] = useState("");
  const [browseLoading, setBrowseLoading] = useState(false);

  const loadTree = async (path: string) => {
    setBrowseErr("");
    setBrowseLoading(true);
    try {
      const d = await getJSON<{ path: string; entries: { name: string; type: "dir" | "file" }[] }>(
        `/api/workflow/repo/tree?path=${encodeURIComponent(path)}`,
      );
      setBrowsePath(d.path || "");
      // Directories first, then files; each group alphabetical.
      const es = (d.entries || []).slice().sort((a, b) =>
        a.type !== b.type ? (a.type === "dir" ? -1 : 1) : a.name.localeCompare(b.name),
      );
      setEntries(es);
    } catch (e) {
      setBrowseErr(`could not list: ${(e as Error).message}`);
      setEntries([]);
    } finally {
      setBrowseLoading(false);
    }
  };
  const openBrowser = () => {
    setBrowsing(true);
    void loadTree(browsePath || "docs/proposals");
  };
  const parentPath = (p: string) => {
    const i = p.lastIndexOf("/");
    return i > 0 ? p.slice(0, i) : "";
  };
  const pickFile = async (name: string) => {
    const rel = browsePath ? `${browsePath}/${name}` : name;
    setBrowseErr("");
    setBrowseLoading(true);
    try {
      const d = await getJSON<{ content: string; truncated: boolean }>(
        `/api/workflow/repo/file?path=${encodeURIComponent(rel)}`,
      );
      setPreview(d.content || "");
      setBrowsing(false);
    } catch (e) {
      setBrowseErr(`could not read: ${(e as Error).message}`);
    } finally {
      setBrowseLoading(false);
    }
  };

  const generate = async () => {
    if (drafting) return; // re-entrancy guard (the button is also disabled while busy)
    setDraftErr("");
    setPreview(null);
    const title = draft.title.trim();
    const body = draft.body.trim();
    if (!title && !body) {
      setDraftErr("Add a title or some notes first.");
      return;
    }
    // Title + notes are the SUBJECT; the server system prompt frames them as data.
    const prompt = `Title: ${title || "(untitled)"}\n\nNotes / requirements:\n${body || "(none)"}`;
    setDrafting(true);
    try {
      const { status: st, data } = await postJSON<{ text?: string; error?: string }>(
        "/api/proposal/draft",
        { prompt },
      );
      if (st >= 200 && st < 300 && data.text) setPreview(data.text);
      else setDraftErr(data.error || `draft failed (HTTP ${st})`);
    } catch {
      setDraftErr("draft failed");
    } finally {
      setDrafting(false);
    }
  };

  return (
    <div style={{ maxWidth: 820 }}>
      <strong style={{ fontSize: 18 }}>New proposal</strong>
      <div style={{ fontSize: 12, color: "#888", margin: "4px 0 12px" }}>
        Describe the change; aimee runs the chosen workflow end-to-end and parks at human
        gates. Draft is saved locally on this device (cleared on logout).
      </div>
      <div style={{ display: "grid", gridTemplateColumns: "2fr 1fr", gap: 10 }}>
        <L label="Title">
          <input
            value={draft.title}
            onChange={(e) => update({ title: e.target.value })}
            placeholder="Short proposal title (becomes the H1)"
            style={inp}
            disabled={busy}
          />
        </L>
        <L label="Workflow">
          <select
            value={draft.workflow}
            onChange={(e) => update({ workflow: e.target.value })}
            style={inp}
            disabled={busy}
          >
            {workflows.map((w) => (
              <option key={w} value={w}>
                {w}
              </option>
            ))}
          </select>
        </L>
      </div>
      <L label="Repo (optional)">
        <input
          value={draft.repo}
          onChange={(e) => update({ repo: e.target.value })}
          placeholder="owner/name or clone URL (blank = default)"
          style={inp}
          disabled={busy}
        />
      </L>
      <L label="Proposal (Markdown)">
        <textarea
          value={draft.body}
          onChange={(e) => update({ body: e.target.value })}
          rows={18}
          style={{ ...inp, fontFamily: "monospace", fontSize: 13, lineHeight: 1.5 }}
          disabled={busy}
        />
      </L>

      {preview !== null && (
        <div
          style={{
            border: "1px solid #bcd4ff",
            background: "#f6f9ff",
            borderRadius: 6,
            padding: 10,
            marginTop: 10,
          }}
        >
          <div style={{ display: "flex", alignItems: "center", gap: 8, marginBottom: 6 }}>
            <strong style={{ fontSize: 13 }}>Delegate draft — preview</strong>
            <button
              onClick={() => {
                update({ body: preview });
                setPreview(null);
              }}
              style={{ ...btn, background: "#2563eb", color: "#fff", borderColor: "#2563eb" }}
            >
              Use this draft
            </button>
            <button onClick={() => setPreview(null)} style={btn}>
              Discard
            </button>
            <span style={{ fontSize: 11, color: "#888" }}>Replaces the body above.</span>
          </div>
          <div
            style={{ fontSize: 13, lineHeight: 1.5, maxHeight: 320, overflow: "auto" }}
            dangerouslySetInnerHTML={{ __html: renderMd(preview) }}
          />
        </div>
      )}

      <div style={{ display: "flex", alignItems: "center", gap: 10, marginTop: 10 }}>
        <button
          onClick={onSubmit}
          disabled={busy}
          style={{ ...btn, background: "#2563eb", color: "#fff", borderColor: "#2563eb" }}
        >
          {submitting ? "Submitting…" : `Submit → run "${draft.workflow || "build"}"`}
        </button>
        <button onClick={() => void generate()} disabled={busy} style={btn}>
          {drafting ? "Drafting…" : "✨ Draft with a delegate"}
        </button>
        <button onClick={() => (browsing ? setBrowsing(false) : openBrowser())} disabled={busy} style={btn}>
          {browsing ? "Close browser" : "📂 Load from project"}
        </button>
        {submitMsg && <span style={{ fontSize: 12, color: "#c00" }}>{submitMsg}</span>}
        {draftErr && <span style={{ fontSize: 12, color: "#c00" }}>{draftErr}</span>}
      </div>

      {browsing && (
        <div
          style={{
            border: "1px solid #d9e2ef",
            background: "#fbfdff",
            borderRadius: 6,
            padding: 10,
            marginTop: 10,
          }}
        >
          <div style={{ display: "flex", alignItems: "center", gap: 8, marginBottom: 6 }}>
            <strong style={{ fontSize: 13 }}>Project</strong>
            <span style={{ fontSize: 12, color: "#667", fontFamily: "monospace", overflowWrap: "anywhere" }}>
              /{browsePath}
            </span>
            {browseLoading && <span style={{ fontSize: 11, color: "#aaa" }}>loading…</span>}
            <span style={{ marginLeft: "auto", fontSize: 11, color: "#888" }}>
              Pick a .md file to load it as the proposal.
            </span>
          </div>
          {browseErr && <div style={{ fontSize: 12, color: "#c00", marginBottom: 6 }}>{browseErr}</div>}
          <div style={{ maxHeight: 280, overflow: "auto", border: "1px solid #eef2f7", borderRadius: 4 }}>
            {browsePath && (
              <BrowseRow icon="↩" label=".." onClick={() => void loadTree(parentPath(browsePath))} />
            )}
            {entries.length === 0 && !browseLoading && (
              <div style={{ padding: "8px 10px", color: "#999", fontSize: 13 }}>
                No sub-folders or .md files here.
              </div>
            )}
            {entries.map((e) =>
              e.type === "dir" ? (
                <BrowseRow
                  key={e.name}
                  icon="📁"
                  label={e.name}
                  onClick={() => void loadTree(browsePath ? `${browsePath}/${e.name}` : e.name)}
                />
              ) : (
                <BrowseRow key={e.name} icon="📄" label={e.name} onClick={() => void pickFile(e.name)} />
              ),
            )}
          </div>
        </div>
      )}
    </div>
  );
}

function BrowseRow({ icon, label, onClick }: { icon: string; label: string; onClick: () => void }) {
  return (
    <div
      onClick={onClick}
      style={{
        display: "flex",
        alignItems: "center",
        gap: 8,
        padding: "6px 10px",
        cursor: "pointer",
        borderBottom: "1px solid #f4f7fb",
        fontSize: 13,
      }}
      onMouseEnter={(ev) => (ev.currentTarget.style.background = "#eef4ff")}
      onMouseLeave={(ev) => (ev.currentTarget.style.background = "transparent")}
    >
      <span style={{ width: 16, textAlign: "center" }}>{icon}</span>
      <span style={{ overflowWrap: "anywhere" }}>{label}</span>
    </div>
  );
}

function L({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <label style={{ display: "block", marginTop: 8 }}>
      <span style={{ color: "#888", fontSize: 12, display: "block", marginBottom: 2 }}>
        {label}
      </span>
      {children}
    </label>
  );
}

function StatusHeader({ item }: { item: Item }) {
  const pr = item.pr_ref ? prHref(item.pr_ref) : null;
  return (
    <div
      style={{
        display: "grid",
        gridTemplateColumns: "repeat(auto-fill, minmax(180px, 1fr))",
        gap: "2px 16px",
        fontSize: 13,
        margin: "6px 0 10px",
      }}
    >
      <Field k="stage" v={item.stage} />
      <Field k="workflow" v={`${item.workflow} · v${(item.version || "").slice(0, 8)}`} />
      {item.repo ? <Field k="repo" v={item.repo} /> : null}
      {typeof item.cum_cost_usd === "number" && (
        <Field
          k="cost"
          v={
            item.work_item_max_cost_usd
              ? `$${item.cum_cost_usd.toFixed(4)} / $${item.work_item_max_cost_usd.toFixed(2)}`
              : `$${item.cum_cost_usd.toFixed(4)}`
          }
        />
      )}
      {item.pr_ref ? (
        <div style={{ display: "flex", gap: 8 }}>
          <span style={{ color: "#888" }}>PR</span>
          {pr ? (
            <a href={pr} target="_blank" rel="noreferrer" style={{ color: "#2563eb" }}>
              {item.pr_ref}
            </a>
          ) : (
            <span style={{ fontFamily: "monospace" }}>{item.pr_ref}</span>
          )}
        </div>
      ) : null}
      {item.override_count ? <Field k="overrides" v={String(item.override_count)} /> : null}
      {item.submitter ? <Field k="submitter" v={item.submitter} /> : null}
    </div>
  );
}

function Timeline({ events }: { events: WfEvent[] }) {
  if (!events.length) return <div style={{ color: "#999", fontSize: 13 }}>No events yet.</div>;
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 0 }}>
      {events.map((e) => (
        <div
          key={e.id}
          style={{
            display: "flex",
            gap: 10,
            padding: "6px 0",
            borderBottom: "1px solid #f3f3f3",
            fontSize: 13,
          }}
        >
          <span style={{ color: "#aaa", fontSize: 11, width: 130, flexShrink: 0 }}>
            {e.created_at}
          </span>
          <span style={{ flexShrink: 0, width: 120 }}>
            <Badge label={KIND_LABEL[e.kind] || e.kind} variant="neutral" />
          </span>
          <span style={{ flex: 1, minWidth: 0 }}>
            <span style={{ color: "#555" }}>{e.stage}</span>
            {e.detail ? <span style={{ color: "#888" }}> — {e.detail}</span> : null}
            <span style={{ color: "#bbb", fontSize: 11 }}>
              {" "}
              ({e.actor}
              {e.cost_usd ? `, $${e.cost_usd.toFixed(4)}` : ""})
            </span>
          </span>
        </div>
      ))}
    </div>
  );
}

function Field({ k, v }: { k: string; v: string }) {
  return (
    <div style={{ display: "flex", gap: 8 }}>
      <span style={{ color: "#888" }}>{k}</span>
      <span style={{ overflowWrap: "anywhere" }}>{v}</span>
    </div>
  );
}

const btn: React.CSSProperties = {
  fontSize: 13,
  padding: "4px 10px",
  border: "1px solid #ccc",
  borderRadius: 4,
  background: "#fff",
  cursor: "pointer",
};
const inp: React.CSSProperties = {
  fontSize: 13,
  padding: "5px 7px",
  border: "1px solid #ccc",
  borderRadius: 4,
  width: "100%",
  boxSizing: "border-box",
};
