import { useEffect, useState, useCallback, useMemo } from 'react';
import { Badge } from '@rakuensoftware/smoothgui';
import type { BadgeVariant } from '@rakuensoftware/smoothgui';

/* The server's own tool-action audit ledger (guardrail verdict on every tool
 * call the agent makes). Server-incurred — distinct from the KB's own logs. */
interface AuditRow {
  ts: string;
  kind: string;
  actor: string;        // primary | delegate
  tool: string;
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

export default function Logs() {
  const [rows, setRows] = useState<AuditRow[]>([]);
  const [loading, setLoading] = useState(true);
  const [verdict, setVerdict] = useState('all');
  const [actor, setActor] = useState('all');
  const [q, setQ] = useState('');

  const load = useCallback(async () => {
    setLoading(true);
    try {
      const data = await fetch('/api/audit').then(r => (r.status === 401 ? [] : r.json()));
      setRows(Array.isArray(data) ? data : []);
    } catch {
      setRows([]);
    } finally {
      setLoading(false);
    }
  }, []);

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
                <tr key={i}>
                  <td style={{ ...td, whiteSpace: 'nowrap', color: '#888' }}>{esc(r.ts).replace('T', ' ').replace('Z', '')}</td>
                  <td style={td}>{esc(r.actor)}</td>
                  <td style={{ ...td, fontFamily: 'monospace' }}>{esc(r.tool)}</td>
                  <td style={td}><Badge label={esc(r.verdict)} variant={verdictVariant(r.verdict)} /></td>
                  <td style={{ ...td, color: '#888' }}>{esc(r.mode)}</td>
                  <td style={{ ...td, color: '#888' }}>{esc(r.reason_code)}</td>
                  <td style={{ ...td, textAlign: 'right', color: '#aaa' }}>{r.task_id || ''}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
    </div>
  );
}
