import { useEffect, useState, useCallback } from 'react';
import { Badge, Button, Panel, EmptyState, StatusDot, DataTable } from '@rakuensoftware/smoothgui';
import type { BadgeVariant, Column } from '@rakuensoftware/smoothgui';

/* ---- API types ---- */

interface Delegation {
  agent: string;
  role: string;
  success: boolean;
  turns: number;
  tool_calls: number;
  latency_ms: number;
  confidence?: number;
}

interface Metric {
  role: string;
  total: number;
  successes: number;
  avg_latency_ms: number;
  tokens: number;
}

interface Trace {
  turn: number;
  tool_name?: string;
  direction: string;
}

interface MemoryStat {
  tier: string;
  functional_name?: string;
  kind: string;
  count: number;
}

interface Plan {
  id: number;
  agent: string;
  status: string;
  done_steps: number;
  total_steps: number;
  task: string;
}

interface LogEntry {
  timestamp: string;
  source: string;
  who: string;
  what: string;
  detail: string;
}

interface LspStatus {
  errors: number;
  warnings: number;
  active_servers: number;
}

// The server-hosted dashboard reports the configured agent roster (from
// `server_agent_list_json`), not a live presence feed — so these are provider
// config rows, keyed on `enabled`, not heartbeat/slot state.
interface Agent {
  name: string;
  provider?: string;
  model?: string;
  enabled?: boolean;
  tools_enabled?: boolean;
  roles?: string[];
  personas?: string[];
}

interface TokenAudit {
  tool_name: string;
  role: string;
  prompt_tokens: number;
  completion_tokens: number;
  cache_write_tokens: number;
  cache_read_tokens: number;
  estimated_cost_usd: number;
  call_count: number;
  last_seen: string;
}

interface Decision {
  id: number;
  options: string;
  chosen: string;
  rationale: string;
  outcome: string;
  created_at: string;
}

interface Session {
  id: string;
  title: string;
  cwd: string;
  created_at: string;
  last_active: string;
}

// Verdict-mix counts over the WHOLE audit ledger (server-aggregated in
// dashboard.all as `audit_summary`), so the Guardrail pane is not sample-capped.
interface AuditSummary {
  total: number;
  allow: number;
  block: number;
  rewrite: number;
  approval_required: number;
  other: number;
}

interface OnboardStep {
  step: string;
  status: 'ok' | 'warn' | 'error' | 'skipped';
  warnings?: number;
  errors?: number;
  message?: string;
}

interface OnboardReport {
  version: string;
  ready: boolean;
  elapsed_ms: number;
  steps: OnboardStep[];
  next_actions: string[];
}

/* One server-incurred tool-action guardrail verdict (the per-row audit ledger the
 * server sends as `audit`; `auditSummary` is its aggregate). */
interface AuditRow {
  ts: string;
  kind: string;
  actor: string;
  tool: string;
  mode?: string;
  reason_code?: string;
  verdict: string;
  task_id?: number;
}

interface DashData {
  delegations: Delegation[];
  metrics: Metric[];
  traces: Trace[];
  memory: MemoryStat[];
  plans: Plan[];
  logs: LogEntry[];
  lsp: LspStatus | null;
  agents: Agent[];
  onboard: OnboardReport | null;
  tokenAudit: TokenAudit[];
  decisions: Decision[];
  sessions: Session[];
  audit: AuditRow[];
  auditSummary: AuditSummary;
}

/* The server (`dashboard.all`) returns some fields in shapes the panels can't
 * consume directly: `memory_stats` is an object whose per-kind rows live under
 * `tier_kinds`, `onboard` may be an `{error}` stub, and `agents` is the config
 * roster. `RawDashboard` types that wire payload; `toDashData` normalizes it. */
interface RawMemoryStats {
  tier_kinds?: MemoryStat[];
  tiers?: unknown;
  scopes?: unknown;
}

interface RawDashboard {
  delegations?: Delegation[];
  metrics?: Metric[];
  traces?: Trace[];
  memory_stats?: RawMemoryStats | MemoryStat[];
  plans?: Plan[];
  logs?: LogEntry[];
  lsp?: LspStatus;
  agents?: Agent[];
  onboard?: (OnboardReport & { error?: string }) | { error: string };
  token_audit?: TokenAudit[];
  decisions?: Decision[];
  sessions?: Session[];
  audit?: AuditRow[];
  audit_summary?: AuditSummary;
}

/* ---- Helpers ---- */

