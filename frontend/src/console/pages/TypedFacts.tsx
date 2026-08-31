import { useCallback, useEffect, useState } from "react";
import { apiGet, apiSend } from "../api";

/* Typed Facts page: the kb's typed-fact layer — its knobs, and the promotion
 * review queue for provisional relations.
 *
 * The typed-fact layer is a kb property end to end: the ontology lives in DB2,
 * the promotion sweep runs in the kb, and `kb_typed_facts_auto_promote_enabled`
 * and `kb_typed_facts_promote_threshold` are not even in aimee-server's
 * config_fields allowlist — POST /v1/console/typed_facts/config is their only
 * editor. That backend shipped without a UI, so nothing reached it; this page is
 * that UI.
 *
 * `typed_facts_enabled` is owned HERE rather than by the console's Settings page:
 * the master switch belongs with the queue it gates. */

interface Candidate {
  relation: string;
  observations: number;
  ready: boolean;
  status: string;
}

interface TypedFacts {
  config: { typed_facts_enabled: boolean; auto_promote: boolean; promote_threshold: number };
  promotion_candidates: Candidate[];
  candidate_count: number;
  assertion_candidates: AssertionCandidate[];
  assertion_candidate_count: number;
  entities: EntitySummary[];
  entity_count: number;
  entity_merges: EntityMerge[];
  entity_merge_count: number;
  generated_at: string;
}

interface EntitySummary {
  canonical_id: number;
  kind: number;
  status: string;
  merged_into: number;
  name: string;
}

interface EntityMerge {
  merge_id: number;
  from_id: number;
  into_id: number;
  undone: boolean;
  from_name: string;
  into_name: string;
  commit_id: string;
}

interface AssertionCandidate {
  id: number;
  subject: string;
  relation: string;
  object: string;
  assertion_kind: string;
  lifecycle: string;
  authority_rank: number;
  evidence_count: number;
  commit_id: string;
}

interface CommitChange {
  assertion_id: number;
  object_kind: string;
  object_key: string;
  action: string;
  before: string;
  after: string;
  detail: string;
}

interface CommitPreview {
  commit_id?: string;
  ingest_run_id?: string;
  changes: CommitChange[];
}

interface ErasureImpact {
  assertions: number;
  evidence_mentions: number;
  residual_data: string;
  commit_id?: string;
}

interface ReviewItem {
  item_id: string;
  item_head: string;
  source_queue: string;
  subject_kind: string;
  subject_id: string;
  epistemic_kind: string;
  priority: number;
  suggested_operation: string;
  offered_decisions: string[];
  evidence_status: string;
  dependents_status: string;
  blast_radius_status: string;
  current_value: unknown;
  proposed_change: unknown;
  authority_comparison: unknown;
  evidence: unknown;
  dependent_memories: unknown;
  recall_blast_radius: unknown;
  outcome_summary: unknown;
}

interface ReviewList {
  items: ReviewItem[];
  changeset_head: string;
}

