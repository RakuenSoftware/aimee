import { useCallback, useEffect, useRef, useState } from "react";
import { Panel, Badge, Spinner, InlineStatus, EmptyState, Button } from "@rakuensoftware/smoothgui";
import type { BadgeVariant } from "@rakuensoftware/smoothgui";
import { renderMd } from "./chat/markdown";

/* ---- API types (mirror the enriched /api/workflow/items* envelopes) ---- */

interface Item {
  id: string;
  workflow: string;
  version: string;
  stage: string;
  state: string; // active | accepted | rejected | stopped | abandoned
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
// A configured trigger rule (aimee.yaml `trigger_rules`) that auto-starts runs.
export interface Trigger {
  source: string;
  event: string;
  schedule: string;
  mode: string;
  template: string;
  workspace: string;
  max_spend_usd?: number;
  origin?: "config" | "workflow";
  last_error?: string;
}

interface TriggerRegistryResponse {
  triggers: Trigger[];
  version: string;
  operator?: boolean;
  editable?: boolean;
  max_rules?: number;
  registry_error?: string;
}

interface WorkspaceOption {
  label: string;
  value: string;
}

const POLL_MS = 4000;
const DRAFT_KEY = "aimee_proposal_draft"; // cleared on logout (App.tsx)
const SCAFFOLD = `## Goal

## Motivation

## Approach

## Risks

## Tests
`;

export interface Draft {
  title: string;
  body: string;
  workflow: string;
  repo: string;
}

export function proposalDraftError(draft: Draft): string {
  if (!draft.body.trim()) return "Proposal body is empty.";
  if (!draft.repo.trim()) return "Choose a repository checkout before starting the workflow.";
  return "";
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
  let data: unknown = {};
  try { data = await r.json(); } catch { /* the status still carries the failure */ }
  if (!r.ok) {
    const message = data && typeof data === "object" && "error" in data
      ? String((data as { error?: unknown }).error || `HTTP ${r.status}`)
      : `HTTP ${r.status}`;
    throw new Error(message);
  }
  return data as T;
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

async function loadWorkflowConfig(): Promise<Record<string, unknown>> {
  return (await getJSON<{ config: Record<string, unknown> }>("/api/workflow/config")).config || {};
}

async function saveWorkflowConfig(key: string, value: unknown, previousVersion?: string): Promise<{ ok: boolean; status: number; value?: unknown; error?: string }> {
  const response = await postJSON<{ ok?: boolean; key?: string; value?: unknown; error?: string }>("/api/workflow/config/set", { key, value, ...(previousVersion ? { previous_version: previousVersion } : {}) });
  return response.status >= 200 && response.status < 300 && response.data.ok === true && response.data.key === key && !response.data.error
    ? { ok: true, status: response.status, value: response.data.value ?? value }
    : { ok: false, status: response.status, error: response.data.error || `save failed (${response.status})` };
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

export const isTerminal = (s: string) =>
  s === "accepted" || s === "rejected" || s === "stopped" || s === "abandoned";

// The Go WFE API uses human_gate/manual while the retained C workflow surface
// uses pending_human/operator_paused. The runtime UI fronts either engine, so
// control visibility must understand both wire-compatible spellings.
export const isHumanGatePause = (reason: string) => reason === "human_gate" || reason === "pending_human";

// A human status label + tone from the row's state + pause_reason + stage. Derived
// strictly from the documented enums — no invented "drafting" state.
function statusOf(it: Item): { label: string; variant: BadgeVariant } {
  if (it.state === "accepted") return { label: "merged (accepted)", variant: "success" };
  if (it.state === "rejected") return { label: "rejected", variant: "error" };
  if (it.state === "stopped") return { label: "stopped", variant: "neutral" };
  if (it.state === "abandoned") return { label: "abandoned", variant: "neutral" };
  // active:
  switch (it.pause_reason) {
    case "manual":
    case "operator_paused":
      return { label: `paused · ${it.stage}`, variant: "warning" };
    case "human_gate":
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
  const [triggers, setTriggers] = useState<Trigger[]>([]);
  const [triggerVersion, setTriggerVersion] = useState("");
  const [triggerEditable, setTriggerEditable] = useState(false);
  const [workflowOperator, setWorkflowOperator] = useState(false);
  const [triggerMaxRules, setTriggerMaxRules] = useState(32);
  const [triggerRegistryError, setTriggerRegistryError] = useState("");
  const [triggerLoadError, setTriggerLoadError] = useState("");
  const [triggerWorkspaces, setTriggerWorkspaces] = useState<WorkspaceOption[]>([]);
  const [triggersOpen, setTriggersOpen] = useState(true);
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
    getJSON<{ defs: { name: string; valid?: boolean }[] }>("/api/workflow/defs")
      .then((d) => setDefs((d.defs || []).filter((x) => x.valid !== false).map((x) => x.name)))
      .catch(() => setDefs([]));
    // Configured trigger rules — what auto-starts runs and whether this caller
    // may edit the global registry.
    getJSON<TriggerRegistryResponse>("/api/workflow/triggers")
      .then((d) => {
        setTriggers(d.triggers || []);
        setTriggerVersion(d.version || "");
        setTriggerEditable(d.editable === true);
        setWorkflowOperator(d.operator === true);
        setTriggerMaxRules(d.max_rules || 32);
        setTriggerRegistryError(d.registry_error || "");
        setTriggerLoadError("");
      })
      .catch((e: Error) => {
        setTriggers([]);
        setTriggerEditable(false);
        setWorkflowOperator(false);
        setTriggerLoadError(`Could not load triggers: ${e.message}`);
      });
    // Offer managed repositories by name. A custom server-visible path remains
    // available for repositories outside the managed workspace root.
    getJSON<{ root?: string; projects?: string[] }>("/api/git/projects")
      .then((d) => {
        const root = (d.root || "").replace(/\/+$/, "");
        const byPath = new Map<string, string>();
        for (const name of d.projects || []) {
          if (root && name) byPath.set(`${root}/${name}`, name);
        }
        setTriggerWorkspaces([...byPath].map(([value, label]) => ({ value, label })));
      })
      .catch(() => setTriggerWorkspaces([]));
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
    setDraft((current) => ({
      ...current,
      repo: current.repo || (triggerWorkspaces.length === 1 ? triggerWorkspaces[0].value : ""),
    }));
  }, [triggerWorkspaces]);
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
    const validation = proposalDraftError(draft);
    if (validation) {
      setSubmitMsg(validation);
      return;
    }
    const repo = draft.repo.trim();
    const md = title ? `# ${title}\n\n${body}` : body;
    setSubmitting(true);
    setSubmitMsg("");
    try {
      const { status: st, data } = await postJSON<{ work_item_id?: string; error?: string }>(
        "/api/dev/submit",
        { proposal_md: md, workflow: draft.workflow || "build", repo },
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

  const canDecide = workflowOperator && !!detail && isHumanGatePause(detail.pause_reason);
  // Which lifecycle controls apply to the current item.
  const term = !!detail && isTerminal(detail.state);
  const paused = !!detail && !term && !!detail.pause_reason;
  const canPause = !!detail && !term && !detail.pause_reason;
  const canResume = paused && !isHumanGatePause(detail!.pause_reason);
  const canStop = !!detail && !term;

  return (
    <div style={{ display: "flex", height: "100%", fontFamily: "system-ui" }}>
      {/* left rail: proposal list */}
      <div
        style={{
          width: 300,
          flexShrink: 0,
          borderRight: "1px solid var(--sg-border-light)",
          overflowY: "auto",
          padding: 12,
        }}
      >
        <div style={{ display: "flex", alignItems: "center", gap: 8, marginBottom: 8 }}>
          <strong style={{ fontSize: 16 }}>Workflows</strong>
          <Badge label={`${items.length}`} variant="neutral" />
          <Button onClick={refreshList} size="md" title="Reload the proposal list.">
            Refresh
          </Button>
        </div>
        <Button
          variant="primary"
          size="md"
          onClick={startNew}
          title="Start composing a new proposal to submit."
          style={{
            width: "100%",
            marginBottom: 8,
            ...(composing ? { background: "var(--sg-info-bg)", color: "var(--sg-info)" } : {}),
          }}
        >
          + New proposal
        </Button>
        {workflowOperator && (
          <label
            style={{ fontSize: 12, color: "var(--sg-text-secondary)", display: "flex", alignItems: "center", gap: 6 }}
            title="Show every run across all users, not just your own."
          >
            <input type="checkbox" checked={showAll} onChange={(e) => setShowAll(e.target.checked)} />
            Show all (operator)
          </label>
        )}
        {status && (
          <div style={{ marginTop: 6 }}>
            <InlineStatus status={status} />
          </div>
        )}
        <TriggersPanel
          triggers={triggers}
          onChange={setTriggers}
          version={triggerVersion}
          onVersion={setTriggerVersion}
          workflows={defs}
          workspaces={triggerWorkspaces}
          editable={triggerEditable}
          maxRules={triggerMaxRules}
          registryError={triggerRegistryError}
          loadError={triggerLoadError}
          open={triggersOpen}
          onToggle={() => setTriggersOpen((v) => !v)}
        />
        <RunPolicyPanel editable={workflowOperator} />
        <div style={{ marginTop: 8 }}>
          {items.length === 0 && (
            <EmptyState message="No proposals yet." inline />
          )}
          {items.map((it) => {
            const s = statusOf(it);
            return (
              <div
                key={it.id}
                onClick={() => openProposal(it.id)}
                title="Open this run to see its status and history."
                style={{
                  padding: "8px 8px",
                  borderRadius: 6,
                  cursor: "pointer",
                  marginBottom: 4,
                  background: selId === it.id ? "var(--sg-info-bg)" : "transparent",
                  border: "1px solid",
                  borderColor: selId === it.id ? "var(--sg-info-border)" : "var(--sg-surface-active)",
                }}
              >
                <div style={{ display: "flex", justifyContent: "space-between", gap: 6 }}>
                  <span style={{ fontWeight: 600, fontSize: 13, wordBreak: "break-all" }}>
                    {it.proposal_name || it.id}
                  </span>
                </div>
                <div style={{ display: "flex", alignItems: "center", gap: 6, marginTop: 4 }}>
                  <Badge label={s.label} variant={s.variant} />
                  <span style={{ fontSize: 11, color: "var(--sg-text-hint)" }}>{it.workflow}</span>
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
            workspaces={triggerWorkspaces}
            update={updateDraft}
            onSubmit={() => void submitProposal()}
            submitting={submitting}
            submitMsg={submitMsg}
          />
        )}
        {!composing && !detail && (
          <div style={{ color: "var(--sg-text-faint)", fontSize: 14, marginTop: 20 }}>
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
                <Button onClick={() => void doLifecycle("resume")} disabled={acting} size="md" title="Resume this paused run.">
                  ▶ Start
                </Button>
              )}
              {canPause && (
                <Button onClick={() => void doLifecycle("pause")} disabled={acting} size="md" title="Pause this active run.">
                  ⏸ Pause
                </Button>
              )}
              {canStop && (
                <Button
                  variant="danger"
                  size="md"
                  onClick={() => void doLifecycle("stop")}
                  disabled={acting}
                  title="Abandon this run; it cannot be resumed."
                >
                  ⏹ Stop
                </Button>
              )}
              <Button
                variant="danger"
                size="md"
                onClick={() => void doLifecycle("delete")}
                disabled={acting}
                style={{ marginLeft: "auto" }}
                title="Permanently delete this run, its history, and proposal file."
              >
                🗑 Delete
              </Button>
              {actMsg && <span style={{ fontSize: 12, color: "var(--sg-danger-dark)", flexBasis: "100%" }}>{actMsg}</span>}
            </div>

            {canDecide && (
              <Panel title={`Human gate · ${detail.stage}`}>
                <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
                  <Button onClick={() => void decideGate("approve")} size="md" title="Approve this gate and resume the run.">
                    Approve
                  </Button>
                  <Button
                    variant="danger"
                    size="md"
                    onClick={() => void decideGate("reject")}
                    title="Reject at this gate."
                  >
                    Reject
                  </Button>
                  {gateMsg && <span style={{ fontSize: 12, color: "var(--sg-text-secondary)" }}>{gateMsg}</span>}
                </div>
              </Panel>
            )}

            <Panel title={`History · ${events.length} events`}>
              <Timeline events={events} />
            </Panel>

            <Panel title="Proposal">
              {proposalTrunc && (
                <div style={{ fontSize: 12, color: "var(--sg-warning-dark)", marginBottom: 6 }}>
                  ⚠ Proposal truncated (over the size cap); showing the first part.
                </div>
              )}
              {proposalMd ? (
                <div
                  style={{ fontSize: 13, lineHeight: 1.5, overflowWrap: "anywhere" }}
                  dangerouslySetInnerHTML={{ __html: renderMd(proposalMd) }}
                />
              ) : (
                <EmptyState message="No proposal markdown." inline />
              )}
            </Panel>
          </>
        )}
      </div>
    </div>
  );
}

// Run policy: trigger admission plus the autonomy safety caps that govern how many
// workflows start and how far each one drives (trigger -> PR). Kept here under
// Workflows because that is exactly what they tune. Config-backed + live; exported
// Values saved here are the live runtime authority.
const RUN_POLICY_FIELDS: { key: string; label: string; help: string; kind: "int" | "bool"; min?: number; max?: number }[] = [
  {
    key: "trigger.max_concurrent",
    label: "Trigger admission cap",
    kind: "int",
    help: "Maximum active runs admitted across configured triggers. New proposals remain queued for a later scheduler pass when the cap is reached. Default 2. 0 pauses new admission.",
  },
  {
    key: "trigger.scan_interval_secs",
    label: "Proposal scan interval",
    kind: "int",
    help: "Seconds between live scans of configured pending-proposal directories. Default 5.",
  },
  {
    key: "autonomy.auto_resume_cap_parks",
    label: "Auto-resume wall-cap parks",
    kind: "bool",
    help: "When on (default), a run that hits its per-resume wall-clock window is resumed so it keeps going instead of being abandoned. Bounded by max auto-resumes.",
  },
  {
    key: "autonomy.max_wall_secs",
    label: "Max wall seconds / resume",
    kind: "int",
    help: "Per-resume wall-clock cap in seconds. On breach the run parks and (if auto-resume is on) is given a fresh window. Default 1800.",
  },
  {
    key: "autonomy.max_turns",
    label: "Max turns / run",
    kind: "int",
    help: "Cumulative per-run turn cap — the ultimate runaway backstop (auto-resume does not reset it). Default 300.",
  },
  {
    key: "autonomy.max_resumes",
    label: "Max auto-resumes / run",
    kind: "int",
    help: "How many times a run may auto-resume before the reaper is allowed to abandon it. Default 50. 0 = never auto-resume.",
  },
  {
    key: "autonomy.stale_abandon_secs",
    label: "Stale-abandon grace (s)",
    kind: "int",
    help: "Grace before a stuck/capped park is reaped -> abandoned. Default 3600. 0 disables the reaper.",
  },
  {
    key: "autonomy.concurrency",
    label: "Concurrency",
    kind: "int",
    help: "Max autonomous runs driven concurrently per scheduler sweep. Default 5.",
  },
  {
    key: "autonomy.delegate_pending_secs",
    label: "Unassigned delegate lease (s)",
    kind: "int",
    min: 2,
    max: 3600,
    help: "Seconds an admitted delegate job may remain pending without an assigned eligible agent before it is cancelled and safely retried. Live range 2–3600; default 120.",
  },
];

// Collapsible run-policy editor: loads the autonomy.* config on first open and writes
// each change back via the same /api/config/set the Settings page uses.
function RunPolicyPanel({ editable }: { editable: boolean }) {
  const [open, setOpen] = useState(false);
  const [cfg, setCfg] = useState<Record<string, unknown> | null>(null);
  const [saving, setSaving] = useState<string | null>(null);
  const [err, setErr] = useState<string | null>(null);
  useEffect(() => {
    if (!open || cfg !== null) return;
    let cancelled = false;
    setErr(null);
    loadWorkflowConfig()
      .then((value) => {
        if (!cancelled) setCfg(value);
      })
      .catch((error: Error) => {
        if (!cancelled) setErr(`Could not load run policy: ${error.message}`);
      });
    return () => {
      cancelled = true;
    };
  }, [open, cfg]);
  const save = async (key: string, value: unknown) => {
    setSaving(key);
    setErr(null);
    const r = await saveWorkflowConfig(key, value);
    setSaving(null);
    if (r.ok) setCfg((c) => ({ ...(c || {}), [key]: r.value }));
    else setErr(`${key}: ${r.error}`);
  };
  return (
    <div style={{ marginTop: 10, borderTop: "1px solid var(--sg-border-light)", paddingTop: 8 }}>
      <div
        onClick={() => setOpen((v) => !v)}
        style={{ display: "flex", alignItems: "center", gap: 6, cursor: "pointer", userSelect: "none" }}
      >
        <span style={{ fontSize: 11, color: "var(--sg-text-hint)", width: 10 }}>{open ? "▾" : "▸"}</span>
        <span style={{ fontWeight: 600, fontSize: 13 }}>⚙ Run policy</span>
        <span style={{ marginLeft: "auto", fontSize: 11, color: "var(--sg-text-pale)" }}>admission + autonomy caps</span>
      </div>
      {open && (
        <div style={{ marginTop: 6 }}>
          {cfg === null && !err ? (
            <div style={{ fontSize: 12, color: "var(--sg-text-hint)" }}>Loading…</div>
          ) : cfg === null ? (
            <div style={{ fontSize: 11, color: "var(--sg-danger-dark)" }}>{err}</div>
          ) : (
            <>
              <div style={{ fontSize: 11, color: "var(--sg-text-hint)", lineHeight: 1.4, marginBottom: 6 }}>
                {editable
                  ? "Admission, safety caps, and auto-resume for autonomous runs. Changes apply live."
                  : "Read-only. Administrator access is required to change global run policy."}
              </div>
              {RUN_POLICY_FIELDS.map((f) => (
                <div
                  key={f.key}
                  title={f.help}
                  style={{ display: "flex", alignItems: "center", gap: 8, padding: "3px 0" }}
                >
                  <span style={{ fontSize: 12, width: 190 }}>{f.label}</span>
                  {f.kind === "bool" ? (
                    <input
                      type="checkbox"
                      checked={!!cfg?.[f.key]}
                      disabled={!editable || saving !== null}
                      onChange={(e) => save(f.key, e.target.checked)}
                    />
                  ) : (
                    <input
                      type="number"
                      min={f.min}
                      max={f.max}
                      defaultValue={Number(cfg?.[f.key] ?? 0)}
                      disabled={!editable || saving !== null}
                      style={{
                        width: 90,
                        fontFamily: "ui-monospace, monospace",
                        fontSize: 12,
                        padding: "2px 4px",
                      }}
                      onBlur={(e) => {
                        const v = parseInt(e.target.value, 10);
                        const inRange =
                          !Number.isNaN(v) &&
                          (f.min === undefined || v >= f.min) &&
                          (f.max === undefined || v <= f.max);
                        if (!inRange) {
                          setErr(`${f.label} must be between ${f.min ?? 0} and ${f.max ?? "the supported maximum"}.`);
                          e.currentTarget.value = String(cfg?.[f.key] ?? 0);
                          return;
                        }
                        if (v !== Number(cfg?.[f.key])) save(f.key, v);
                      }}
                    />
                  )}
                  {saving === f.key && <span style={{ fontSize: 11, color: "var(--sg-text-hint)" }}>saving…</span>}
                </div>
              ))}
              {err && <div style={{ fontSize: 11, color: "var(--sg-danger-dark)", marginTop: 4 }}>{err}</div>}
            </>
          )}
        </div>
      )}
    </div>
  );
}

export function triggerValidationError(trigger: Trigger): string {
  const workspace = trigger.workspace.trim();
  if (!workspace) return "Choose a repository or enter its server-visible checkout path.";
  if (!workspace.startsWith("/") || /[\u0000-\u001f\u007f]/.test(workspace)) {
    return "Enter an absolute server-visible checkout path (for example /srv/repos/project).";
  }
  if (!trigger.template.trim()) return "Choose a saved workflow.";
  const directory = trigger.event.trim();
  if (!directory) return "Enter the repository-relative directory to watch.";
  if (directory.includes("\\") || /[\u0000-\u001f\u007f]/.test(directory)) {
    return "Use a repository path with forward slashes and no control characters.";
  }
  const parts = directory.split("/");
  if (directory.startsWith("/") || parts.includes("..") || directory === ".") {
    return "The watched directory must stay inside the repository (for example docs/proposals/pending).";
  }
  if (trigger.schedule.trim().startsWith("-")) return "A branch or ref cannot start with '-'.";
  if (trigger.mode !== "autonomous" && trigger.mode !== "interactive") return "Choose a supported run mode.";
  if (trigger.max_spend_usd !== undefined && (!Number.isFinite(trigger.max_spend_usd) || trigger.max_spend_usd < 0)) {
    return "The spend cap must be zero or a positive dollar amount.";
  }
  return "";
}

export function triggerRulesPayload(triggers: Trigger[]) {
  return triggers.filter((t) => t.origin !== "workflow").map((t) => ({
    source: "watch-dir",
    event: t.event.trim() || "docs/proposals/pending",
    schedule: t.schedule.trim(),
    mode: t.mode || "autonomous",
    pipeline: {
      template: t.template.trim(),
      workspace: t.workspace.trim(),
      ...(t.max_spend_usd && t.max_spend_usd > 0 ? { max_spend_usd: t.max_spend_usd } : {}),
    },
  }));
}

function emptyTrigger(workflows: string[], workspaces: WorkspaceOption[]): Trigger {
  return {
    source: "watch-dir",
    event: "docs/proposals/pending",
    schedule: "",
    mode: "autonomous",
    template: workflows[0] || "build",
    workspace: workspaces.length === 1 ? workspaces[0].value : "",
    origin: "config",
  };
}

// The configured trigger rules that auto-start runs, rendered as a compact,
// collapsible section above the run list. Config-origin rules are edited through
// a guarded form; graph-native triggers remain read-only and point back to the
// workflow editor that owns them.
export function TriggersPanel({
  triggers,
  onChange,
  version,
  onVersion,
  workflows,
  workspaces,
  editable,
  maxRules,
  registryError,
  loadError,
  open,
  onToggle,
}: {
  triggers: Trigger[];
  onChange: (triggers: Trigger[]) => void;
  version: string;
  onVersion: (version: string) => void;
  workflows: string[];
  workspaces: WorkspaceOption[];
  editable: boolean;
  maxRules: number;
  registryError: string;
  loadError: string;
  open: boolean;
  onToggle: () => void;
}) {
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const [editing, setEditing] = useState<number | "new" | null>(null);
  const [draft, setDraft] = useState<Trigger>(() => emptyTrigger(workflows, workspaces));

  const reload = async () => {
    const current = await getJSON<TriggerRegistryResponse>("/api/workflow/triggers");
    onChange(current.triggers || []);
    onVersion(current.version || "");
  };
  const persist = async (next: Trigger[]): Promise<boolean> => {
    setSaving(true);
    setError("");
    setNotice("");
    try {
      const result = await saveWorkflowConfig("trigger_rules", triggerRulesPayload(next), version);
      if (!result.ok) {
        if (result.status === 409) {
          try { await reload(); } catch { /* retain the conflict error */ }
          setError("Triggers changed in another session. The current registry was reloaded; review your edit and save again.");
        } else {
          setError(result.error || "Could not save the trigger registry.");
        }
        return false;
      }
      await reload();
      return true;
    } catch (e) {
      setError(`Could not save triggers: ${e instanceof Error ? e.message : "service unavailable"}`);
      return false;
    } finally {
      setSaving(false);
    }
  };
  const startAdd = () => {
    setDraft(emptyTrigger(workflows, workspaces));
    setEditing("new");
    setError("");
    setNotice("");
  };
  const startEdit = (index: number) => {
    setDraft({ ...triggers[index], source: "watch-dir", origin: "config" });
    setEditing(index);
    setError("");
    setNotice("");
  };
  const saveDraft = async () => {
    const validation = triggerValidationError(draft);
    if (validation) {
      setError(validation);
      return;
    }
    const next = editing === "new"
      ? [...triggers, draft]
      : triggers.map((trigger, index) => index === editing ? draft : trigger);
    if (await persist(next)) {
      setEditing(null);
      setNotice(editing === "new" ? "Trigger created. It will scan on the next scheduler pass." : "Trigger updated.");
    }
  };
  const remove = async (index: number) => {
    if (!window.confirm("Remove this automatic trigger? Existing workflow runs are not affected.")) return;
    if (await persist(triggers.filter((_, i) => i !== index))) {
      if (editing === index) setEditing(null);
      setNotice("Trigger removed. Existing workflow runs were left intact.");
    }
  };
  const configCount = triggers.filter((trigger) => trigger.origin !== "workflow").length;
  const unsupportedConfigCount = triggers.filter((trigger) =>
    trigger.origin !== "workflow" && trigger.source !== "watch-dir" && trigger.source !== "proposals",
  ).length;
  const canEdit = editable && !registryError && !loadError && unsupportedConfigCount === 0;
  const atLimit = configCount >= maxRules;

  return (
    <div style={{ marginTop: 10, borderTop: "1px solid var(--sg-border-light)", paddingTop: 8 }}>
      <div
        onClick={onToggle}
        style={{ display: "flex", alignItems: "center", gap: 6, cursor: "pointer", userSelect: "none" }}
      >
        <span style={{ fontSize: 11, color: "var(--sg-text-hint)", width: 10 }}>{open ? "▾" : "▸"}</span>
        <span style={{ fontWeight: 600, fontSize: 13 }}>⚡ Triggers</span>
        <Badge label={`${triggers.length}`} variant="neutral" />
        <span style={{ marginLeft: "auto", fontSize: 11, color: "var(--sg-text-pale)" }}>automatic starts</span>
      </div>
      {open && (
        <div style={{ marginTop: 6 }}>
          <div style={{ fontSize: 11, color: "var(--sg-text-faint)", lineHeight: 1.45, marginBottom: 8 }}>
            Watch a Git repository folder for committed Markdown files and start a saved workflow for each new proposal.
          </div>
          {loadError && <div role="alert" style={triggerErrorStyle}>{loadError}</div>}
          {registryError && (
            <div role="alert" style={triggerErrorStyle}>
              The saved trigger registry is invalid and cannot be safely edited here: {registryError}
            </div>
          )}
          {unsupportedConfigCount > 0 && (
            <div role="alert" style={triggerErrorStyle}>
              {unsupportedConfigCount} config trigger{unsupportedConfigCount === 1 ? " uses" : "s use"} a source this workflow scanner cannot edit. Manage the registry in <code>aimee.yaml</code>; browser writes are disabled to preserve those rules exactly.
            </div>
          )}
          {!editable && !loadError && (
            <div style={{ fontSize: 11, color: "var(--sg-text-faint)", marginBottom: 8 }}>
              Only the appliance administrator can change automatic triggers. You can still inspect what is active.
            </div>
          )}
          {triggers.length === 0 ? (
            <div style={{ fontSize: 12, color: "var(--sg-text-hint)", lineHeight: 1.4 }}>
              No triggers configured — runs start from a manual submit (+ New proposal).
            </div>
          ) : (
            triggers.map((trigger, index) => (
              <div key={`${trigger.origin || "config"}-${trigger.source}-${trigger.workspace}-${trigger.template}-${index}`} style={{ marginBottom: 8 }}>
                <TriggerCard t={trigger} />
                {trigger.origin === "workflow" ? (
                  <div style={{ fontSize: 11, color: "var(--sg-text-faint)", marginTop: 4 }}>
                    Owned by this workflow’s <code>trigger.watch-dir</code> start node. Edit that node in Edit Workflows to change or disarm it.
                  </div>
                ) : canEdit && editing !== index ? (
                  <div style={{ display: "flex", gap: 6, marginTop: 4 }}>
                    <Button size="sm" onClick={() => startEdit(index)} disabled={saving || editing !== null}>Edit</Button>
                    <Button size="sm" onClick={() => remove(index)} disabled={saving || editing !== null}>Remove</Button>
                  </div>
                ) : null}
                {editing === index && (
                  <TriggerEditor
                    draft={draft}
                    onChange={(patch) => setDraft((current) => ({ ...current, ...patch }))}
                    workflows={workflows}
                    workspaces={workspaces}
                    saving={saving}
                    onSave={saveDraft}
                    onCancel={() => { setEditing(null); setError(""); }}
                  />
                )}
              </div>
            ))
          )}
          {editing === "new" && (
            <TriggerEditor
              draft={draft}
              onChange={(patch) => setDraft((current) => ({ ...current, ...patch }))}
              workflows={workflows}
              workspaces={workspaces}
              saving={saving}
              onSave={saveDraft}
              onCancel={() => { setEditing(null); setError(""); }}
            />
          )}
          {canEdit && editing === null && (
            <Button size="sm" onClick={startAdd} disabled={saving || atLimit} title={atLimit ? `At the ${maxRules}-trigger safety limit.` : "Create an automatic workflow trigger."}>
              + New trigger
            </Button>
          )}
          {atLimit && canEdit && <div style={{ fontSize: 11, color: "var(--sg-text-faint)", marginTop: 4 }}>The {maxRules}-trigger safety limit has been reached.</div>}
          {notice && <div role="status" style={{ color: "var(--sg-success-dark)", fontSize: 11, marginTop: 5 }}>{notice}</div>}
          {error && <div role="alert" style={triggerErrorStyle}>{error}</div>}
        </div>
      )}
    </div>
  );
}

function TriggerEditor({
  draft,
  onChange,
  workflows,
  workspaces,
  saving,
  onSave,
  onCancel,
}: {
  draft: Trigger;
  onChange: (patch: Partial<Trigger>) => void;
  workflows: string[];
  workspaces: WorkspaceOption[];
  saving: boolean;
  onSave: () => void;
  onCancel: () => void;
}) {
  const managed = workspaces.some((workspace) => workspace.value === draft.workspace);
  const availableWorkflows = workflows.includes(draft.template) || !draft.template
    ? workflows
    : [draft.template, ...workflows];
  return (
    <div style={{ border: "1px solid var(--sg-info-border)", background: "var(--sg-surface-alt)", borderRadius: 6, padding: 9, marginTop: 6 }}>
      <div style={{ fontSize: 12, fontWeight: 600, marginBottom: 7 }}>Watch for committed Markdown proposals</div>
      <div style={{ display: "grid", gap: 8 }}>
        {workspaces.length > 0 && (
          <TriggerField label="Managed repository" help="Choose a repository Aimee already manages, or use a custom checkout path.">
            <select
              aria-label="Managed repository"
              style={inp}
              value={managed ? draft.workspace : "__custom__"}
              onChange={(e) => onChange({ workspace: e.target.value === "__custom__" ? "" : e.target.value })}
            >
              <option value="__custom__">Custom server path…</option>
              {workspaces.map((workspace) => <option key={workspace.value} value={workspace.value}>{workspace.label}</option>)}
            </select>
          </TriggerField>
        )}
        {(!managed || workspaces.length === 0) && (
          <TriggerField label="Repository checkout" help="Absolute checkout path visible to the Aimee server, not a path on your browser device.">
            <input aria-label="Repository checkout" value={draft.workspace} placeholder="/srv/repos/my-project" style={inp} onChange={(e) => onChange({ workspace: e.target.value })} />
          </TriggerField>
        )}
        <TriggerField label="Workflow" help="The saved workflow started once for each new Markdown file.">
          <select aria-label="Workflow" value={draft.template} style={inp} onChange={(e) => onChange({ template: e.target.value })}>
            {!draft.template && <option value="">Choose a workflow…</option>}
            {availableWorkflows.map((workflow) => <option key={workflow} value={workflow}>{workflow}</option>)}
          </select>
        </TriggerField>
        <TriggerField label="Directory to watch" help="Repository-relative directory containing proposal Markdown files.">
          <input aria-label="Directory to watch" value={draft.event} placeholder="docs/proposals/pending" style={inp} onChange={(e) => onChange({ event: e.target.value })} />
        </TriggerField>
        <TriggerField label="Branch or Git ref (optional)" help="Blank follows the refreshed origin default branch. Branch names are resolved on the remote.">
          <input aria-label="Branch or Git ref (optional)" value={draft.schedule} placeholder="default branch" style={inp} onChange={(e) => onChange({ schedule: e.target.value })} />
        </TriggerField>
        <div style={{ display: "grid", gridTemplateColumns: "minmax(0, 1fr) minmax(0, 1fr)", gap: 8 }}>
          <TriggerField label="Run mode" help="Interactive records the mode; use a human gate in the workflow when approval must be mandatory.">
            <select aria-label="Run mode" value={draft.mode} style={inp} onChange={(e) => onChange({ mode: e.target.value })}>
              <option value="autonomous">Autonomous</option>
              <option value="interactive">Interactive metadata</option>
            </select>
          </TriggerField>
          <TriggerField label="Spend cap (optional)" help="Maximum provider spend for each admitted run; blank means the workflow default.">
            <input
              aria-label="Spend cap (optional)"
              type="number"
              min="0"
              step="0.01"
              value={draft.max_spend_usd ?? ""}
              placeholder="No extra cap"
              style={inp}
              onChange={(e) => onChange({ max_spend_usd: e.target.value === "" ? undefined : Number(e.target.value) })}
            />
          </TriggerField>
        </div>
      </div>
      <div style={{ fontSize: 11, color: "var(--sg-text-faint)", lineHeight: 1.4, marginTop: 8 }}>
        The scanner reads committed files from Git. Uncommitted files do not fire, and unchanged proposal content is deduplicated.
      </div>
      <div style={{ display: "flex", gap: 6, marginTop: 8 }}>
        <Button variant="primary" size="sm" onClick={onSave} disabled={saving}>{saving ? "Saving…" : "Save trigger"}</Button>
        <Button size="sm" onClick={onCancel} disabled={saving}>Cancel</Button>
      </div>
    </div>
  );
}

function TriggerField({ label, help, children }: { label: string; help: string; children: React.ReactNode }) {
  return (
    <label title={help} style={{ display: "grid", gap: 3, fontSize: 11, color: "var(--sg-text-muted)" }}>
      <span style={{ fontWeight: 600 }}>{label}</span>
      {children}
      <span style={{ color: "var(--sg-text-faint)", lineHeight: 1.25 }}>{help}</span>
    </label>
  );
}

const triggerErrorStyle: React.CSSProperties = { color: "var(--sg-danger-dark)", background: "var(--sg-danger-bg)", border: "1px solid var(--sg-danger-bg)", borderRadius: 4, padding: "5px 7px", fontSize: 11, marginTop: 5, marginBottom: 5 };

// One trigger rule as a compact card: what fires it (source + event/schedule),
// which workflow it starts, how it runs (mode), and where (workspace).
function TriggerCard({ t }: { t: Trigger }) {
  const isCron = t.source === "cron";
  const isWatch = t.source === "watch-dir" || t.source === "proposals";
  // The "fires when" phrase, per source: a watched dir, a cron schedule, else the raw event.
  const fires = isCron
    ? `cron ${t.schedule || "(unset)"}`
    : isWatch
      ? `watches ${t.event || "docs/proposals/pending"}`
      : t.event || t.source;
  const interactive = t.mode === "interactive";
  return (
    <div
      style={{
        border: "1px solid var(--sg-surface-active)",
        borderRadius: 6,
        padding: "6px 8px",
        marginBottom: 4,
        background: "var(--sg-surface-alt)",
      }}
    >
      <div style={{ display: "flex", alignItems: "center", gap: 6, flexWrap: "wrap" }}>
        <Badge label={t.source} variant="running" />
        <span style={{ fontSize: 12, color: "var(--sg-text-muted)", overflowWrap: "anywhere" }}>{fires}</span>
      </div>
      <div style={{ display: "flex", alignItems: "center", gap: 6, marginTop: 4, flexWrap: "wrap" }}>
        <span style={{ fontSize: 11, color: "var(--sg-text-hint)" }}>→</span>
        <span style={{ fontSize: 12, fontWeight: 600 }}>{t.template || "(no workflow)"}</span>
        <Badge
          label={interactive ? "interactive" : "autonomous"}
          variant={interactive ? "warning" : "success"}
        />
      </div>
      {t.last_error && <div style={{ marginTop: 4, color: "var(--sg-danger-dark)", fontSize: 11 }}>{t.last_error}</div>}
      {t.workspace && (
        <div style={{ fontSize: 11, color: "var(--sg-text-pale)", fontFamily: "monospace", marginTop: 3, overflowWrap: "anywhere" }}>
          {t.workspace}
          {typeof t.max_spend_usd === "number" && t.max_spend_usd > 0
            ? ` · cap $${t.max_spend_usd.toFixed(2)}`
            : ""}
        </div>
      )}
    </div>
  );
}

function Composer({
  draft,
  defs,
  workspaces,
  update,
  onSubmit,
  submitting,
  submitMsg,
}: {
  draft: Draft;
  defs: string[];
  workspaces: WorkspaceOption[];
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
      <div style={{ fontSize: 12, color: "var(--sg-text-faint)", margin: "4px 0 12px" }}>
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
            title="Workflow to run this proposal through end-to-end."
          >
            {workflows.map((w) => (
              <option key={w} value={w}>
                {w}
              </option>
            ))}
          </select>
        </L>
      </div>
      {workspaces.length > 0 && (
        <L label="Managed repository">
          <select
            aria-label="Proposal repository"
            value={workspaces.some((workspace) => workspace.value === draft.repo) ? draft.repo : "__custom__"}
            onChange={(e) => update({ repo: e.target.value === "__custom__" ? "" : e.target.value })}
            style={inp}
            disabled={busy}
            title="Repository checkout the workflow is allowed to modify."
          >
            <option value="__custom__">Custom server path…</option>
            {workspaces.map((workspace) => <option key={workspace.value} value={workspace.value}>{workspace.label}</option>)}
          </select>
        </L>
      )}
      {(!workspaces.some((workspace) => workspace.value === draft.repo) || workspaces.length === 0) && (
        <L label="Repository checkout (required)">
          <input
            aria-label="Repository checkout (required)"
            value={draft.repo}
            onChange={(e) => update({ repo: e.target.value })}
            placeholder="/srv/repos/my-project"
            style={inp}
            disabled={busy}
            title="Absolute checkout path visible to the Aimee server."
          />
        </L>
      )}
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
            border: "1px solid var(--sg-info-border)",
            background: "var(--sg-surface-alt)",
            borderRadius: 6,
            padding: 10,
            marginTop: 10,
          }}
        >
          <div style={{ display: "flex", alignItems: "center", gap: 8, marginBottom: 6 }}>
            <strong style={{ fontSize: 13 }}>Delegate draft — preview</strong>
            <Button
              variant="primary"
              size="md"
              onClick={() => {
                update({ body: preview });
                setPreview(null);
              }}
              title="Replace the proposal body with this generated draft."
            >
              Use this draft
            </Button>
            <Button onClick={() => setPreview(null)} size="md" title="Discard this draft preview and keep your current body.">
              Discard
            </Button>
            <span style={{ fontSize: 11, color: "var(--sg-text-faint)" }}>Replaces the body above.</span>
          </div>
          <div
            style={{ fontSize: 13, lineHeight: 1.5, maxHeight: 320, overflow: "auto" }}
            dangerouslySetInnerHTML={{ __html: renderMd(preview) }}
          />
        </div>
      )}

      <div style={{ display: "flex", alignItems: "center", gap: 10, marginTop: 10 }}>
        <Button
          variant="primary"
          size="md"
          onClick={onSubmit}
          disabled={busy}
          title="Submit the proposal and start the selected workflow."
        >
          {submitting ? "Submitting…" : `Submit → run "${draft.workflow || "build"}"`}
        </Button>
        <Button
          size="md"
          onClick={() => void generate()}
          disabled={busy}
          title="Generate a proposal draft from your title and notes (shown as a preview to accept)."
        >
          {drafting ? "Drafting…" : "✨ Draft with a delegate"}
        </Button>
        <Button
          size="md"
          onClick={() => (browsing ? setBrowsing(false) : openBrowser())}
          disabled={busy}
          title="Browse the project checkout and load a .md file as the proposal."
        >
          {browsing ? "Close browser" : "📂 Load from project"}
        </Button>
        {submitMsg && <span style={{ fontSize: 12, color: "var(--sg-danger-dark)" }}>{submitMsg}</span>}
        {draftErr && <span style={{ fontSize: 12, color: "var(--sg-danger-dark)" }}>{draftErr}</span>}
      </div>

      {browsing && (
        <div
          style={{
            border: "1px solid var(--sg-info-border)",
            background: "var(--sg-surface-alt)",
            borderRadius: 6,
            padding: 10,
            marginTop: 10,
          }}
        >
          <div style={{ display: "flex", alignItems: "center", gap: 8, marginBottom: 6 }}>
            <strong style={{ fontSize: 13 }}>Project</strong>
            <span style={{ fontSize: 12, color: "var(--sg-text-secondary)", fontFamily: "monospace", overflowWrap: "anywhere" }}>
              /{browsePath}
            </span>
            {browseLoading && <span style={{ fontSize: 11, color: "var(--sg-text-pale)" }}>loading…</span>}
            <span style={{ marginLeft: "auto", fontSize: 11, color: "var(--sg-text-faint)" }}>
              Pick a .md file to load it as the proposal.
            </span>
          </div>
          {browseErr && <div style={{ fontSize: 12, color: "var(--sg-danger-dark)", marginBottom: 6 }}>{browseErr}</div>}
          <div style={{ maxHeight: 280, overflow: "auto", border: "1px solid var(--sg-surface-sunken)", borderRadius: 4 }}>
            {browsePath && (
              <BrowseRow icon="↩" label=".." onClick={() => void loadTree(parentPath(browsePath))} />
            )}
            {entries.length === 0 && !browseLoading && (
              <div style={{ padding: "8px 10px", color: "var(--sg-text-hint)", fontSize: 13 }}>
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
        borderBottom: "1px solid var(--sg-surface-alt)",
        fontSize: 13,
      }}
      onMouseEnter={(ev) => (ev.currentTarget.style.background = "var(--sg-info-bg)")}
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
      <span style={{ color: "var(--sg-text-faint)", fontSize: 12, display: "block", marginBottom: 2 }}>
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
          <span style={{ color: "var(--sg-text-faint)" }}>PR</span>
          {pr ? (
            <a href={pr} target="_blank" rel="noreferrer" style={{ color: "var(--sg-info)" }}>
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
  if (!events.length) return <EmptyState message="No events yet." inline />;
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 0 }}>
      {events.map((e) => (
        <div
          key={e.id}
          style={{
            display: "flex",
            gap: 10,
            padding: "6px 0",
            borderBottom: "1px solid var(--sg-surface-alt)",
            fontSize: 13,
          }}
        >
          <span style={{ color: "var(--sg-text-pale)", fontSize: 11, width: 130, flexShrink: 0 }}>
            {e.created_at}
          </span>
          <span style={{ flexShrink: 0, width: 120 }}>
            <Badge label={KIND_LABEL[e.kind] || e.kind} variant="neutral" />
          </span>
          <span style={{ flex: 1, minWidth: 0 }}>
            <span style={{ color: "var(--sg-text-muted)" }}>{e.stage}</span>
            {e.detail ? <span style={{ color: "var(--sg-text-faint)" }}> — {e.detail}</span> : null}
            <span style={{ color: "var(--sg-text-pale)", fontSize: 11 }}>
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
      <span style={{ color: "var(--sg-text-faint)" }}>{k}</span>
      <span style={{ overflowWrap: "anywhere" }}>{v}</span>
    </div>
  );
}

const inp: React.CSSProperties = {
  fontSize: 13,
  padding: "5px 7px",
  border: "1px solid var(--sg-border-medium)",
  borderRadius: 4,
  width: "100%",
  boxSizing: "border-box",
};
