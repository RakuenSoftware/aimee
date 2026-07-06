import { useEffect, useState, useCallback, useMemo } from 'react';
import { Badge } from '@rakuensoftware/smoothgui';
import type { BadgeVariant } from '@rakuensoftware/smoothgui';

/* All fields of an audit row, in display order, for the detail modal. */
const DETAIL_FIELDS: { key: keyof AuditRow; label: string }[] = [
  { key: 'ts', label: 'Timestamp' },
  { key: 'verdict', label: 'Verdict' },
  { key: 'actor', label: 'Actor' },
  { key: 'tool', label: 'Tool' },
  { key: 'command', label: 'Command' },
  { key: 'kind', label: 'Kind' },
  { key: 'mode', label: 'Guardrail mode' },
  { key: 'reason_code', label: 'Reason code' },
  { key: 'args_hash', label: 'Args hash' },
  { key: 'task_id', label: 'Task id' },
];

/* The server's own tool-action audit ledger (guardrail verdict on every tool
 * call the agent makes). Server-incurred — distinct from the KB's own logs. */
interface AuditRow {
  ts: string;
  kind: string;
  actor: string;        // primary | delegate
  tool: string;
  command?: string;     // arg-free command preview (shell tools; program name only)
  args_hash?: string;
  mode?: string;        // guardrail mode
  reason_code?: string;
  verdict: string;      // allow | block | rewrite | approval_required | unknown
  task_id?: number;
}

function verdictVariant(v: string): BadgeVariant {
  if (v === 'allow') return 'success';
  if (v === 'block') return 'error';
  if (v === 'approval_required' || v === 'rewrite') return 'running';
  return 'running';
}

function esc(s: unknown): string {
  return s == null ? '' : String(s);
}

const th: React.CSSProperties = {
  textAlign: 'left', padding: '6px 10px', borderBottom: '1px solid #e0e0e0',
  color: '#666', fontSize: 11, textTransform: 'uppercase', letterSpacing: '0.3px',
  position: 'sticky', top: 0, background: '#fafafa', zIndex: 1,
};
const td: React.CSSProperties = { padding: '5px 10px', borderBottom: '1px solid #f2f2f2', fontSize: 12 };

const selectStyle: React.CSSProperties = {
  padding: '4px 8px', borderRadius: 4, border: '1px solid #ddd', background: '#fff',
  color: '#444', fontSize: 12,
};

const PAGE = 500; // rows per request (matches the server's dashboard.audit page size)