function e(s: unknown): string {
  if (s == null) return '';
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

function planVariant(status: string): BadgeVariant {
  if (status === 'done') return 'success';
  if (status === 'failed') return 'error';
  return 'running';
}

/* Human-readable duration. Raw milliseconds (e.g. "127428ms") read as broken for
 * multi-second LLM delegations, so format as ms / s / m+s. */
export function fmtDuration(ms: number): string {
  if (!Number.isFinite(ms) || ms < 0) return '—';
  if (ms < 1000) return `${Math.round(ms)}ms`;
  const s = ms / 1000;
  if (s < 60) return `${s.toFixed(s < 10 ? 1 : 0)}s`;
  const m = Math.floor(s / 60);
  const rem = Math.round(s - m * 60);
  return `${m}m ${rem}s`;
}

/* Compact token / count formatting: 4380809 -> "4.4M", 6250 -> "6.3k". */
export function fmtCompact(n: number): string {
  if (!Number.isFinite(n)) return '0';
  const abs = Math.abs(n);
  if (abs >= 1e9) return `${(n / 1e9).toFixed(1)}B`;
  if (abs >= 1e6) return `${(n / 1e6).toFixed(1)}M`;
  if (abs >= 1e3) return `${(n / 1e3).toFixed(1)}k`;
  return String(Math.round(n));
}

/* USD cost: sub-cent shows "<$0.01", else two decimals. */
export function fmtUsd(n: number): string {
  if (!Number.isFinite(n) || n <= 0) return '$0.00';
  if (n < 0.01) return '<$0.01';
  return `$${n.toFixed(n < 100 ? 2 : 0)}`;
}

const arr = <T,>(v: unknown): T[] => (Array.isArray(v) ? (v as T[]) : []);

/* Normalize the raw `dashboard.all` payload into the shapes the panels render.
 * Exported so the contract (server shape -> panel shape) is unit-tested against
 * a captured live payload — the panels themselves stay dumb over clean data. */
export function toDashData(raw: RawDashboard | null | undefined): DashData {
  const r = raw ?? {};
  // `memory_stats` is an object with rows under `tier_kinds` (per tier+kind),
  // but a legacy/plain array is tolerated too.
  const memRaw = r.memory_stats;
  const memory = Array.isArray(memRaw) ? memRaw : arr<MemoryStat>(memRaw?.tier_kinds);
  // Only a real onboard report (with a `steps` array) is renderable; the
  // server's `{error: …}` stub falls back to null.
  const onboard = r.onboard;
  return {
    delegations: arr<Delegation>(r.delegations),
    metrics: arr<Metric>(r.metrics),
    traces: arr<Trace>(r.traces),
    memory,
    plans: arr<Plan>(r.plans),
    logs: arr<LogEntry>(r.logs),
    lsp: r.lsp ?? null,
    agents: arr<Agent>(r.agents),
    onboard: Array.isArray((onboard as OnboardReport | undefined)?.steps)
      ? (onboard as OnboardReport)
      : null,
    tokenAudit: arr<TokenAudit>(r.token_audit),
    decisions: arr<Decision>(r.decisions),
    sessions: arr<Session>(r.sessions),
    audit: arr<AuditRow>(r.audit),
    auditSummary: r.audit_summary || {
      total: 0, allow: 0, block: 0, rewrite: 0, approval_required: 0, other: 0,
    },
  };
}

/* ---- Panels ---- */

const DELEGATION_COLUMNS: Column<Delegation>[] = [
  { key: 'agent', label: 'Agent', render: r => e(r.agent) },
  { key: 'role', label: 'Role', render: r => e(r.role) },
  {
    key: 'status', label: 'Status',
    render: r => <Badge label={r.success ? 'OK' : 'ERR'} variant={r.success ? 'success' : 'error'} />,
  },
  { key: 'turns', label: 'Turns', align: 'right', render: r => r.turns },
  { key: 'tools', label: 'Tools', align: 'right', render: r => r.tool_calls },
  { key: 'latency', label: 'Latency', align: 'right', render: r => <span title={`${r.latency_ms}ms`}>{fmtDuration(r.latency_ms)}</span> },
];

function DelegationsPanel({ data }: { data: Delegation[] }) {
  return (
    <Panel title="Delegations" count={data.length}>
      <DataTable columns={DELEGATION_COLUMNS} rows={data} rowKey={(_r, i) => i} />
    </Panel>
  );
}

const METRIC_COLUMNS: Column<Metric>[] = [
  { key: 'role', label: 'Role', render: r => e(r.role) },
  { key: 'total', label: 'Total', align: 'right', render: r => r.total },
  { key: 'ok', label: 'OK', align: 'right', render: r => r.successes },
  { key: 'avglat', label: 'Avg Lat', align: 'right', render: r => <span title={`${r.avg_latency_ms}ms`}>{fmtDuration(r.avg_latency_ms)}</span> },
  { key: 'tokens', label: 'Tokens', align: 'right', render: r => <span title={`${r.tokens} tokens`}>{fmtCompact(r.tokens)}</span> },
];

function MetricsPanel({ data }: { data: Metric[] }) {
  return (
    <Panel title="Metrics">
      <DataTable columns={METRIC_COLUMNS} rows={data} rowKey={(_r, i) => i} />
    </Panel>
  );
}

const PLAN_COLUMNS: Column<Plan>[] = [
  { key: 'id', label: 'ID', align: 'right', render: r => r.id },
  { key: 'agent', label: 'Agent', render: r => e(r.agent) },
  { key: 'status', label: 'Status', render: r => <Badge label={r.status} variant={planVariant(r.status)} /> },
  { key: 'steps', label: 'Steps', align: 'right', render: r => `${r.done_steps}/${r.total_steps}` },
  { key: 'task', label: 'Task', render: r => e(r.task.substring(0, 60)) },
];

function PlansPanel({ data }: { data: Plan[] }) {
  return (
    <Panel title="Execution Plans">
      <DataTable columns={PLAN_COLUMNS} rows={data} rowKey={(_r, i) => i} />
    </Panel>
  );
}

const TRACE_COLUMNS: Column<Trace>[] = [
  { key: 'turn', label: 'Turn', align: 'right', render: r => r.turn },
  { key: 'tool', label: 'Tool', render: r => e(r.tool_name ?? '--') },
  { key: 'dir', label: 'Dir', render: r => e(r.direction) },
];

function TracesPanel({ data }: { data: Trace[] }) {
  return (
    <Panel title="Traces">
      <DataTable columns={TRACE_COLUMNS} rows={data} rowKey={(_r, i) => i} />
    </Panel>
  );
}

const MEMORY_COLUMNS: Column<MemoryStat>[] = [
  {
    key: 'tier', label: 'Tier',
    render: r => (
      <>
        {e(r.tier)}
        {r.functional_name ? (
          <span style={{ color: '#aaa', marginLeft: '6px' }}>{e(r.functional_name)}</span>
        ) : null}
      </>
    ),
  },
  { key: 'kind', label: 'Kind', render: r => e(r.kind) },
  { key: 'count', label: 'Count', align: 'right', render: r => r.count },
];

function MemoryPanel({ data }: { data: MemoryStat[] }) {
  return (
    <Panel title="Memory">
      {data.length === 0 ? (
        <div style={{ padding: '12px', color: '#aaa', fontSize: '12px' }}>
          No memories recorded
        </div>
      ) : (
        <DataTable columns={MEMORY_COLUMNS} rows={data} rowKey={(_r, i) => i} />
      )}
    </Panel>
  );
}

/* Cost / Token spend — realized spend rolled up per role, plus a headline total. */
type CostRow = [string, { tokens: number; cost: number; calls: number }];

function CostPanel({ data }: { data: TokenAudit[] }) {
  const totalCost = data.reduce((n, r) => n + (r.estimated_cost_usd || 0), 0);
  const totalTokens = data.reduce(
    (n, r) => n + (r.prompt_tokens || 0) + (r.completion_tokens || 0), 0);
  // Roll up per role so the table stays compact regardless of tool cardinality.
  const byRole = new Map<string, { tokens: number; cost: number; calls: number }>();
  for (const r of data) {
    const key = r.role || r.tool_name || '—';
    const cur = byRole.get(key) ?? { tokens: 0, cost: 0, calls: 0 };
    cur.tokens += (r.prompt_tokens || 0) + (r.completion_tokens || 0);
    cur.cost += r.estimated_cost_usd || 0;
    cur.calls += r.call_count || 0;
    byRole.set(key, cur);
  }
  const rows = [...byRole.entries()].sort((a, b) => b[1].tokens - a[1].tokens);
  const columns: Column<CostRow>[] = [
    { key: 'role', label: 'Role', render: ([role]) => e(role) },
    { key: 'calls', label: 'Calls', align: 'right', render: ([, v]) => v.calls },
    { key: 'tokens', label: 'Tokens', align: 'right', render: ([, v]) => fmtCompact(v.tokens) },
    { key: 'cost', label: 'Cost', align: 'right', render: ([, v]) => fmtUsd(v.cost) },
  ];
  return (
    <Panel title="Cost / Tokens" count={rows.length}>
      {data.length === 0 ? (
        <EmptyState message="No realized spend recorded" inline />
      ) : (
        <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '8px', padding: '8px 10px' }}>
            <div style={{ padding: '8px', border: '1px solid #eee', borderRadius: '6px', background: '#fafafa' }}>
              <div style={{ color: '#888', fontSize: '11px' }}>Total tokens</div>
              <div style={{ color: '#333', fontSize: '18px', fontWeight: 600 }}>{fmtCompact(totalTokens)}</div>
            </div>
            <div style={{ padding: '8px', border: '1px solid #eee', borderRadius: '6px', background: '#fafafa' }}>
              <div style={{ color: '#888', fontSize: '11px' }}>Realized cost</div>
              <div style={{ color: '#333', fontSize: '18px', fontWeight: 600 }}>{fmtUsd(totalCost)}</div>
            </div>
          </div>
          <div style={{ flex: 1, overflow: 'auto' }}>
            <DataTable columns={columns} rows={rows} rowKey={([role]) => role} />
          </div>
        </div>
      )}
    </Panel>
  );
}

