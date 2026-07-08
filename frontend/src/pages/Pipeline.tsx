import { useCallback, useEffect, useState } from "react";

/* Pipeline page: the curator pipeline as an ordered, resource-lane-grouped view of
 * the stage registry (mirrors CURATOR_STAGES in src/kb/kb_curator_drain.c). Each
 * stage's enable flag comes from GET /api/config and toggles via POST
 * /api/config/set (persisted to aimee.yaml; the KB picks it up on next load).
 *
 * NB: STAGES below mirrors the C registry (name/lane/order). Keep in sync when
 * stages are added/reordered there — the enable state is live from config, but the
 * lane/order/labels are presentational metadata. */

type Val = boolean | number | string;
type Lane = "LLM" | "INDEX";
type Stage = { key: string; label: string; lane: Lane; desc: string; gated?: "embedder" };

// Registry order. LLM = GPU-bound (one/pass); INDEX = CPU-bound (drains each pass).
const STAGES: Stage[] = [
  { key: "kb_curator_extract_docs_enabled", label: "Extract docs", lane: "LLM", desc: "LLM-extract a document chunk into claims / entities / doc_summary." },
  { key: "kb_curator_extract_code_enabled", label: "Extract code", lane: "LLM", desc: "Extract a code symbol into a code_unit artifact." },
  { key: "kb_curator_resolve_entities_enabled", label: "Resolve entities", lane: "LLM", desc: "Resolve entity mentions against the canonical entity graph." },
  { key: "kb_curator_synthesize_enabled", label: "Synthesize", lane: "LLM", desc: "Synthesize a topic summary from committed artifacts." },
  { key: "kb_curator_promote_entity_enabled", label: "Promote entity", lane: "LLM", desc: "Promote a recurrent entity to the canonical registry." },
  { key: "kb_curator_index_narrative_enabled", label: "Index narrative", lane: "INDEX", desc: "Embed doc_summary / narrative text." },
  { key: "kb_curator_index_claims_enabled", label: "Index claims", lane: "INDEX", desc: "Embed each claim's subject/attribute/value into the claim-vector store." },
  { key: "kb_curator_detect_contradictions_enabled", label: "Detect contradictions", lane: "INDEX", desc: "Self-join claim vectors: same subject+attribute, different value → contradicts link." },
  { key: "kb_curator_index_code_unit_enabled", label: "Index code_unit", lane: "INDEX", desc: "Embed code_unit artifacts." },
  { key: "kb_curator_link_artifacts_enabled", label: "Link artifacts", lane: "INDEX", desc: "Link related artifacts (implements / relates-to)." },
  { key: "kb_curator_projection_graph_enabled", label: "Projection graph", lane: "INDEX", desc: "Publish a fresh typed-edge generation per changed project." },
  { key: "kb_curator_cross_repo_graph_enabled", label: "Cross-repo graph", lane: "INDEX", desc: "Rebuild cross-repo identities / routes / distinctiveness model." },
  { key: "kb_evidence_embed_enabled", label: "Embed evidence", lane: "INDEX", desc: "Fill evidence_vectors for the neighbourhood builder." },
];
// Embedder-gated stages (no config toggle — active whenever an embedder is configured).
const GATED: Stage[] = [
  { key: "embed_code", label: "Embed code", lane: "INDEX", desc: "Embed changed files into the code-vector layer.", gated: "embedder" },
  { key: "ingest_docs", label: "Ingest docs", lane: "INDEX", desc: "Chunk + embed prose/doc files into kb_documents.", gated: "embedder" },
];

async function getJSON<T>(url: string): Promise<T> {
  const r = await fetch(url, { headers: { "X-CSRF-Token": window._csrf || "" } });
  return (await r.json()) as T;
}
async function postJSON(url: string, body: unknown): Promise<{ status: number; error?: string }> {
  const r = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json", "X-CSRF-Token": window._csrf || "" },
    body: JSON.stringify(body),
  });
  let d: { error?: string } = {};
  try { d = await r.json(); } catch { /* empty */ }
  return { status: r.status, error: d.error };
}

const LANE_META: Record<Lane, { title: string; hint: string; color: string }> = {
  LLM: { title: "LLM lane · GPU", hint: "One unit per pass — extraction/reasoning on the GPU model.", color: "#8cf" },
  INDEX: { title: "Index lane · CPU", hint: "Drains its queue each pass — embedding + SQL, concurrent with the GPU lane.", color: "#7d7" },
};

