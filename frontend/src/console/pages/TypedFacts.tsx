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

export default function TypedFacts() {
  const [data, setData] = useState<TypedFacts | null>(null);
  const [loaded, setLoaded] = useState(false);
  const [busy, setBusy] = useState("");
  const [err, setErr] = useState("");
  const [notice, setNotice] = useState("");
  const [threshold, setThreshold] = useState(0);
  const [lastReview, setLastReview] = useState<{ id: number; action: string } | null>(null);
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

  const reviewAssertion = async (action: "approve" | "reject" | "undo", assertionId: number) => {
    setBusy(`assertion:${assertionId}`);
    setNotice("");
    try {
      await apiSend("POST", "/v1/console/typed_facts/assertion", {
        action,
        assertion_id: assertionId,
      });
      setLastReview(action === "undo" ? null : { id: assertionId, action });
      setNotice(`Assertion ${assertionId} ${action === "reject" ? "declined" : `${action}d`}.`);
      setErr("");
      refresh();
    } catch (e) {
      setErr(`Review failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  const commitSelector = () =>
    ingestRunId.trim() ? { ingest_run_id: ingestRunId.trim() } : { commit_id: commitId.trim() };

  const previewCommit = async () => {
    if (!commitId.trim() && !ingestRunId.trim()) return;
    setBusy("commit:preview");
    try {
      const result = await apiSend<CommitPreview>("POST", "/v1/console/typed_facts/commit", {
        action: "preview",
        ...commitSelector(),
      });
      setCommitPreview(result);
      setErr("");
    } catch (e) {
      setErr(`Commit preview failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  const rollbackCommit = async () => {
    if (!commitPreview || !window.confirm(`Roll back all ${commitPreview.changes.length} previewed changes?`)) return;
    setBusy("commit:rollback");
    try {
      const result = await apiSend<{ rollback_commit_id: string }>("POST", "/v1/console/typed_facts/commit", {
        action: "rollback",
        ...commitSelector(),
      });
      setNotice(`Rollback committed as ${result.rollback_commit_id}.`);
      setCommitPreview(null);
      setErr("");
      refresh();
    } catch (e) {
      setErr(`Rollback failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  const erasureSelector = () =>
    JSON.stringify({ subject: eraseSubject.trim(), relation: eraseRelation.trim(), object: eraseObject.trim() });

  const runErasure = async (action: "preview" | "erase") => {
    if (!eraseSubject.trim()) return;
    if (action === "erase" &&
        (previewedSelector !== erasureSelector() ||
         !window.confirm("Permanently erase the previewed assertions and evidence? This cannot be undone."))) return;
    setBusy(`erasure:${action}`);
    try {
      const result = await apiSend<ErasureImpact>("POST", "/v1/console/typed_facts/erasure", {
        action,
        subject: eraseSubject.trim(),
        relation: eraseRelation.trim(),
        object: eraseObject.trim(),
      });
      setErasurePreview(result);
      setPreviewedSelector(action === "preview" ? erasureSelector() : "");
      setNotice(action === "erase" ? `Permanent erasure completed as ${result.commit_id}.` : "Erasure impact previewed.");
      setErr("");
      if (action === "erase") refresh();
    } catch (e) {
      setErr(`Erasure ${action} failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  const mergeEntities = async () => {
    if (!mergeFrom || !mergeInto || mergeFrom === mergeInto ||
        !window.confirm("Merge the duplicate entity into the canonical entity? This changes name resolution.")) return;
    setBusy("entity:merge");
    try {
      const result = await apiSend<{ merge_id: number; commit_id: string }>(
        "POST", "/v1/console/typed_facts/entity",
        { action: "merge", from_id: Number(mergeFrom), into_id: Number(mergeInto) },
      );
      setNotice(`Entity merge ${result.merge_id} committed as ${result.commit_id}.`);
      setMergeFrom("");
      setMergeInto("");
      setErr("");
      refresh();
    } catch (e) {
      setErr(`Entity merge failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  const unmergeEntity = async (mergeId: number) => {
    if (!window.confirm(`Undo entity merge ${mergeId}?`)) return;
    setBusy(`entity:unmerge:${mergeId}`);
    try {
      const result = await apiSend<{ commit_id: string }>(
        "POST", "/v1/console/typed_facts/entity", { action: "unmerge", merge_id: mergeId },
      );
      setNotice(`Entity merge ${mergeId} undone as ${result.commit_id}.`);
      setErr("");
      refresh();
    } catch (e) {
      setErr(`Entity unmerge failed: ${e}`);
    } finally {
      setBusy("");
    }
  };

  if (!loaded) return <div style={{ padding: 24, color: "#888" }}>Loading typed facts…</div>;

  const cfg = data?.config;
  const cands = data?.promotion_candidates ?? [];
  const thresholdDirty = !!cfg && threshold !== cfg.promote_threshold;

  return (
    <div style={{ padding: "18px 24px", maxWidth: 860, margin: "0 auto", fontFamily: "system-ui" }}>
      <h2 style={{ margin: "0 0 4px" }}>Typed facts</h2>
      <p style={{ color: "#777", fontSize: 13, margin: "0 0 18px" }}>
        The typed-fact layer turns observed relations into structured knowledge. New relations start
        provisional; once one has been observed enough times it can be promoted into the ontology —
        automatically, or by you from the queue below.
      </p>
      {err && <p style={{ color: "#b00", fontSize: 13 }}>{err}</p>}
      {notice && <p style={{ color: "#1f7a3d", fontSize: 13 }}>{notice}</p>}
      {lastReview && (
        <p style={{ fontSize: 12, marginTop: -8 }}>
          <button
            disabled={!!busy}
            onClick={() => reviewAssertion("undo", lastReview.id)}
            style={plainBtn}
          >
            undo {lastReview.action}
          </button>
        </p>
      )}

      {cfg && (
        <div style={{ marginBottom: 22, border: "1px solid #e2e2e2", borderRadius: 8, overflow: "hidden" }}>
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
            <label style={rowStyle}>
              <input
                type="checkbox"
                checked={cfg.auto_promote}
                disabled={!!busy}
                onChange={(e) =>
                  patchConfig(
                    { auto_promote: e.target.checked },
                    `auto-promote ${e.target.checked ? "enabled" : "disabled"}`,
                  )
                }
              />
              <span>
                Auto-promote recurrent relations
                <div style={helpStyle}>
                  Promote a provisional relation once it clears the threshold, without waiting for
                  review. On by default. With it off, everything waits in the queue below.
                </div>
              </span>
            </label>
            <div style={rowStyle}>
              <span style={{ width: 16 }} />
              <span>
                Promotion threshold
                <div style={helpStyle}>Observations required before a relation is eligible.</div>
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

      <div style={{ border: "1px solid #e2e2e2", borderRadius: 8, overflow: "hidden" }}>
        <div style={panelHead}>
          Promotion queue{" "}
          <span style={{ fontWeight: 400, color: "#999", fontSize: 12 }}>
            {cands.length} provisional relation{cands.length === 1 ? "" : "s"}
          </span>
        </div>
        {cands.length === 0 ? (
          <div style={{ padding: "10px 12px", fontSize: 12, color: "#999" }}>
            Nothing waiting — no provisional relations have been observed yet.
          </div>
        ) : (
          cands.map((c) => (
            <div
              key={c.relation}
              style={{ display: "flex", alignItems: "center", gap: 12, padding: "8px 12px", borderBottom: "1px solid #eee", flexWrap: "wrap" }}
            >
              <div style={{ flex: "1 1 240px", minWidth: 180 }}>
                <div style={{ fontWeight: 600, fontSize: 14, fontFamily: "ui-monospace, monospace" }}>
                  {c.relation}
                </div>
                <div style={{ fontSize: 12, color: "#777" }}>
                  {c.observations} observation{c.observations === 1 ? "" : "s"}
                  {c.status ? ` · ${c.status}` : ""}
                  {c.ready ? " · clears the threshold" : ""}
                </div>
              </div>
              {c.ready && (
                <span style={{ fontSize: 11, color: "#1f7a3d", background: "#eaf6ee", border: "1px solid #bfe0cb", borderRadius: 4, padding: "0 6px" }}>
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
                  style={{ ...plainBtn, color: "#b00", borderColor: "#e0b4b4" }}
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
  background: "#fafafa",
  borderBottom: "1px solid #e2e2e2",
  fontWeight: 700,
  color: "#555",
};
const rowStyle: React.CSSProperties = { display: "flex", alignItems: "flex-start", gap: 8, fontSize: 13 };
const helpStyle: React.CSSProperties = { fontSize: 12, color: "#777", lineHeight: 1.4 };
const numStyle: React.CSSProperties = {
  width: 80,
  fontFamily: "ui-monospace, monospace",
  fontSize: 12,
  padding: "3px 6px",
  borderRadius: 6,
  border: "1px solid #ccc",
};
const textStyle: React.CSSProperties = {
  fontFamily: "ui-monospace, monospace",
  fontSize: 12,
  padding: "6px 8px",
  borderRadius: 6,
  border: "1px solid #ccc",
};
const primaryBtn: React.CSSProperties = {
  padding: "4px 10px",
  borderRadius: 6,
  border: "1px solid #2563eb",
  background: "#2563eb",
  color: "#fff",
  cursor: "pointer",
  fontSize: 12,
};
const plainBtn: React.CSSProperties = {
  padding: "4px 10px",
  borderRadius: 6,
  border: "1px solid #ccc",
  background: "#fff",
  color: "#555",
  cursor: "pointer",
  fontSize: 12,
};
