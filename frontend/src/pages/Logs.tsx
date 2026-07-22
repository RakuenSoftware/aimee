import { useEffect, useState, useCallback, useMemo } from 'react';
import { Badge, Button, Modal, EmptyState, DataTable } from '@rakuensoftware/smoothgui';
import type { BadgeVariant, Column } from '@rakuensoftware/smoothgui';

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

const AUDIT_COLUMNS: Column<AuditRow>[] = [
  { key: 'ts', label: 'Time', render: r => <span style={{ whiteSpace: 'nowrap', color: '#888' }}>{esc(r.ts).replace('T', ' ').replace('Z', '')}</span> },
  { key: 'actor', label: 'Actor', render: r => esc(r.actor) },
  {
    key: 'tool', label: 'Tool',
    render: r => (
      <span style={{ fontFamily: 'monospace' }}>
        {esc(r.tool)}
        {r.command ? <span style={{ color: '#999' }}> · {esc(r.command)}</span> : null}
      </span>
    ),
  },
  { key: 'verdict', label: 'Verdict', render: r => <Badge label={esc(r.verdict)} variant={verdictVariant(r.verdict)} /> },
  { key: 'mode', label: 'Mode', render: r => <span style={{ color: '#888' }}>{esc(r.mode)}</span> },
  { key: 'reason_code', label: 'Reason', render: r => <span style={{ color: '#888' }}>{esc(r.reason_code)}</span> },
  { key: 'task_id', label: 'Task', align: 'right', render: r => <span style={{ color: '#aaa' }}>{r.task_id || ''}</span> },
];

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
          <select value={verdict} onChange={e => setVerdict(e.target.value)} style={selectStyle} title="Filter rows to a single guardrail verdict.">
            {['all', 'allow', 'block', 'rewrite', 'approval_required'].map(v => <option key={v} value={v}>{v}</option>)}
          </select>
          <select value={actor} onChange={e => setActor(e.target.value)} style={selectStyle} title="Filter rows by actor (primary agent or delegate).">
            {['all', 'primary', 'delegate'].map(a => <option key={a} value={a}>{a}</option>)}
          </select>
          <Button size="sm" onClick={load} title="Reload the newest page of audit rows.">Refresh</Button>
          {loading && <span style={{ fontSize: 12, color: '#aaa' }}>Loading…</span>}
        </div>
      </div>

      {/* Table (scrolls) */}
      <div style={{ flex: 1, minHeight: 0, overflow: 'auto' }}>
        {filtered.length === 0 ? (
          loading ? (
            <div style={{ padding: 16, color: '#aaa', fontSize: 13 }}>Loading…</div>
          ) : (
            <EmptyState message="No audit rows" inline />
          )
        ) : (
          <DataTable
            columns={AUDIT_COLUMNS}
            rows={filtered}
            rowKey={(_r, i) => i}
            onRowClick={r => setSelected(r)}
          />
        )}
        {rows.length < total && (
          <div style={{ padding: 12, textAlign: 'center' }}>
            <Button
              size="sm"
              onClick={loadMore}
              disabled={loadingMore}
              style={{ padding: '6px 16px' }}
            >
              {loadingMore ? 'Loading…' : `Load ${Math.min(PAGE, total - rows.length).toLocaleString()} more`}
            </Button>
          </div>
        )}
      </div>

      {/* `open` is constant true: the surrounding `selected &&` already gates
          mount/unmount, so Modal is only rendered when there is a row to show. */}
      {selected && (
        <Modal
          open
          onClose={() => setSelected(null)}
          title="Audit entry"
          headerExtra={<Badge label={esc(selected.verdict)} variant={verdictVariant(selected.verdict)} />}
        >
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
        </Modal>
      )}
    </div>
  );
}
