import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useSessions } from '../SessionContext';

type MemoryId = number | string;

interface MemoryRow {
  id: MemoryId; tier: string; kind: string; key: string; content: string;
  confidence: number; lifecycle: string; review_reason: string;
  scope_type: string; scope_value: string; updated_at?: string;
}

async function memoryPost<T>(path: string, body: unknown): Promise<T> {
  const response = await fetch(path, {
    method: 'POST',
    credentials: 'same-origin',
    headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
    body: JSON.stringify(body),
  });
  const data = await response.json();
  if (!response.ok || data.status === 'error') throw new Error(data.message || `HTTP ${response.status}`);
  return data as T;
}

export default function Memory() {
  const { active } = useSessions();
  const [rows, setRows] = useState<MemoryRow[]>([]);
  const [view, setView] = useState('active');
  const [store, setStore] = useState('user');
  const requestVersion = useRef(0);
  const [busy, setBusy] = useState<MemoryId | null>(null);
  const [error, setError] = useState('');
  const [reloadTick, setReloadTick] = useState(0);

  const scope = useMemo(() => ({ store, cwd: store === 'kb' ? active?.projectRoot || undefined : undefined }), [store, active?.projectRoot]);
  const load = useCallback(async () => {
    const version = ++requestVersion.current;
    if (view === 'corrections') return;
    try {
      const result = await memoryPost<{ memories?: MemoryRow[] }>('/v1/memory/review', { ...scope, limit: 64, state: view === 'active' || view === 'retired' ? view : undefined });
      if (version !== requestVersion.current) return;
      setRows(result.memories ?? []);
      setError('');
    } catch (e) { if (version === requestVersion.current) setError(String(e)); }
  }, [scope, view]);
  useEffect(() => { void load(); }, [load, reloadTick]);

  const visible = rows.filter((row) => view === 'all' ||
    (view === 'review' ? row.lifecycle === 'pending' || row.lifecycle === 'rejected' : row.lifecycle === view));

  async function decide(row: MemoryRow, action: 'reject' | 'restore' | 'retire') {
    const reason = action === 'reject'
      ? window.prompt('Why is this memory wrong?', row.review_reason || '') : '';
    if (action === 'reject' && reason === null) return;
    setBusy(row.id);
    try {
      await memoryPost(action === 'retire' ? '/v1/memory/delete' : action === 'reject' ? '/v1/memory/reject' : '/v1/memory/restore', {
        ...scope, id: row.id, reason: reason || undefined,
      });
      // Refresh the currently selected view, even if it changed while the
      // mutation was in flight; an old closure must not replace its rows.
      setReloadTick((value) => value + 1);
    } catch (e) { setError(String(e)); }
    finally { setBusy(null); }
  }

  return (
    <div style={{ padding: 24, maxWidth: 1180, margin: '0 auto' }}>
      <h1 style={{ marginTop: 0 }}>Memory Center</h1>
      <p style={{ color: 'var(--sg-text-muted)' }}>
        Personal memories stay in your local store. Knowledge base memories are shared and can be reviewed by project.
      </p>
      {store === 'kb' && !active?.projectRoot && <p style={{ color: 'var(--sg-warning-dark)' }}>No project is bound to the active session; only global memories are shown.</p>}
      <div style={{ display: 'flex', gap: 8, alignItems: 'center', marginBottom: 16 }}>
        <select aria-label="Memory store" value={store} disabled={busy !== null} onChange={(e) => {
          ++requestVersion.current;
          setRows([]);
          setError('');
          setStore(e.target.value);
          setView('active');
        }}>
          <option value="user">Personal (local)</option><option value="kb">Knowledge base</option>
        </select>
        <select aria-label="Memory view" value={view} onChange={(e) => setView(e.target.value)}>
          <option value="corrections">Correction proposals</option><option value="review">Needs attention</option><option value="active">Active</option>
          <option value="pending">Pending</option><option value="rejected">Rejected</option>
          <option value="retired">Retired</option><option value="archived">Archived</option><option value="all">All history</option>
        </select>
        {view !== 'corrections' && <button onClick={() => void load()}>Refresh</button>}
        <span style={{ color: 'var(--sg-text-faint)' }}>{visible.length} shown</span>
      </div>
      {error && <p style={{ color: 'var(--sg-danger-dark)' }}>{error}</p>}
      {view === 'corrections' ? <CorrectionReview key={`${scope.store}:${scope.cwd || ''}`} scope={scope} /> : <div style={{ display: 'grid', gap: 10 }}>
        {visible.map((row) => (
          <article key={row.id} style={{ border: '1px solid var(--sg-border)', borderRadius: 8, padding: 14 }}>
            <div style={{ display: 'flex', gap: 10, justifyContent: 'space-between', flexWrap: 'wrap' }}>
              <strong>{row.key}</strong>
              <span>{row.lifecycle} · {row.scope_type}:{row.scope_value} · confidence {row.confidence.toFixed(2)}</span>
            </div>
            <div style={{ whiteSpace: 'pre-wrap', margin: '8px 0' }}>{row.content}</div>
            {row.review_reason && <div style={{ color: 'var(--sg-danger-dark)' }}>Reason: {row.review_reason}</div>}
            <div style={{ display: 'flex', gap: 8, alignItems: 'center', marginTop: 8 }}>
              <code>{store}:memory:{row.id}</code><small style={{ color: 'var(--sg-text-faint)' }}>{row.tier} · {row.kind} · {row.updated_at}</small>
              <span style={{ flex: 1 }} />
              {store === 'user'
                ? row.lifecycle === 'active' && <button disabled={busy === row.id} onClick={() => void decide(row, 'retire')}>Retire</button>
                : row.lifecycle === 'rejected'
                ? <button disabled={busy === row.id} onClick={() => void decide(row, 'restore')}>Restore</button>
                : (row.lifecycle === 'active' || row.lifecycle === 'pending')
                  ? <button disabled={busy === row.id} onClick={() => void decide(row, 'reject')}>Reject</button> : null}
            </div>
          </article>
        ))}
        {visible.length === 0 && <p style={{ color: 'var(--sg-text-faint)' }}>No memories match this view.</p>}
      </div>}
    </div>
  );
}