export default function TypedFacts() {
  const [data, setData] = useState<TypedFacts | null>(null);
  const [loaded, setLoaded] = useState(false);
  const [busy, setBusy] = useState("");
  const [err, setErr] = useState("");
  const [notice, setNotice] = useState("");
  const [threshold, setThreshold] = useState(0);
  const [commitId, setCommitId] = useState("");
  const [ingestRunId, setIngestRunId] = useState("");
  const [commitPreview, setCommitPreview] = useState<CommitPreview | null>(null);
  const [eraseSubject, setEraseSubject] = useState("");
  const [eraseRelation, setEraseRelation] = useState("");
  const [eraseObject, setEraseObject] = useState("");
  const [erasurePreview, setErasurePreview] = useState<ErasureImpact | null>(null);
  const [previewedSelector, setPreviewedSelector] = useState("");
  const [mergeFrom, setMergeFrom] = useState("");
  const [mergeInto, setMergeInto] = useState("");
  const [review, setReview] = useState<ReviewList>({ items: [], changeset_head: "" });
  const [selectedReview, setSelectedReview] = useState("");

  const refresh = useCallback(() => {
    apiGet<TypedFacts>("/v1/console/typed_facts")
      .then((d) => {
        setData(d);
        setThreshold(d.config?.promote_threshold ?? 3);
        setErr("");
        setLoaded(true);
      })
      .catch((e) => {
        setErr(`Failed to load typed facts: ${e}`);
        setLoaded(true);
      });
    apiSend<ReviewList>("POST", "/v1/console/evidence", { action: "review.list", limit: 100 })
      .then((d) => {
        setReview(d);
        setSelectedReview((selected) =>
          selected && d.items.some((item) => item.item_id === selected)
            ? selected
            : (d.items[0]?.item_id ?? ""),
        );
      })
      .catch((e) => setErr((old) => old || `Failed to load review workbench: ${e}`));
  }, []);

  useEffect(() => {
    refresh();
  }, [refresh]);

  const patchConfig = async (patch: Record<string, boolean | number>, msg: string) => {
    setBusy(JSON.stringify(patch));
    setNotice("");
    try {
      await apiSend("POST", "/v1/console/typed_facts/config", patch);
      setNotice(msg);
      setErr("");
      refresh();
    } catch (e) {
      setErr(`Save failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  /* approve promotes a provisional relation, reject drops it, map folds it into
   * an existing relation (so the operator can collapse a near-duplicate rather
   * than growing the ontology). */
  const act = async (action: "approve" | "reject" | "map", relation: string) => {
    let target: string | undefined;
    if (action === "map") {
      const t = window.prompt(`Map "${relation}" onto which existing relation?`)?.trim();
      if (!t) return;
      target = t;
    } else if (!window.confirm(`${action === "approve" ? "Approve" : "Reject"} "${relation}"?`)) {
      return;
    }
    setBusy(relation);
    setNotice("");
    try {
      await apiSend("POST", "/v1/console/typed_facts/relation", { action, relation, target });
      setNotice(`${relation} ${action === "map" ? `mapped onto ${target}` : `${action}d`}`);
      setErr("");
      refresh();
    } catch (e) {
      setErr(`Action failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  const reviewAssertion = async (action: "approve" | "reject", id: number) => {
    setBusy(`assertion:${id}`);
    try {
      await apiSend("POST", "/v1/console/typed_facts/assertion", { action, assertion_id: id });
      setNotice(`Assertion ${id} ${action === "approve" ? "approved" : "declined"}`);
      refresh();
    } catch (e) {
      setErr(`Assertion review failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  const mergeEntities = async () => {
    if (!mergeFrom || !mergeInto || mergeFrom === mergeInto) return;
    setBusy("entity-merge");
    try {
      await apiSend("POST", "/v1/console/typed_facts/entity", {
        action: "merge", from_id: Number(mergeFrom), into_id: Number(mergeInto),
      });
      setMergeFrom("");
      setMergeInto("");
      setNotice("Entities merged in a reversible changeset");
      refresh();
    } catch (e) {
      setErr(`Entity merge failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  const unmergeEntity = async (mergeId: number) => {
    if (!window.confirm(`Undo entity merge ${mergeId}?`)) return;
    setBusy(`entity-unmerge:${mergeId}`);
    try {
      await apiSend("POST", "/v1/console/typed_facts/entity", { action: "unmerge", merge_id: mergeId });
      setNotice(`Entity merge ${mergeId} reversed`);
      refresh();
    } catch (e) {
      setErr(`Entity unmerge failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  const previewCommit = async () => {
    setBusy("commit-preview");
    try {
      const payload = ingestRunId.trim()
        ? { action: "preview", ingest_run_id: ingestRunId.trim() }
        : { action: "preview", commit_id: commitId.trim() };
      setCommitPreview(await apiSend<CommitPreview>("POST", "/v1/console/typed_facts/commit", payload));
    } catch (e) {
      setErr(`Changeset preview failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  const rollbackCommit = async () => {
    if (!commitPreview || !window.confirm("Apply this compensating revert?")) return;
    setBusy("commit-rollback");
    try {
      const payload = ingestRunId.trim()
        ? { action: "rollback", ingest_run_id: ingestRunId.trim() }
        : { action: "rollback", commit_id: commitId.trim() };
      await apiSend("POST", "/v1/console/typed_facts/commit", payload);
      setCommitPreview(null);
      setNotice("Compensating changeset applied");
      refresh();
    } catch (e) {
      setErr(`Changeset revert failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  const erasureSelector = () => [eraseSubject, eraseRelation, eraseObject].map((v) => v.trim()).join("\n");
  const runErasure = async (action: "preview" | "erase") => {
    if (action === "erase" && !window.confirm("Permanently erase the previewed evidence?")) return;
    setBusy(`erasure:${action}`);
    try {
      const result = await apiSend<ErasureImpact>("POST", "/v1/console/typed_facts/erasure", {
        action, subject: eraseSubject.trim(), relation: eraseRelation.trim(), object: eraseObject.trim(),
      });
      if (action === "preview") {
        setErasurePreview(result);
        setPreviewedSelector(erasureSelector());
      } else {
        setErasurePreview(null);
        setNotice("Evidence permanently erased; the content-free receipt remains");
        refresh();
      }
    } catch (e) {
      setErr(`Erasure ${action} failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  const decideReview = async (item: ReviewItem, decision: string) => {
    let requestedValue = "";
    let previewToken = "";
    if (["request_evidence", "correct", "annotate", "resolve"].includes(decision)) {
      requestedValue = window.prompt(
        decision === "request_evidence" ? "What evidence is required?" : `Value or rationale for ${decision}:`,
      )?.trim() ?? "";
      if (!requestedValue) return;
    }
    if (["invalidate_source", "purge"].includes(decision)) {
      previewToken = window.prompt("Paste the current document lifecycle preview token:")?.trim() ?? "";
      if (!previewToken) return;
    } else if (!window.confirm(`${decision} ${item.subject_kind}:${item.subject_id}?`)) {
      return;
    }
    setBusy(`review:${item.item_id}`);
    try {
      await apiSend("POST", "/v1/console/evidence", {
        action: "review.decide", item_id: item.item_id, item_head: item.item_head,
        decision, requested_value: requestedValue, preview_token: previewToken,
      });
      setNotice(`${decision} recorded as a reversible knowledge changeset`);
      refresh();
    } catch (e) {
      setErr(`Review decision refused: ${e}`);
      refresh();
    } finally {
      setBusy("");
    }
  };

  if (!loaded) return <div style={{ padding: 24, color: "var(--sg-text-faint)" }}>Loading typed facts…</div>;

  const cfg = data?.config;
  const cands = data?.promotion_candidates ?? [];
  const thresholdDirty = !!cfg && threshold !== cfg.promote_threshold;

  return (
    <div style={{ padding: "18px 24px", maxWidth: 860, margin: "0 auto", fontFamily: "system-ui" }}>
      <h2 style={{ margin: "0 0 4px" }}>Typed facts</h2>
      <p style={{ color: "var(--sg-text-faint)", fontSize: 13, margin: "0 0 18px" }}>
        The typed-fact layer turns observed relations into structured knowledge. New relations start
        provisional; recurrence raises review priority, while activation always remains an explicit
        operator governance decision.
      </p>
      {err && <p style={{ color: "var(--sg-danger-dark)", fontSize: 13 }}>{err}</p>}
      {notice && <p style={{ color: "var(--sg-success-dark)", fontSize: 13 }}>{notice}</p>}

      {cfg && (
        <div style={{ marginBottom: 22, border: "1px solid var(--sg-border)", borderRadius: 8, overflow: "hidden" }}>
          <div style={panelHead}>Configuration</div>
          <div style={{ padding: "10px 12px", display: "grid", gap: 12 }}>
            <label style={rowStyle}>
              <input
                type="checkbox"
                checked={cfg.typed_facts_enabled}
                disabled={!!busy}
                onChange={(e) =>
                  patchConfig(
                    { enabled: e.target.checked },
                    `typed-fact layer ${e.target.checked ? "enabled" : "disabled"}`,
                  )
                }
              />
              <span>
                Typed-fact layer enabled
                <div style={helpStyle}>
                  The master switch for the whole layer. Off by default; with it off nothing is
                  extracted and the queue below stays empty.
                </div>
              </span>
            </label>
            <div style={rowStyle}>
              <span style={{ width: 16 }} />
              <span>
                Review-priority threshold
                <div style={helpStyle}>Observations required before a relation is marked ready for operator review.</div>
                <div style={{ display: "flex", alignItems: "center", gap: 6, marginTop: 4 }}>
                  <input
                    type="number"
                    min={1}
                    value={threshold}
                    onChange={(e) => setThreshold(Math.max(1, Number(e.target.value) || 1))}
                    style={numStyle}
                  />
                  {thresholdDirty && (
                    <>
                      <button
                        disabled={!!busy}
                        onClick={() => patchConfig({ promote_threshold: threshold }, `threshold set to ${threshold}`)}
                        style={primaryBtn}
                      >
                        save
                      </button>
                      <button onClick={() => setThreshold(cfg.promote_threshold)} title="discard change" style={plainBtn}>
                        ↺
                      </button>
                    </>
                  )}
                </div>
              </span>
            </div>
          </div>
        </div>
      )}

      <div style={{ border: "1px solid var(--sg-border)", borderRadius: 8, overflow: "hidden", marginBottom: 18 }}>
        <div style={panelHead}>
          Knowledge review workbench{" "}
          <span style={{ fontWeight: 400, color: "var(--sg-text-hint)", fontSize: 12 }}>
            {review.items.length} pending decision{review.items.length === 1 ? "" : "s"}
          </span>
        </div>
        {review.items.length === 0 ? (
          <div style={{ padding: 12, fontSize: 12, color: "var(--sg-text-hint)" }}>Nothing waiting for review.</div>
        ) : (
          <div style={{ display: "grid", gridTemplateColumns: "minmax(220px, 0.8fr) minmax(320px, 1.2fr)" }}>
            <div style={{ borderRight: "1px solid var(--sg-border)" }}>
              {review.items.map((item) => (
                <button key={item.item_id} onClick={() => setSelectedReview(item.item_id)} style={{
                  width: "100%", textAlign: "left", padding: "9px 10px", border: 0,
                  borderBottom: "1px solid var(--sg-border-light)", cursor: "pointer",
                  background: selectedReview === item.item_id ? "var(--sg-surface-alt)" : "var(--sg-surface)",
                }}>
                  <div style={{ fontWeight: 650 }}>{item.subject_kind}:{item.subject_id}</div>
                  <div style={helpStyle}>{item.epistemic_kind} · {item.source_queue} · priority {item.priority}</div>
                  <div style={{ fontSize: 11, marginTop: 3 }}>
                    evidence: {item.evidence_status} · dependents: {item.dependents_status} · blast radius: {item.blast_radius_status}
                  </div>
                </button>
              ))}
            </div>
            {review.items.filter((item) => item.item_id === selectedReview).map((item) => (
              <div key={item.item_id} style={{ padding: 12, minWidth: 0 }}>
                <div style={{ fontWeight: 700 }}>{item.subject_kind}:{item.subject_id}</div>
                <div style={helpStyle}>Suggested: {item.suggested_operation}. Decisions are checked against this item’s immutable head.</div>
                <details open style={{ marginTop: 8, fontSize: 12 }}>
                  <summary>Current value and proposed change</summary>
                  <pre style={jsonStyle}>{JSON.stringify({ current: item.current_value, proposed: item.proposed_change, authority: item.authority_comparison }, null, 2)}</pre>
                </details>
                <details style={{ marginTop: 6, fontSize: 12 }}>
                  <summary>Evidence, dependents, blast radius, and outcomes</summary>
                  <pre style={jsonStyle}>{JSON.stringify({ evidence: item.evidence, dependents: item.dependent_memories, blast_radius: item.recall_blast_radius, outcomes: item.outcome_summary }, null, 2)}</pre>
                </details>
                <div style={{ display: "flex", gap: 6, flexWrap: "wrap", marginTop: 10 }}>
                  {item.offered_decisions.map((decision) => (
                    <button key={decision} disabled={!!busy} onClick={() => decideReview(item, decision)}
                      style={decision === item.suggested_operation ? primaryBtn : plainBtn}>{decision}</button>
                  ))}
                </div>
              </div>
            ))}
          </div>
        )}
      </div>

      <div style={{ border: "1px solid var(--sg-border)", borderRadius: 8, overflow: "hidden" }}>
        <div style={panelHead}>
          Promotion queue{" "}
          <span style={{ fontWeight: 400, color: "var(--sg-text-hint)", fontSize: 12 }}>
            {cands.length} provisional relation{cands.length === 1 ? "" : "s"}
          </span>
        </div>
        {cands.length === 0 ? (
          <div style={{ padding: "10px 12px", fontSize: 12, color: "var(--sg-text-hint)" }}>
            Nothing waiting — no provisional relations have been observed yet.
          </div>
        ) : (
          cands.map((c) => (
            <div
              key={c.relation}
              style={{ display: "flex", alignItems: "center", gap: 12, padding: "8px 12px", borderBottom: "1px solid var(--sg-border-light)", flexWrap: "wrap" }}
            >
              <div style={{ flex: "1 1 240px", minWidth: 180 }}>
                <div style={{ fontWeight: 600, fontSize: 14, fontFamily: "ui-monospace, monospace" }}>
                  {c.relation}
                </div>
                <div style={{ fontSize: 12, color: "var(--sg-text-faint)" }}>
                  {c.observations} observation{c.observations === 1 ? "" : "s"}
                  {c.status ? ` · ${c.status}` : ""}
                  {c.ready ? " · clears the threshold" : ""}
                </div>
              </div>
              {c.ready && (
                <span style={{ fontSize: 11, color: "var(--sg-success-dark)", background: "var(--sg-success-bg)", border: "1px solid var(--sg-success-bg)", borderRadius: 4, padding: "0 6px" }}>
                  ready
                </span>
              )}
              <div style={{ display: "flex", gap: 6 }}>
                <button disabled={busy === c.relation} onClick={() => act("approve", c.relation)} style={primaryBtn}>
                  approve
                </button>
                <button disabled={busy === c.relation} onClick={() => act("map", c.relation)} style={plainBtn}>
                  map…
                </button>
                <button
                  disabled={busy === c.relation}
                  onClick={() => act("reject", c.relation)}
                  style={{ ...plainBtn, color: "var(--sg-danger-dark)", borderColor: "var(--sg-danger-bg)" }}
                >
                  reject
                </button>
              </div>
            </div>
          ))
        )}
      </div>

      <div style={{ border: "1px solid #e2e2e2", borderRadius: 8, overflow: "hidden", marginTop: 18 }}>
        <div style={panelHead}>
          Assertion review{" "}
          <span style={{ fontWeight: 400, color: "#999", fontSize: 12 }}>
            {(data?.assertion_candidates ?? []).length} quarantined candidate
            {(data?.assertion_candidates ?? []).length === 1 ? "" : "s"}
          </span>
        </div>
        {(data?.assertion_candidates ?? []).length === 0 ? (
          <div style={{ padding: "10px 12px", fontSize: 12, color: "#999" }}>
            Nothing waiting. Candidate assertions stay out of default recall until approved here.
          </div>
        ) : (
          (data?.assertion_candidates ?? []).map((candidate) => (
            <div
              key={candidate.id}
              style={{ display: "flex", alignItems: "center", gap: 12, padding: "10px 12px", borderBottom: "1px solid #eee", flexWrap: "wrap" }}
            >
              <div style={{ flex: "1 1 360px", minWidth: 240 }}>
                <div style={{ fontSize: 14 }}>
                  <code>{candidate.subject}</code> · <code>{candidate.relation}</code> · <code>{candidate.object}</code>
                </div>
                <div style={{ fontSize: 12, color: "#777", marginTop: 3 }}>
                  {candidate.assertion_kind} · {candidate.evidence_count} evidence mention
                  {candidate.evidence_count === 1 ? "" : "s"} · authority {candidate.authority_rank}
                </div>
              </div>
              <div style={{ display: "flex", gap: 6 }}>
                <button
                  disabled={busy === `assertion:${candidate.id}`}
                  onClick={() => reviewAssertion("approve", candidate.id)}
                  style={primaryBtn}
                >
                  approve
                </button>
                <button
                  disabled={busy === `assertion:${candidate.id}`}
                  onClick={() => reviewAssertion("reject", candidate.id)}
                  style={{ ...plainBtn, color: "#b00", borderColor: "#e0b4b4" }}
                >
                  decline
                </button>
              </div>
            </div>
          ))
        )}
      </div>

      <div style={{ border: "1px solid #e2e2e2", borderRadius: 8, overflow: "hidden", marginTop: 18 }}>
        <div style={panelHead}>Canonical entity merge</div>
        <div style={{ padding: 12, display: "grid", gap: 8 }}>
          <div style={helpStyle}>
            Merge a duplicate entity into the canonical entity. The authenticated operator, diff,
            commit ID, and reversal are recorded atomically.
          </div>
          <label style={{ fontSize: 12 }}>
            Duplicate entity
            <select aria-label="Duplicate entity" value={mergeFrom}
              onChange={(e) => setMergeFrom(e.target.value)} style={{ ...textStyle, width: "100%", marginTop: 3 }}>
              <option value="">select duplicate…</option>
              {(data?.entities ?? []).filter((entity) => entity.status === "active").map((entity) => (
                <option key={entity.canonical_id} value={entity.canonical_id} disabled={String(entity.canonical_id) === mergeInto}>
                  {entity.name || `(unnamed ${entity.canonical_id})`} · #{entity.canonical_id}
                </option>
              ))}
            </select>
          </label>
          <label style={{ fontSize: 12 }}>
            Canonical entity
            <select aria-label="Canonical entity" value={mergeInto}
              onChange={(e) => setMergeInto(e.target.value)} style={{ ...textStyle, width: "100%", marginTop: 3 }}>
              <option value="">select canonical…</option>
              {(data?.entities ?? []).filter((entity) => entity.status === "active").map((entity) => (
                <option key={entity.canonical_id} value={entity.canonical_id} disabled={String(entity.canonical_id) === mergeFrom}>
                  {entity.name || `(unnamed ${entity.canonical_id})`} · #{entity.canonical_id}
                </option>
              ))}
            </select>
          </label>
          <button disabled={!!busy || !mergeFrom || !mergeInto || mergeFrom === mergeInto}
            onClick={mergeEntities} style={{ ...primaryBtn, justifySelf: "start" }}>merge entities</button>
          {(data?.entity_merges ?? []).length > 0 && (
            <div style={{ fontSize: 12, marginTop: 4 }}>
              {(data?.entity_merges ?? []).map((merge) => (
                <div key={merge.merge_id} style={{ padding: "6px 0", borderTop: "1px solid #eee", display: "flex", alignItems: "center", gap: 8 }}>
                  <span style={{ flex: 1 }}>
                    #{merge.merge_id}: <code>{merge.from_name || merge.from_id}</code> → <code>{merge.into_name || merge.into_id}</code>
                    {merge.undone ? " · undone" : ""}
                  </span>
                  {!merge.undone && (
                    <button disabled={!!busy} onClick={() => unmergeEntity(merge.merge_id)} style={plainBtn}>undo merge</button>
                  )}
                </div>
              ))}
            </div>
          )}
        </div>
      </div>

      <div style={{ border: "1px solid #e2e2e2", borderRadius: 8, overflow: "hidden", marginTop: 18 }}>
        <div style={panelHead}>Commit rollback</div>
        <div style={{ padding: 12, display: "grid", gap: 8 }}>
          <div style={helpStyle}>Preview one graph commit, or an entire ingest run, before atomically rolling it back.</div>
          <input aria-label="Commit ID" placeholder="commit ID" value={commitId}
            disabled={!!ingestRunId} onChange={(e) => { setCommitId(e.target.value); setCommitPreview(null); }} style={textStyle} />
          <input aria-label="Ingest run ID" placeholder="ingest run ID (for example memory-facts:42)" value={ingestRunId}
            disabled={!!commitId} onChange={(e) => { setIngestRunId(e.target.value); setCommitPreview(null); }} style={textStyle} />
          <div style={{ display: "flex", gap: 6 }}>
            <button disabled={!!busy || (!commitId.trim() && !ingestRunId.trim())} onClick={previewCommit} style={plainBtn}>preview diff</button>
            <button disabled={!!busy || !commitPreview} onClick={rollbackCommit} style={{ ...plainBtn, color: "#b00", borderColor: "#e0b4b4" }}>rollback previewed changes</button>
          </div>
          {commitPreview && (
            <div style={{ fontSize: 12 }}>
              {commitPreview.changes.length} change{commitPreview.changes.length === 1 ? "" : "s"}
              {commitPreview.changes.map((change, i) => (
                <div key={`${change.object_kind}:${change.object_key}:${i}`} style={{ padding: "5px 0", borderTop: "1px solid #eee" }}>
                  <code>{change.object_kind}:{change.object_key}</code> · {change.action} · {change.before || "∅"} → {change.after || "∅"}
                </div>
              ))}
            </div>
          )}
        </div>
      </div>

      <div style={{ border: "1px solid #e2e2e2", borderRadius: 8, overflow: "hidden", marginTop: 18 }}>
        <div style={panelHead}>Permanent erasure</div>
        <div style={{ padding: 12, display: "grid", gap: 8 }}>
          <div style={helpStyle}>Use reversible invalidation for corrections. This operator-only workflow permanently removes matching assertions and evidence.</div>
          <input aria-label="Erasure subject" placeholder="subject (required)" value={eraseSubject}
            onChange={(e) => { setEraseSubject(e.target.value); setErasurePreview(null); }} style={textStyle} />
          <input aria-label="Erasure relation" placeholder="relation (optional)" value={eraseRelation}
            onChange={(e) => { setEraseRelation(e.target.value); setErasurePreview(null); }} style={textStyle} />
          <input aria-label="Erasure object" placeholder="object (optional)" value={eraseObject}
            onChange={(e) => { setEraseObject(e.target.value); setErasurePreview(null); }} style={textStyle} />
          <div style={{ display: "flex", gap: 6 }}>
            <button disabled={!!busy || !eraseSubject.trim()} onClick={() => runErasure("preview")} style={plainBtn}>preview impact</button>
            <button disabled={!!busy || !erasurePreview || previewedSelector !== erasureSelector()}
              onClick={() => runErasure("erase")} style={{ ...plainBtn, color: "#b00", borderColor: "#e0b4b4" }}>erase permanently</button>
          </div>
          {erasurePreview && (
            <div style={{ fontSize: 12, color: "#555" }}>
              {erasurePreview.assertions} assertion{erasurePreview.assertions === 1 ? "" : "s"}, {erasurePreview.evidence_mentions} evidence mention{erasurePreview.evidence_mentions === 1 ? "" : "s"}. {erasurePreview.residual_data}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}

const panelHead: React.CSSProperties = {
  padding: "8px 12px",
  background: "var(--sg-surface-alt)",
  borderBottom: "1px solid var(--sg-border)",
  fontWeight: 700,
  color: "var(--sg-text-muted)",
};
const rowStyle: React.CSSProperties = { display: "flex", alignItems: "flex-start", gap: 8, fontSize: 13 };
const helpStyle: React.CSSProperties = { fontSize: 12, color: "var(--sg-text-faint)", lineHeight: 1.4 };
const numStyle: React.CSSProperties = {
  width: 80,
  fontFamily: "ui-monospace, monospace",
  fontSize: 12,
  padding: "3px 6px",
  borderRadius: 6,
  border: "1px solid var(--sg-border-medium)",
};
const textStyle: React.CSSProperties = {
  fontFamily: "ui-monospace, monospace",
  fontSize: 12,
  padding: "6px 8px",
  borderRadius: 6,
  border: "1px solid #ccc",
};
const jsonStyle: React.CSSProperties = {
  maxHeight: 240,
  overflow: "auto",
  whiteSpace: "pre-wrap",
  overflowWrap: "anywhere",
  padding: 8,
  borderRadius: 6,
  background: "var(--sg-surface-alt)",
  border: "1px solid var(--sg-border-light)",
};
const primaryBtn: React.CSSProperties = {
  padding: "4px 10px",
  borderRadius: 6,
  border: "1px solid var(--sg-info)",
  background: "var(--sg-info)",
  color: "var(--sg-surface)",
  cursor: "pointer",
  fontSize: 12,
};
const plainBtn: React.CSSProperties = {
  padding: "4px 10px",
  borderRadius: 6,
  border: "1px solid var(--sg-border-medium)",
  background: "var(--sg-surface)",
  color: "var(--sg-text-muted)",
  cursor: "pointer",
  fontSize: 12,
};
