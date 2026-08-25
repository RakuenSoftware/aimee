import { useCallback, useEffect, useMemo, useState } from 'react';
import { apiGet, apiSend } from '../api';

interface MemoryRow {
  id: number;
  tier: string;
  kind: string;
  key: string;
  content: string;
  confidence: number;
  lifecycle: string;
  review_reason: string;
  scope_type: string;
  scope_value: string;
  created_at: string;
  updated_at: string;
}

interface MemoryReviewResponse {
  memories: MemoryRow[];
  count: number;
}

export default function Memories() {
  const [rows, setRows] = useState<MemoryRow[]>([]);
  const [filter, setFilter] = useState('review');
  const [busy, setBusy] = useState<number | null>(null);
  const [error, setError] = useState('');

  const load = useCallback(async () => {
    try {
      const result = await apiGet<MemoryReviewResponse>('/v1/console/memories');
      setRows(result.memories ?? []);
      setError('');
    } catch (e) {
      setError(String(e));
    }
  }, []);

  useEffect(() => { void load(); }, [load]);

  const visible = useMemo(() => rows.filter((row) => {
    if (filter === 'all') return true;
    if (filter === 'review') return row.lifecycle === 'pending' || row.lifecycle === 'rejected';
    return row.lifecycle === filter;
  }), [rows, filter]);

  async function decide(row: MemoryRow, action: 'reject' | 'restore') {
    const reason = action === 'reject'
      ? window.prompt('Why is this memory wrong? This reason is retained with its tombstone.', row.review_reason || '')
      : '';
    if (action === 'reject' && reason === null) return;
    setBusy(row.id);
    try {
      await apiSend('POST', '/v1/console/memories/review', {
        memory_id: row.id,
        action,
        reason: reason || undefined,
      });
      await load();
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(null);
    }
  }

  return (
    <section>
      <h1>Memories</h1>
      <p className="kbc-muted">
        Inspect retained memory exactly as the system stores it. Rejecting creates durable negative
        memory, keeps the row out of recall, and refuses automatic re-extraction until a human restores it.
      </p>
      <div style={{ display: 'flex', gap: 8, alignItems: 'center', marginBottom: 14 }}>
        <label>Show{' '}
          <select value={filter} onChange={(e) => setFilter(e.target.value)}>
            <option value="review">Needs attention</option>
            <option value="active">Active</option>
            <option value="pending">Pending</option>
            <option value="rejected">Rejected</option>
            <option value="archived">Archived</option>
            <option value="all">All history</option>
          </select>
        </label>
        <button onClick={() => void load()}>Refresh</button>
        <span className="kbc-muted">{visible.length} shown</span>
      </div>
      {error && <p className="kbc-error">{error}</p>}
      <table className="kbc-table">
        <thead>
          <tr><th>Memory</th><th>State</th><th>Scope</th><th>Confidence</th><th>Action</th></tr>
        </thead>
        <tbody>
          {visible.map((row) => (
            <tr key={row.id}>
              <td style={{ maxWidth: 620 }}>
                <div><strong>{row.key}</strong> <code>memory:{row.id}</code></div>
                <div style={{ whiteSpace: 'pre-wrap', marginTop: 4 }}>{row.content}</div>
                {row.review_reason && <div className="kbc-error">Reason: {row.review_reason}</div>}
                <small className="kbc-muted">{row.tier} · {row.kind} · updated {row.updated_at}</small>
              </td>
              <td><span className={`kbc-badge ${row.lifecycle === 'active' ? 'kbc-badge-ok' : row.lifecycle === 'rejected' ? 'kbc-badge-err' : 'kbc-badge-warn'}`}>{row.lifecycle}</span></td>
              <td><code>{row.scope_type}:{row.scope_value}</code></td>
              <td>{row.confidence.toFixed(2)}</td>
              <td>
                {row.lifecycle === 'rejected' ? (
                  <button disabled={busy === row.id} onClick={() => void decide(row, 'restore')}>Restore</button>
                ) : row.lifecycle === 'active' || row.lifecycle === 'pending' ? (
                  <button disabled={busy === row.id} onClick={() => void decide(row, 'reject')}>Reject</button>
                ) : null}
              </td>
            </tr>
          ))}
          {visible.length === 0 && <tr><td colSpan={5} className="kbc-muted">No memories match this view.</td></tr>}
        </tbody>
      </table>
    </section>
  );
}