interface RecordVersion {
  schema_version: number; owner_id: string; record_id: string; record_revision: string;
}
interface NativeVersion {
  commit: string; operation: string; belief: string;
}
interface ProposedValidity {
  valid_from: string | null; valid_until: string | null;
}
interface RequiredProcessing {
  producer: { extraction: string; synthesis_model: string; embedding_generation: string };
  baseline: { entity: string; field: string; selector: string; version: NativeVersion };
}
interface ProposalDraft {
  content: string; tier: string; confidence: number; use_cases: string; epistemic_kind: string;
  native_validity?: ProposedValidity | null;
  native_required_jobs?: RequiredProcessing | null;
  native_inputs?: NativeVersion[];
}
interface Proposal {
  proposal_id: string; payload_digest: string; target_version: RecordVersion;
  state: string; proposer: string; reviewer?: string; decision_id?: string;
  draft?: ProposalDraft;
}
function ProposedMetadata({ draft }: { draft: ProposalDraft }) {
  return <section aria-label="Proposed validity and processing">
    <h4>Proposed validity</h4>
    {draft.native_validity === undefined
      ? <p>This proposal format does not supply validity details.</p>
      : draft.native_validity === null
        ? <p>No logical validity bounds.</p>
        : <dl>
          <dt>Valid from (inclusive UTC seconds)</dt><dd><code>{draft.native_validity.valid_from ?? 'Unbounded'}</code></dd>
          <dt>Valid until (exclusive UTC seconds)</dt><dd><code>{draft.native_validity.valid_until ?? 'Unbounded'}</code></dd>
        </dl>}
    <h4>Required processing jobs</h4>
    {draft.native_required_jobs === undefined
      ? <p>This proposal format does not supply processing details.</p>
      : draft.native_required_jobs === null
        ? <p>No required processing jobs.</p>
        : <dl>
          <dt>Extraction producer</dt><dd><code>{draft.native_required_jobs.producer.extraction}</code></dd>
          <dt>Synthesis model</dt><dd><code>{draft.native_required_jobs.producer.synthesis_model}</code></dd>
          <dt>Embedding generation</dt><dd><code>{draft.native_required_jobs.producer.embedding_generation}</code></dd>
          <dt>Proposed baseline reference</dt>
          <dd><pre style={{ whiteSpace: 'pre-wrap', overflowWrap: 'anywhere' }}>{JSON.stringify(draft.native_required_jobs.baseline, null, 2)}</pre></dd>
        </dl>}
    {draft.native_inputs !== undefined && <details>
      <summary>Proposed input versions ({draft.native_inputs.length})</summary>
      <pre style={{ whiteSpace: 'pre-wrap', overflowWrap: 'anywhere' }}>{JSON.stringify(draft.native_inputs, null, 2)}</pre>
    </details>}
  </section>;
}
function sameVersion(a: RecordVersion | undefined, b: RecordVersion) {
  return a?.schema_version === b.schema_version && a.owner_id === b.owner_id &&
    a.record_id === b.record_id && a.record_revision === b.record_revision;
}