/* Active Sessions — live chat/agent sessions (title, working dir, last activity). */
const SESSION_COLUMNS: Column<Session>[] = [
  { key: 'title', label: 'Title', render: s => e(s.title || '(untitled)') },
  {
    key: 'cwd', label: 'Working dir', width: 140,
    render: s => (
      <span style={{ display: 'block', color: '#999', maxWidth: 140, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }} title={s.cwd}>
        {e(shortenPath(s.cwd))}
      </span>
    ),
  },
  {
    key: 'last_active', label: 'Last active',
    render: s => <span style={{ whiteSpace: 'nowrap', color: '#999' }}>{e((s.last_active || '').replace('T', ' ').slice(0, 16))}</span>,
  },
];

function SessionsPanel({ data }: { data: Session[] }) {
  return (
    <Panel title="Active Sessions" count={data.length}>
      {data.length === 0 ? (
        <EmptyState message="No active sessions" inline />
      ) : (
        <DataTable columns={SESSION_COLUMNS} rows={data} rowKey={(s, i) => s.id ?? i} />
      )}
    </Panel>
  );
}

function shortenPath(p: string): string {
  if (!p) return '';
  const parts = p.split('/').filter(Boolean);
  return parts.length <= 2 ? p : '…/' + parts.slice(-2).join('/');
}

