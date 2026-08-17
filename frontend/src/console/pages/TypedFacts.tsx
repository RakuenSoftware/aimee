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
  generated_at: string;
}

export default function TypedFacts() {
  const [data, setData] = useState<TypedFacts | null>(null);
  const [loaded, setLoaded] = useState(false);
  const [busy, setBusy] = useState("");
  const [err, setErr] = useState("");
  const [notice, setNotice] = useState("");
  const [threshold, setThreshold] = useState(0);

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

  if (!loaded) return <div style={{ padding: 24, color: "var(--sg-text-faint)" }}>Loading typed facts…</div>;

  const cfg = data?.config;
  const cands = data?.promotion_candidates ?? [];
  const thresholdDirty = !!cfg && threshold !== cfg.promote_threshold;

  return (
    <div style={{ padding: "18px 24px", maxWidth: 860, margin: "0 auto", fontFamily: "system-ui" }}>
      <h2 style={{ margin: "0 0 4px" }}>Typed facts</h2>
      <p style={{ color: "var(--sg-text-faint)", fontSize: 13, margin: "0 0 18px" }}>
        The typed-fact layer turns observed relations into structured knowledge. New relations start
        provisional; once one has been observed enough times it can be promoted into the ontology —
        automatically, or by you from the queue below.
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