function CorrectionReview({ scope }: { scope: { store: string; cwd?: string } }) {
  const [proposals, setProposals] = useState<Proposal[]>([]);
  const [selected, setSelected] = useState<Proposal>();
  const [source, setSource] = useState<{ content: string; version?: RecordVersion }>();
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);
  const generation = useRef(0);
  const load = useCallback(async () => {
    const request = ++generation.current;
    setSelected(undefined); setSource(undefined); setError('');
    try {
      const result = await memoryPost<{ proposals: Proposal[] }>('/v1/memory/correction_proposals', { ...scope, limit: 100 });
      if (request === generation.current) setProposals(result.proposals);
    } catch (e) { if (request === generation.current) setError(String(e)); }
  }, [scope]);
  useEffect(() => { void load(); return () => { ++generation.current; }; }, [load]);

  async function inspect(proposal: Proposal) {
    const request = ++generation.current;
    setSelected(undefined); setSource(undefined); setError(''); setBusy(true);
    try {
      const result = await memoryPost<{ proposals: Proposal[] }>('/v1/memory/correction_proposals', { ...scope, proposal_id: proposal.proposal_id });
      if (request !== generation.current) return;
      const full = result.proposals[0];
      if (!full?.draft) throw new Error('Proposal draft is unavailable.');
      const current = await memoryPost<{ memory: { content: string; version?: RecordVersion } }>('/v1/memory/get', {
        ...scope, id: full.target_version.record_id, include_version: true,
      });
      if (request !== generation.current) return;
      setSelected(full); setSource(current.memory);
    } catch (e) { if (request === generation.current) setError(String(e)); }
    finally { if (request === generation.current) setBusy(false); }
  }
  async function review(action: 'approve' | 'reject') {
    if (!selected || busy) return;
    const request = ++generation.current;
    setBusy(true); setError('');
    try {
      await memoryPost('/v1/memory/review_correction', {
        ...scope, proposal_id: selected.proposal_id, payload_digest: selected.payload_digest,
        expected_version: selected.target_version, action,
      });
      if (request === generation.current) await load();
    } catch (e) { if (request === generation.current) setError(String(e)); }
    finally { setBusy(false); }
  }
  const stale = selected && !sameVersion(source?.version, selected.target_version);
  return <section aria-label="Correction proposals">
    <h2>Correction proposals</h2>
    <p>Review model suggestions for the selected store. Approval uses the exact proposed revision; changed records require a fresh proposal.</p>
    <button disabled={busy} onClick={() => void load()}>Refresh proposals</button>
    {error && <p role="alert">{error}</p>}
    {proposals.length === 0 && <p>No correction proposals.</p>}
    {proposals.map((p) => <article key={p.proposal_id}>
      <p>Memory {p.target_version.record_id}, revision {p.target_version.record_revision}: {p.state} · Proposed by {p.proposer}</p>
      {p.reviewer && <p>Reviewed by {p.reviewer} · Decision {p.decision_id}</p>}
      <button disabled={busy} onClick={() => void inspect(p)}>Inspect {p.proposal_id}</button>
    </article>)}
    {selected && <article>
      <h3>Current content</h3><pre style={{ whiteSpace: 'pre-wrap' }}>{source?.content}</pre>
      <h3>Proposed content</h3><pre style={{ whiteSpace: 'pre-wrap' }}>{selected.draft?.content}</pre>
      <p>Tier {selected.draft?.tier} · Confidence {selected.draft?.confidence} · Kind {selected.draft?.epistemic_kind}</p>
      <p>Use cases: {selected.draft?.use_cases}</p>
      {selected.draft && <ProposedMetadata draft={selected.draft} />}
      {stale && <p role="alert">The source revision has changed. Refresh and request a new proposal.</p>}
      {selected.state === 'pending' && <>
        <button disabled={busy || !!stale} onClick={() => void review('approve')}>Approve correction</button>
        <button disabled={busy || !!stale} onClick={() => void review('reject')}>Reject correction</button>
      </>}
    </article>}
  </section>;
}