function LspPanel({ data }: { data: LspStatus }) {
  const statusColor = data.errors > 0 ? '#ff6b6b' : data.warnings > 0 ? '#ffd93d' : '#4caf50';
  const statusLabel = data.errors > 0 ? 'Errors' : data.warnings > 0 ? 'Warnings' : 'Healthy';

  return (
    <Panel title="LSP Health" count={data.active_servers}>
      {data.active_servers === 0 ? (
        <div style={{ padding: '16px', color: '#888', fontSize: '12px' }}>
          No active LSP servers
        </div>
      ) : (
        <div style={{ padding: '12px', display: 'flex', flexDirection: 'column', gap: '8px' }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
            <StatusDot color={statusColor} />
            <span style={{ fontSize: '12px', color: statusColor, fontWeight: 600 }}>{statusLabel}</span>
          </div>
          <table style={{ fontSize: '12px', width: '100%', borderCollapse: 'collapse' }}>
            <tbody>
              <tr>
                <td style={{ color: '#888', padding: '2px 8px 2px 0' }}>Active servers</td>
                <td style={{ color: '#ccc', textAlign: 'right' }}>{data.active_servers}</td>
              </tr>
              <tr>
                <td style={{ color: '#888', padding: '2px 8px 2px 0' }}>Errors</td>
                <td style={{ color: data.errors > 0 ? '#ff6b6b' : '#ccc', textAlign: 'right', fontWeight: data.errors > 0 ? 600 : 400 }}>
                  {data.errors}
                </td>
              </tr>
              <tr>
                <td style={{ color: '#888', padding: '2px 8px 2px 0' }}>Warnings</td>
                <td style={{ color: data.warnings > 0 ? '#ffd93d' : '#ccc', textAlign: 'right', fontWeight: data.warnings > 0 ? 600 : 400 }}>
                  {data.warnings}
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      )}
    </Panel>
  );
}

function agentEnabledDot(enabled: boolean | undefined): string {
  return enabled ? '#22c55e' : '#9ca3af';
}

const AGENT_COLUMNS: Column<Agent>[] = [
  {
    key: 'enabled', label: '',
    render: a => (
      <span title={a.enabled ? 'enabled' : 'disabled'}>
        <StatusDot color={agentEnabledDot(a.enabled)} />
      </span>
    ),
  },
  { key: 'name', label: 'Name', render: a => e(a.name) },
  { key: 'provider', label: 'Provider', render: a => e(a.provider) || '—' },
  { key: 'model', label: 'Model', render: a => e(a.model) || '—' },
  {
    key: 'roles', label: 'Roles',
    render: a => (
      <span title={(a.roles ?? []).join(', ')}>
        {a.roles && a.roles.length > 0 ? e(a.roles.join(', ')) : '—'}
      </span>
    ),
  },
];

function AgentsPanel({ data }: { data: Agent[] }) {
  return (
    <Panel title="Agents" count={data.length}>
      {data.length === 0 ? (
        <div style={{ padding: '12px', color: '#aaa', fontSize: '12px' }}>
          No agents configured
        </div>
      ) : (
        <DataTable columns={AGENT_COLUMNS} rows={data} rowKey={(_a, i) => i} />
      )}
    </Panel>
  );
}

function OnboardPanel({ data }: { data: OnboardReport | null }) {
  if (!data) {
    return (
      <Panel title="Readiness">
        <div style={{ padding: '12px', fontSize: '12px', color: '#888' }}>
          Onboard report unavailable.
        </div>
      </Panel>
    );
  }
  const stepColor = (s: OnboardStep['status']): string => {
    switch (s) {
      case 'ok':
        return '#4caf50';
      case 'warn':
        return '#f39c12';
      case 'error':
        return '#ff6b6b';
      case 'skipped':
      default:
        return '#aaa';
    }
  };
  const readyColor = data.ready ? '#4caf50' : '#ff6b6b';
  return (
    <Panel title="Readiness" count={data.ready ? 1 : 0}>
      <div style={{ padding: '12px', display: 'flex', flexDirection: 'column', gap: '10px', fontSize: '12px' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
          <span style={{ color: '#888' }}>Ready:</span>
          <span style={{ color: readyColor, fontWeight: 600 }}>{data.ready ? 'yes' : 'no'}</span>
          <span style={{ color: '#aaa', marginLeft: 'auto' }}>
            {Math.round(data.elapsed_ms)}&thinsp;ms
          </span>
        </div>
        <table style={{ fontSize: '12px', width: '100%', borderCollapse: 'collapse' }}>
          <tbody>
            {data.steps.map((s, i) => (
              <tr key={`${s.step}-${i}`}>
                <td style={{ color: '#888', padding: '2px 8px 2px 0' }}>{e(s.step)}</td>
                <td style={{ color: stepColor(s.status), textAlign: 'right', fontWeight: 600 }}>
                  {e(s.status)}
                  {(s.warnings || s.errors) ? (
                    <span style={{ color: '#aaa', fontWeight: 400, marginLeft: '6px' }}>
                      (w={s.warnings ?? 0}, e={s.errors ?? 0})
                    </span>
                  ) : null}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        {data.next_actions.length > 0 && (
          <div style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
            <div style={{ color: '#888', fontSize: '11px' }}>Next actions</div>
            {data.next_actions.slice(0, 3).map((a, i) => (
              <div
                key={`action-${i}`}
                style={{
                  padding: '6px 8px',
                  borderRadius: '4px',
                  background: data.ready ? '#f1f7f1' : '#fff3f3',
                  border: `1px solid ${data.ready ? '#cfe2cf' : '#ffd2d2'}`,
                  color: '#444',
                }}
                title={a}
              >
                {e(a)}
              </div>
            ))}
          </div>
        )}
      </div>
    </Panel>
  );
}

/* ---- Derived-metric panels (all from server-incurred data) ---- */

/* A labeled horizontal bar row for compact distributions. */
function BarRow({ label, value, max, right, color }: {
  label: string; value: number; max: number; right: string; color: string;
}) {
  const pct = max > 0 ? Math.max(2, Math.round((value / max) * 100)) : 0;
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '3px 10px', fontSize: 12 }}>
      <span style={{ width: 92, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', color: '#555' }} title={label}>{label}</span>
      <div style={{ flex: 1, background: '#f0f0f0', borderRadius: 3, height: 10, overflow: 'hidden' }}>
        <div style={{ width: `${pct}%`, background: color, height: '100%' }} />
      </div>
      <span style={{ width: 62, textAlign: 'right', color: '#666', fontVariantNumeric: 'tabular-nums' }}>{right}</span>
    </div>
  );
}

function percentile(sorted: number[], p: number): number {
  if (sorted.length === 0) return 0;
  const idx = Math.min(sorted.length - 1, Math.floor((p / 100) * sorted.length));
  return sorted[idx];
}

/* Guardrail Actions — verdict mix over the server's tool-action audit. */
function GuardrailPanel({ data }: { data: AuditSummary }) {
  const counts: Record<string, number> = {
    allow: data.allow, block: data.block, rewrite: data.rewrite, approval_required: data.approval_required,
  };
  const max = Math.max(1, ...Object.values(counts));
  const colors: Record<string, string> = {
    allow: '#22c55e', block: '#ef4444', rewrite: '#f59e0b', approval_required: '#3b82f6',
  };
  return (
    <Panel title="Guardrail Actions" count={data.total}>
      {data.total === 0 ? <EmptyState message="No tool-action audit yet" inline /> : (
        <div style={{ padding: '8px 0' }}>
          {Object.keys(counts).map(v => (
            <BarRow key={v} label={v} value={counts[v]} max={max} right={String(counts[v])} color={colors[v] || '#888'} />
          ))}
        </div>
      )}
    </Panel>
  );
}

/* Success by Agent — success rate + volume per delegate agent. */
function SuccessByAgentPanel({ data }: { data: Delegation[] }) {
  const by = new Map<string, { ok: number; total: number }>();
  for (const d of data) {
    const c = by.get(d.agent) ?? { ok: 0, total: 0 };
    c.total++; if (d.success) c.ok++;
    by.set(d.agent, c);
  }
  const rows = [...by.entries()].sort((a, b) => b[1].total - a[1].total);
  const max = Math.max(1, ...rows.map(r => r[1].total));
  return (
    <Panel title="Success by Agent" count={rows.length}>
      {rows.length === 0 ? <EmptyState message="No delegations" inline /> : (
        <div style={{ padding: '8px 0' }}>
          {rows.map(([agent, c]) => {
            const rate = Math.round((c.ok / c.total) * 100);
            return <BarRow key={agent} label={agent} value={c.ok} max={max}
              right={`${rate}% · ${c.total}`} color={rate >= 80 ? '#22c55e' : rate >= 50 ? '#f59e0b' : '#ef4444'} />;
          })}
        </div>
      )}
    </Panel>
  );
}

/* Latency by Role — p50 / p95 / max per role (from delegation latencies). */
function LatencyByRolePanel({ data }: { data: Delegation[] }) {
  const by = new Map<string, number[]>();
  for (const d of data) {
    const a = by.get(d.role) ?? []; a.push(d.latency_ms); by.set(d.role, a);
  }
  const rows = [...by.entries()].map(([role, lats]) => {
    const s = [...lats].sort((a, b) => a - b);
    return { role, p50: percentile(s, 50), p95: percentile(s, 95), max: s[s.length - 1], n: s.length };
  }).sort((a, b) => b.p95 - a.p95);
  const columns: Column<typeof rows[number]>[] = [
    { key: 'role', label: 'Role', render: r => e(r.role) },
    { key: 'p50', label: 'p50', align: 'right', render: r => fmtDuration(r.p50) },
    { key: 'p95', label: 'p95', align: 'right', render: r => fmtDuration(r.p95) },
    { key: 'max', label: 'max', align: 'right', render: r => fmtDuration(r.max) },
  ];
  return (
    <Panel title="Latency by Role" count={rows.length}>
      {rows.length === 0 ? <EmptyState message="No delegations" inline /> : (
        <DataTable columns={columns} rows={rows} rowKey={r => r.role} />
      )}
    </Panel>
  );
}

/* Top Tools — most-invoked tools from the tool-call trace stream. */
function TopToolsPanel({ data }: { data: Trace[] }) {
  const by = new Map<string, number>();
  for (const t of data) { if (t.tool_name) by.set(t.tool_name, (by.get(t.tool_name) || 0) + 1); }
  const rows = [...by.entries()].sort((a, b) => b[1] - a[1]).slice(0, 12);
  const max = Math.max(1, ...rows.map(r => r[1]));
  return (
    <Panel title="Top Tools" count={rows.length}>
      {rows.length === 0 ? <EmptyState message="No tool calls traced" inline /> : (
        <div style={{ padding: '8px 0' }}>
          {rows.map(([tool, n]) => <BarRow key={tool} label={tool} value={n} max={max} right={String(n)} color="#6366f1" />)}
        </div>
      )}
    </Panel>
  );
}

/* Cost by Agent — realized token spend rolled up per agent (token_audit.tool_name). */
function CostByAgentPanel({ data }: { data: TokenAudit[] }) {
  const by = new Map<string, number>();
  for (const r of data) {
    const k = r.tool_name || r.role || '—';
    by.set(k, (by.get(k) || 0) + (r.prompt_tokens || 0) + (r.completion_tokens || 0));
  }
  const rows = [...by.entries()].sort((a, b) => b[1] - a[1]).slice(0, 12);
  const max = Math.max(1, ...rows.map(r => r[1]));
  return (
    <Panel title="Tokens by Agent" count={rows.length}>
      {rows.length === 0 ? <EmptyState message="No spend recorded" inline /> : (
        <div style={{ padding: '8px 0' }}>
          {rows.map(([agent, tok]) => <BarRow key={agent} label={agent} value={tok} max={max} right={fmtCompact(tok)} color="#0ea5e9" />)}
        </div>
      )}
    </Panel>
  );
}

/* Cache Efficiency — cache-read share of input tokens (higher = cheaper reuse). */
function CachePanel({ data }: { data: TokenAudit[] }) {
  const prompt = data.reduce((n, r) => n + (r.prompt_tokens || 0), 0);
  const cacheRead = data.reduce((n, r) => n + (r.cache_read_tokens || 0), 0);
  const cacheWrite = data.reduce((n, r) => n + (r.cache_write_tokens || 0), 0);
  const denom = prompt + cacheRead;
  const pct = denom > 0 ? Math.round((cacheRead / denom) * 100) : 0;
  return (
    <Panel title="Cache Efficiency">
      <div style={{ padding: 14, display: 'flex', flexDirection: 'column', gap: 10, fontSize: 12 }}>
        <div style={{ display: 'flex', alignItems: 'baseline', gap: 8 }}>
          <span style={{ fontSize: 30, fontWeight: 700, color: pct >= 50 ? '#22c55e' : pct >= 20 ? '#f59e0b' : '#888' }}>{pct}%</span>
          <span style={{ color: '#888' }}>cache-read share of input</span>
        </div>
        <div style={{ background: '#f0f0f0', borderRadius: 4, height: 12, overflow: 'hidden' }}>
          <div style={{ width: `${pct}%`, background: '#22c55e', height: '100%' }} />
        </div>
        <table style={{ fontSize: 12, width: '100%' }}><tbody>
          <tr><td style={{ color: '#888' }}>Cache reads</td><td style={{ textAlign: 'right' }}>{fmtCompact(cacheRead)}</td></tr>
          <tr><td style={{ color: '#888' }}>Cache writes</td><td style={{ textAlign: 'right' }}>{fmtCompact(cacheWrite)}</td></tr>
          <tr><td style={{ color: '#888' }}>Fresh prompt</td><td style={{ textAlign: 'right' }}>{fmtCompact(prompt)}</td></tr>
        </tbody></table>
      </div>
    </Panel>
  );
}

/* Failures — recent unsuccessful delegations. */
const FAILURE_COLUMNS: Column<Delegation>[] = [
  { key: 'agent', label: 'Agent', render: d => e(d.agent) },
  { key: 'role', label: 'Role', render: d => e(d.role) },
  { key: 'turns', label: 'Turns', align: 'right', render: d => d.turns },
  { key: 'latency', label: 'Latency', align: 'right', render: d => fmtDuration(d.latency_ms) },
];

function FailuresPanel({ data }: { data: Delegation[] }) {
  const fails = data.filter(d => !d.success);
  return (
    <Panel title="Failures" count={fails.length}>
      {fails.length === 0 ? <EmptyState message="No failed delegations 🎉" inline /> : (
        <DataTable columns={FAILURE_COLUMNS} rows={fails.slice(0, 20)} rowKey={(_d, i) => i} />
      )}
    </Panel>
  );
}

/* Provider Mix — configured agents grouped by provider. */
function ProviderMixPanel({ data }: { data: Agent[] }) {
  const by = new Map<string, number>();
  for (const a of data) by.set(a.provider || '—', (by.get(a.provider || '—') || 0) + 1);
  const rows = [...by.entries()].sort((a, b) => b[1] - a[1]);
  const max = Math.max(1, ...rows.map(r => r[1]));
  return (
    <Panel title="Provider Mix" count={rows.length}>
      {rows.length === 0 ? <EmptyState message="No agents" inline /> : (
        <div style={{ padding: '8px 0' }}>
          {rows.map(([p, n]) => <BarRow key={p} label={p} value={n} max={max} right={String(n)} color="#8b5cf6" />)}
        </div>
      )}
    </Panel>
  );
}

/* Confidence by Role — average delegate self-reported confidence per role. */
function ConfidenceByRolePanel({ data }: { data: Delegation[] }) {
  const by = new Map<string, { sum: number; n: number }>();
  for (const d of data) {
    if (typeof d.confidence !== 'number') continue;
    const c = by.get(d.role) ?? { sum: 0, n: 0 };
    c.sum += d.confidence; c.n++;
    by.set(d.role, c);
  }
  const rows = [...by.entries()].map(([role, c]) => ({ role, avg: Math.round(c.sum / c.n), n: c.n }))
    .sort((a, b) => b.avg - a.avg);
  return (
    <Panel title="Confidence" count={rows.length}>
      {rows.length === 0 ? <EmptyState message="No confidence data" inline /> : (
        <div style={{ padding: '8px 0' }}>
          {rows.map(r => (
            <BarRow key={r.role} label={r.role} value={r.avg} max={100}
              right={`${r.avg}% · ${r.n}`} color={r.avg >= 70 ? '#22c55e' : r.avg >= 40 ? '#f59e0b' : '#ef4444'} />
          ))}
        </div>
      )}
    </Panel>
  );
}

/* ---- Main Dashboard component ---- */

/* Panel registry: id -> {title, render, defaultOn}. Users pick which panels show
 * (persisted per-browser in localStorage), so the dashboard is customizable. */
interface PanelDef {
  id: string;
  title: string;
  defaultOn: boolean;
  render: (d: DashData) => React.ReactNode;
}

const PANELS: PanelDef[] = [
  { id: 'readiness',   title: 'Readiness',        defaultOn: true,  render: d => <OnboardPanel data={d.onboard} /> },
  { id: 'agents',      title: 'Agents',           defaultOn: true,  render: d => <AgentsPanel data={d.agents} /> },
  { id: 'sessions',    title: 'Active Sessions',  defaultOn: true,  render: d => <SessionsPanel data={d.sessions} /> },
  { id: 'delegations', title: 'Delegations',      defaultOn: true,  render: d => <DelegationsPanel data={d.delegations} /> },
  { id: 'metrics',     title: 'Metrics',          defaultOn: true,  render: d => <MetricsPanel data={d.metrics} /> },
  { id: 'cost',        title: 'Cost / Tokens',    defaultOn: true,  render: d => <CostPanel data={d.tokenAudit} /> },
  { id: 'guardrail',   title: 'Guardrail Actions',defaultOn: true,  render: d => <GuardrailPanel data={d.auditSummary} /> },
  { id: 'plans',       title: 'Execution Plans',  defaultOn: true,  render: d => <PlansPanel data={d.plans} /> },
  { id: 'traces',      title: 'Traces',           defaultOn: true,  render: d => <TracesPanel data={d.traces} /> },
  // Available to add:
  { id: 'success',     title: 'Success by Agent', defaultOn: false, render: d => <SuccessByAgentPanel data={d.delegations} /> },
  { id: 'latency',     title: 'Latency by Role',  defaultOn: false, render: d => <LatencyByRolePanel data={d.delegations} /> },
  { id: 'toptools',    title: 'Top Tools',        defaultOn: false, render: d => <TopToolsPanel data={d.traces} /> },
  { id: 'tokagent',    title: 'Tokens by Agent',  defaultOn: false, render: d => <CostByAgentPanel data={d.tokenAudit} /> },
  { id: 'cache',       title: 'Cache Efficiency', defaultOn: false, render: d => <CachePanel data={d.tokenAudit} /> },
  { id: 'failures',    title: 'Failures',         defaultOn: false, render: d => <FailuresPanel data={d.delegations} /> },
  { id: 'provider',    title: 'Provider Mix',     defaultOn: false, render: d => <ProviderMixPanel data={d.agents} /> },
  { id: 'confidence',  title: 'Confidence',       defaultOn: false, render: d => <ConfidenceByRolePanel data={d.delegations} /> },
  { id: 'memory',      title: 'Memory',           defaultOn: false, render: d => <MemoryPanel data={d.memory} /> },
  { id: 'lsp',         title: 'LSP Health',       defaultOn: false, render: d => <LspPanel data={d.lsp ?? { errors: 0, warnings: 0, active_servers: 0 }} /> },
];

const LAYOUT_KEY = 'aimee_dash_layout_v1';

function defaultLayout(): string[] {
  return PANELS.filter(p => p.defaultOn).map(p => p.id);
}

function loadLayout(): string[] {
  try {
    const raw = localStorage.getItem(LAYOUT_KEY);
    if (!raw) return defaultLayout();
    const ids = JSON.parse(raw) as string[];
    const valid = ids.filter(id => PANELS.some(p => p.id === id));
    return valid.length ? valid : defaultLayout();
  } catch {
    return defaultLayout();
  }
}

/* Customize popover: toggle panels on/off and reorder the enabled ones. */
function CustomizePopover({ layout, setLayout, onClose }: {
  layout: string[]; setLayout: (l: string[]) => void; onClose: () => void;
}) {
  const enabled = new Set(layout);
  const toggle = (id: string) => {
    setLayout(enabled.has(id) ? layout.filter(x => x !== id) : [...layout, id]);
  };
  const move = (id: string, dir: -1 | 1) => {
    const i = layout.indexOf(id);
    const j = i + dir;
    if (i < 0 || j < 0 || j >= layout.length) return;
    const next = [...layout];
    [next[i], next[j]] = [next[j], next[i]];
    setLayout(next);
  };
  // Enabled first (in order), then the rest.
  const ordered = [...layout, ...PANELS.map(p => p.id).filter(id => !enabled.has(id))];
  return (
    <>
      <div onClick={onClose} style={{ position: 'fixed', inset: 0, zIndex: 20 }} />
      <div style={{
        position: 'absolute', top: 40, right: 12, zIndex: 21, width: 280, maxHeight: '70vh', overflow: 'auto',
        background: '#fff', border: '1px solid #ddd', borderRadius: 8, boxShadow: '0 8px 24px rgba(0,0,0,0.15)', padding: 8,
      }}>
        <div style={{ fontSize: 12, fontWeight: 600, color: '#555', padding: '4px 6px 8px' }}>Customize panels</div>
        {ordered.map(id => {
          const p = PANELS.find(x => x.id === id)!;
          const on = enabled.has(id);
          return (
            <div key={id} style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '3px 6px', borderRadius: 4 }}
              title="Show or hide this panel on the dashboard.">
              <input type="checkbox" checked={on} onChange={() => toggle(id)} />
              <span style={{ flex: 1, fontSize: 13, color: on ? '#333' : '#999' }}>{p.title}</span>
              {on && (
                <>
                  <Button size="sm" onClick={() => move(id, -1)} title="Move up" style={{ padding: '0 6px' }}>↑</Button>
                  <Button size="sm" onClick={() => move(id, 1)} title="Move down" style={{ padding: '0 6px' }}>↓</Button>
                </>
              )}
            </div>
          );
        })}
        <div style={{ display: 'flex', justifyContent: 'space-between', padding: '8px 6px 2px' }}>
          <Button size="sm" onClick={() => setLayout(defaultLayout())} title="Restore the default set and order of panels.">Reset</Button>
          <Button size="sm" onClick={onClose}>Done</Button>
        </div>
      </div>
    </>
  );
}

export default function Dashboard() {
  const [data, setData] = useState<DashData | null>(null);
  const [loading, setLoading] = useState(true);
  const [layout, setLayoutState] = useState<string[]>(loadLayout);
  const [customizing, setCustomizing] = useState(false);

  const setLayout = useCallback((l: string[]) => {
    setLayoutState(l);
    try { localStorage.setItem(LAYOUT_KEY, JSON.stringify(l)); } catch { /* ignore */ }
  }, []);

  const load = useCallback(async () => {
    try {
      const all = await fetch('/api/dashboard').then(r => r.json()) as RawDashboard;
      setData(toDashData(all));
    } catch (err) {
      console.error('Dashboard load failed', err);
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    if (document.visibilityState !== 'hidden') load();
    const onVisible = () => { if (document.visibilityState === 'visible') load(); };
    document.addEventListener('visibilitychange', onVisible);
    return () => document.removeEventListener('visibilitychange', onVisible);
  }, [load]);

  const shown = layout.map(id => PANELS.find(p => p.id === id)).filter(Boolean) as PanelDef[];

  return (
    // Fill the routed <main> EXACTLY (no negative-margin hack) so nothing overflows.
    <div style={{
      position: 'relative', height: '100%', width: '100%', boxSizing: 'border-box',
      display: 'flex', flexDirection: 'column', overflow: 'hidden', background: '#f0f0f0',
    }}>
      <div style={{
        padding: '8px 16px', background: '#fafafa', borderBottom: '1px solid #e0e0e0',
        display: 'flex', alignItems: 'center', gap: 12, flexShrink: 0,
      }}>
        <span style={{ fontSize: 14, fontWeight: 600, color: '#555' }}>Dashboard</span>
        <Button size="sm" onClick={load} title="Reload all dashboard data from the server.">Refresh</Button>
        <Button size="sm" onClick={() => setCustomizing(v => !v)} style={{ marginLeft: 'auto' }} title="Choose which panels are shown and reorder them.">⚙ Customize</Button>
        {loading && <span style={{ fontSize: 12, color: '#aaa' }}>Loading…</span>}
      </div>

      {customizing && <CustomizePopover layout={layout} setLayout={setLayout} onClose={() => setCustomizing(false)} />}

      {/* Grid: 3 columns that never overflow horizontally (minmax(0,1fr)); rows are
          at least 200px and grow to fill. Few panels fill the viewport with no
          scroll; many panels scroll vertically only. */}
      {data && (
        shown.length === 0 ? (
          <div style={{ padding: 24, color: '#888', fontSize: 13 }}>
            No panels selected — click <b>⚙ Customize</b> to add some.
          </div>
        ) : (
          <div style={{
            flex: 1, minHeight: 0, boxSizing: 'border-box',
            display: 'grid', gridTemplateColumns: 'repeat(3, minmax(0, 1fr))',
            gridAutoRows: 'minmax(200px, 1fr)', gap: 8, padding: 8,
            overflowY: 'auto', overflowX: 'hidden',
          }}>
            {shown.map(p => <div key={p.id} style={{ minWidth: 0, minHeight: 0, overflow: 'hidden' }}>{p.render(data)}</div>)}
          </div>
        )
      )}
    </div>
  );
}
