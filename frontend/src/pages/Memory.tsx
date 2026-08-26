import { useCallback, useEffect, useMemo, useState } from 'react';
import { useSessions } from '../SessionContext';

interface MemoryRow {
  id: number; tier: string; kind: string; key: string; content: string;
  confidence: number; lifecycle: string; review_reason: string;
  scope_type: string; scope_value: string; updated_at: string;
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
  const [view, setView] = useState('review');
  const [busy, setBusy] = useState<number | null>(null);
  const [error, setError] = useState('');

  const scope = useMemo(() => ({ cwd: active?.projectRoot || undefined }), [active?.projectRoot]);
  const load = useCallback(async () => {
    try {
      const result = await memoryPost<{ memories?: MemoryRow[] }>('/v1/memory/review', { ...scope, limit: 64 });
      setRows(result.memories ?? []);
      setError('');
    } catch (e) { setError(String(e)); }
  }, [scope]);
  useEffect(() => { void load(); }, [load]);

  const visible = rows.filter((row) => view === 'all' ||
    (view === 'review' ? row.lifecycle === 'pending' || row.lifecycle === 'rejected' : row.lifecycle === view));

  async function decide(row: MemoryRow, action: 'reject' | 'restore') {
    const reason = action === 'reject'
      ? window.prompt('Why is this memory wrong?', row.review_reason || '') : '';
    if (action === 'reject' && reason === null) return;
    setBusy(row.id);
    try {
      await memoryPost(action === 'reject' ? '/v1/memory/reject' : '/v1/memory/restore', {
        ...scope, id: row.id, reason: reason || undefined,
      });
      await load();
    } catch (e) { setError(String(e)); }
    finally { setBusy(null); }
  }

  return (
    <div style={{ padding: 24, maxWidth: 1180, margin: '0 auto' }}>
      <h1 style={{ marginTop: 0 }}>Memory Center</h1>
      <p style={{ color: 'var(--sg-text-muted)' }}>
        Review memories visible to the active project. Rejecting keeps a durable tombstone so the same
        value cannot return through extraction; restoring is an explicit human decision.
      </p>
      {!active?.projectRoot && <p style={{ color: 'var(--sg-warning-dark)' }}>No project is bound to the active session; only global memories are shown.</p>}
      <div style={{ display: 'flex', gap: 8, alignItems: 'center', marginBottom: 16 }}>
        <select value={view} onChange={(e) => setView(e.target.value)}>
          <option value="review">Needs attention</option><option value="active">Active</option>
          <option value="pending">Pending</option><option value="rejected">Rejected</option>
          <option value="archived">Archived</option><option value="all">All history</option>
        </select>
        <button onClick={() => void load()}>Refresh</button>
        <span style={{ color: 'var(--sg-text-faint)' }}>{visible.length} shown</span>
      </div>
      {error && <p style={{ color: 'var(--sg-danger-dark)' }}>{error}</p>}
      <div style={{ display: 'grid', gap: 10 }}>
        {visible.map((row) => (
          <article key={row.id} style={{ border: '1px solid var(--sg-border)', borderRadius: 8, padding: 14 }}>
            <div style={{ display: 'flex', gap: 10, justifyContent: 'space-between', flexWrap: 'wrap' }}>
              <strong>{row.key}</strong>
              <span>{row.lifecycle} · {row.scope_type}:{row.scope_value} · confidence {row.confidence.toFixed(2)}</span>
            </div>
            <div style={{ whiteSpace: 'pre-wrap', margin: '8px 0' }}>{row.content}</div>
            {row.review_reason && <div style={{ color: 'var(--sg-danger-dark)' }}>Reason: {row.review_reason}</div>}
            <div style={{ display: 'flex', gap: 8, alignItems: 'center', marginTop: 8 }}>
              <code>memory:{row.id}</code><small style={{ color: 'var(--sg-text-faint)' }}>{row.tier} · {row.kind} · {row.updated_at}</small>
              <span style={{ flex: 1 }} />
              {row.lifecycle === 'rejected'
                ? <button disabled={busy === row.id} onClick={() => void decide(row, 'restore')}>Restore</button>
                : (row.lifecycle === 'active' || row.lifecycle === 'pending')
                  ? <button disabled={busy === row.id} onClick={() => void decide(row, 'reject')}>Reject</button> : null}
            </div>
          </article>
        ))}
        {visible.length === 0 && <p style={{ color: 'var(--sg-text-faint)' }}>No memories match this view.</p>}
      </div>
    </div>
  );
}