export default function Logs() {
  const [rows, setRows] = useState<AuditRow[]>([]);
  const [total, setTotal] = useState(0);
  const [loading, setLoading] = useState(true);
  const [loadingMore, setLoadingMore] = useState(false);
  const [verdict, setVerdict] = useState('all');
  const [actor, setActor] = useState('all');
  const [q, setQ] = useState('');
  const [selected, setSelected] = useState<AuditRow | null>(null);

  // Close the detail modal on Escape.
  useEffect(() => {
    if (!selected) return;
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') setSelected(null); };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [selected]);

  // Paginated fetch (most-recent first). offset=0 replaces; offset>0 appends the
  // next older page. The audit ledger can be very large, so it is never loaded in
  // one shot — the Logs page pulls pages of PAGE rows via "Load more".
  const fetchPage = useCallback(async (offset: number) => {
    const setBusy = offset === 0 ? setLoading : setLoadingMore;
    setBusy(true);
    try {
      const r = await fetch(`/api/audit?limit=${PAGE}&offset=${offset}`);
      const d = r.status === 401 ? { data: [], total: 0 } : await r.json();
      const page: AuditRow[] = Array.isArray(d?.data) ? d.data : Array.isArray(d) ? d : [];
      setTotal(typeof d?.total === 'number' ? d.total : page.length);
      setRows(prev => (offset === 0 ? page : [...prev, ...page]));
    } catch {
      if (offset === 0) { setRows([]); setTotal(0); }
    } finally {
      setBusy(false);
    }
  }, []);

  const load = useCallback(() => fetchPage(0), [fetchPage]);
  const loadMore = useCallback(() => fetchPage(rows.length), [fetchPage, rows.length]);

  useEffect(() => { load(); }, [load]);

  const filtered = useMemo(() => rows.filter(r =>
    (verdict === 'all' || r.verdict === verdict) &&
    (actor === 'all' || r.actor === actor) &&
    (q === '' || (r.tool || '').toLowerCase().includes(q.toLowerCase()))
  ), [rows, verdict, actor, q]);

  const counts = useMemo(() => {
    const c: Record<string, number> = { allow: 0, block: 0, rewrite: 0, approval_required: 0 };
    for (const r of rows) c[r.verdict] = (c[r.verdict] || 0) + 1;
    return c;
  }, [rows]);

  return (
    <div style={{ height: '100%', display: 'flex', flexDirection: 'column', overflow: 'hidden', boxSizing: 'border-box' }}>
      {/* Header / controls */}
      <div style={{
        padding: '8px 16px', background: '#fafafa', borderBottom: '1px solid #e0e0e0',
        display: 'flex', alignItems: 'center', gap: 12, flexWrap: 'wrap', flexShrink: 0,
      }}>
        <span style={{ fontSize: 14, fontWeight: 600, color: '#555' }}>Logs</span>
        <span style={{ fontSize: 12, color: '#999' }}>tool-action audit</span>
        <span style={{ fontSize: 12, color: '#999' }}>
          loaded {rows.length.toLocaleString()} of {total.toLocaleString()}
        </span>
        <div style={{ display: 'flex', gap: 6, marginLeft: 8 }}>
          {(['allow', 'block', 'rewrite', 'approval_required'] as const).map(v => (
            <span key={v} style={{ fontSize: 11, color: '#777' }}>
              {v}: <b style={{ color: '#444' }}>{counts[v] || 0}</b>
            </span>
          ))}
        </div>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 8, alignItems: 'center' }}>
          <input
            value={q}
            onChange={e => setQ(e.target.value)}
            placeholder="filter tool…"
            style={{ ...selectStyle, width: 120 }}
          />
          <select value={verdict} onChange={e => setVerdict(e.target.value)} style={selectStyle}>
            {['all', 'allow', 'block', 'rewrite', 'approval_required'].map(v => <option key={v} value={v}>{v}</option>)}
          </select>
          <select value={actor} onChange={e => setActor(e.target.value)} style={selectStyle}>
            {['all', 'primary', 'delegate'].map(a => <option key={a} value={a}>{a}</option>)}
          </select>
          <button onClick={load} style={{ ...selectStyle, cursor: 'pointer' }}>Refresh</button>
          {loading && <span style={{ fontSize: 12, color: '#aaa' }}>Loading…</span>}
        </div>
      </div>

      {/* Table (scrolls) */}
      <div style={{ flex: 1, minHeight: 0, overflow: 'auto' }}>
        {filtered.length === 0 ? (
          <div style={{ padding: 16, color: '#aaa', fontSize: 13 }}>
            {loading ? 'Loading…' : 'No audit rows'}
          </div>
        ) : (
          <table style={{ width: '100%', borderCollapse: 'collapse' }}>
            <thead>
              <tr>{['Time', 'Actor', 'Tool', 'Verdict', 'Mode', 'Reason', 'Task'].map(h => <th key={h} style={th}>{h}</th>)}</tr>
            </thead>
            <tbody>
              {filtered.map((r, i) => (
                <tr
                  key={i}
                  onClick={() => setSelected(r)}
                  style={{ cursor: 'pointer' }}
                  onMouseEnter={ev => (ev.currentTarget.style.background = '#f6f9ff')}
                  onMouseLeave={ev => (ev.currentTarget.style.background = 'transparent')}
                  title="Click for full detail"
                >
                  <td style={{ ...td, whiteSpace: 'nowrap', color: '#888' }}>{esc(r.ts).replace('T', ' ').replace('Z', '')}</td>
                  <td style={td}>{esc(r.actor)}</td>
                  <td style={{ ...td, fontFamily: 'monospace' }}>
                    {esc(r.tool)}
                    {r.command ? <span style={{ color: '#999' }}> · {esc(r.command)}</span> : null}
                  </td>
                  <td style={td}><Badge label={esc(r.verdict)} variant={verdictVariant(r.verdict)} /></td>
                  <td style={{ ...td, color: '#888' }}>{esc(r.mode)}</td>
                  <td style={{ ...td, color: '#888' }}>{esc(r.reason_code)}</td>
                  <td style={{ ...td, textAlign: 'right', color: '#aaa' }}>{r.task_id || ''}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
        {rows.length < total && (
          <div style={{ padding: 12, textAlign: 'center' }}>
            <button
              onClick={loadMore}
              disabled={loadingMore}
              style={{ ...selectStyle, cursor: 'pointer', padding: '6px 16px' }}
            >
              {loadingMore ? 'Loading…' : `Load ${Math.min(PAGE, total - rows.length).toLocaleString()} more`}
            </button>
          </div>
        )}
      </div>

      {selected && (
        <div
          onClick={() => setSelected(null)}
          style={{
            position: 'fixed', inset: 0, zIndex: 30, background: 'rgba(0,0,0,0.35)',
            display: 'flex', alignItems: 'center', justifyContent: 'center', padding: 16,
          }}
        >
          <div
            onClick={ev => ev.stopPropagation()}
            style={{
              background: '#fff', borderRadius: 8, maxWidth: 560, width: '100%',
              maxHeight: '80vh', overflow: 'auto', boxShadow: '0 8px 32px rgba(0,0,0,0.25)',
            }}
          >
            <div style={{
              display: 'flex', alignItems: 'center', gap: 10, padding: '12px 16px',
              borderBottom: '1px solid #eee', position: 'sticky', top: 0, background: '#fff',
            }}>
              <strong style={{ fontSize: 14 }}>Audit entry</strong>
              <Badge label={esc(selected.verdict)} variant={verdictVariant(selected.verdict)} />
              <button
                onClick={() => setSelected(null)}
                style={{ ...selectStyle, cursor: 'pointer', marginLeft: 'auto' }}
              >
                Close
              </button>
            </div>
            <div style={{ padding: '8px 16px 16px' }}>
              {DETAIL_FIELDS.map(f => {
                const v = selected[f.key];
                return (
                  <div key={f.key} style={{ display: 'flex', gap: 12, padding: '7px 0', borderBottom: '1px solid #f4f4f4' }}>
                    <span style={{ width: 130, flexShrink: 0, color: '#888', fontSize: 12 }}>{f.label}</span>
                    <span style={{ fontSize: 13, fontFamily: 'monospace', overflowWrap: 'anywhere', minWidth: 0 }}>
                      {v == null || v === '' ? <span style={{ color: '#bbb' }}>—</span> : String(v)}
                    </span>
                  </div>
                );
              })}
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