export default function Pipeline() {
  const [cfg, setCfg] = useState<Record<string, Val>>({});
  const [loaded, setLoaded] = useState(false);
  const [busy, setBusy] = useState<string>("");
  const [status, setStatus] = useState<{ kind: "ok" | "err"; msg: string } | null>(null);

  const refresh = useCallback(() => {
    getJSON<{ config?: Record<string, Val> }>("/api/config")
      .then((d) => { setCfg(d.config || {}); setLoaded(true); })
      .catch(() => setLoaded(true));
  }, []);
  useEffect(() => { refresh(); }, [refresh]);

  const toggle = useCallback(async (key: string, next: boolean) => {
    setBusy(key);
    const { status: st, error } = await postJSON("/api/config/set", { key, value: next });
    setBusy("");
    if (st >= 200 && st < 300 && !error) {
      setCfg((p) => ({ ...p, [key]: next }));
      setStatus({ kind: "ok", msg: `${key} ${next ? "enabled" : "disabled"}` });
    } else {
      setStatus({ kind: "err", msg: error || `save failed (${st})` });
    }
  }, []);

  const enabled = (k: string) => cfg[k] === true || cfg[k] === 1;

  const row = (s: Stage, i: number) => {
    const on = s.gated ? true : enabled(s.key);
    return (
      <div key={s.key} style={{
        display: "flex", alignItems: "center", gap: 12, padding: "8px 12px",
        borderBottom: "1px solid #eee", opacity: s.gated ? 0.75 : 1,
      }}>
        <span style={{ width: 22, color: "#aaa", fontSize: 12, fontFamily: "ui-monospace, monospace" }}>{i + 1}</span>
        <div style={{ flex: 1, minWidth: 0 }}>
          <div style={{ fontWeight: 600, fontSize: 14 }}>{s.label}
            {s.gated && <span style={{ marginLeft: 8, fontSize: 11, color: "#999" }}>(embedder-gated)</span>}
          </div>
          <div style={{ fontSize: 12, color: "#777" }}>{s.desc}</div>
        </div>
        {s.gated ? (
          <span style={{ fontSize: 12, color: "#7d7", whiteSpace: "nowrap" }}>● active</span>
        ) : (
          <button
            disabled={busy === s.key}
            onClick={() => toggle(s.key, !on)}
            title={on ? "Disable stage" : "Enable stage"}
            style={{
              width: 52, height: 26, borderRadius: 13, border: "1px solid #ccc", cursor: "pointer",
              background: on ? "#7d7" : "#ddd", position: "relative", transition: "background .15s",
            }}
          >
            <span style={{
              position: "absolute", top: 2, left: on ? 28 : 2, width: 20, height: 20,
              borderRadius: "50%", background: "#fff", transition: "left .15s",
            }} />
          </button>
        )}
      </div>
    );
  };

  if (!loaded) return <div style={{ padding: 24, color: "#888" }}>Loading pipeline…</div>;

  const all = [...STAGES, ...GATED];
  return (
    <div style={{ padding: "18px 24px", maxWidth: 860, margin: "0 auto", fontFamily: "system-ui" }}>
      <h2 style={{ margin: "0 0 4px" }}>Curator pipeline</h2>
      <p style={{ color: "#777", fontSize: 13, margin: "0 0 18px" }}>
        Ingested content flows through these stages into curated knowledge (claims, entities,
        contradictions, graph). Toggle a stage to include/exclude it; changes persist to aimee.yaml and
        take effect on the KB's next config load. Stages run in two resource lanes concurrently.
      </p>
      {(["LLM", "INDEX"] as Lane[]).map((lane) => {
        const meta = LANE_META[lane];
        const stages = all.filter((s) => s.lane === lane);
        return (
          <div key={lane} style={{ marginBottom: 22, border: "1px solid #e2e2e2", borderRadius: 8, overflow: "hidden" }}>
            <div style={{ padding: "8px 12px", background: "#fafafa", borderBottom: "1px solid #e2e2e2" }}>
              <span style={{ fontWeight: 700, color: meta.color }}>{meta.title}</span>
              <span style={{ marginLeft: 10, fontSize: 12, color: "#999" }}>{meta.hint}</span>
            </div>
            {stages.map((s, i) => row(s, i))}
          </div>
        );
      })}
      {status && (
        <div style={{ fontSize: 13, color: status.kind === "ok" ? "#2a2" : "#c33", marginTop: 8 }}>{status.msg}</div>
      )}
    </div>
  );
}
