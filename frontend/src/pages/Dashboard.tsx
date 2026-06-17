import { useEffect, useState, useCallback } from 'react';
import { Badge, Panel } from '@rakuensoftware/smoothgui';
import type { BadgeVariant } from '@rakuensoftware/smoothgui';

/* ---- API types ---- */

interface Delegation {
  agent: string;
  role: string;
  success: boolean;
  turns: number;
  tool_calls: number;
  latency_ms: number;
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

interface PluginFailure {
  timestamp: string;
  phase: string;
  hook: string;
  rc: number;
  detail: string;
}

interface PluginMetrics {
  installed: number;
  enabled: number;
  hooks_total: number;
  tools_total: number;
  hook_runs_24h: number;
  hook_failures_24h: number;
  recent_failures: PluginFailure[];
}

interface Agent {
  name: string;
  family: string;
  slot: number;
  state: 'pending' | 'active' | 'idle' | 'offline';
  role: string;
  registered_at: number;
  last_heartbeat: number;
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

interface DashData {
  delegations: Delegation[];
  metrics: Metric[];
  traces: Trace[];
  memory: MemoryStat[];
  plans: Plan[];
  logs: LogEntry[];
  plugins: PluginMetrics;
  lsp: LspStatus | null;
  agents: Agent[];
  onboard: OnboardReport | null;
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

/* ---- Shared table styles ---- */

const tableStyle: React.CSSProperties = {
  width: '100%', borderCollapse: 'collapse', fontSize: '12px',
};

const thStyle: React.CSSProperties = {
  textAlign: 'left', padding: '6px 10px', borderBottom: '1px solid #eee',
  color: '#888', fontSize: '11px', textTransform: 'uppercase',
  letterSpacing: '0.3px', position: 'sticky', top: 0, background: '#fafafa',
};

const tdStyle: React.CSSProperties = {
  padding: '5px 10px', borderBottom: '1px solid #f5f5f5',
};

const numTd: React.CSSProperties = { ...tdStyle, textAlign: 'right', fontVariantNumeric: 'tabular-nums' };

const filterButtonStyle: React.CSSProperties = {
  padding: '2px 8px',
  borderRadius: '999px',
  border: '1px solid #ddd',
  background: '#fff',
  color: '#666',
  cursor: 'pointer',
  fontSize: '11px',
};

/* ---- Panels ---- */

function DelegationsPanel({ data }: { data: Delegation[] }) {
  return (
    <Panel title="Delegations" count={data.length}>
      <table style={tableStyle}>
        <thead>
          <tr>
            {['Agent', 'Role', 'Status', 'Turns', 'Tools', 'Latency'].map(h => (
              <th key={h} style={thStyle}>{h}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {data.map((r, i) => (
            <tr key={i}>
              <td style={tdStyle}>{e(r.agent)}</td>
              <td style={tdStyle}>{e(r.role)}</td>
              <td style={tdStyle}>
                <Badge label={r.success ? 'OK' : 'ERR'} variant={r.success ? 'success' : 'error'} />
              </td>
              <td style={numTd}>{r.turns}</td>
              <td style={numTd}>{r.tool_calls}</td>
              <td style={numTd}>{r.latency_ms}ms</td>
            </tr>
          ))}
        </tbody>
      </table>
    </Panel>
  );
}

function MetricsPanel({ data }: { data: Metric[] }) {
  return (
    <Panel title="Metrics">
      <table style={tableStyle}>
        <thead>
          <tr>
            {['Role', 'Total', 'OK', 'Avg Lat', 'Tokens'].map(h => (
              <th key={h} style={thStyle}>{h}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {data.map((r, i) => (
            <tr key={i}>
              <td style={tdStyle}>{e(r.role)}</td>
              <td style={numTd}>{r.total}</td>
              <td style={numTd}>{r.successes}</td>
              <td style={numTd}>{r.avg_latency_ms}ms</td>
              <td style={numTd}>{r.tokens}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </Panel>
  );
}

function PlansPanel({ data }: { data: Plan[] }) {
  return (
    <Panel title="Execution Plans">
      <table style={tableStyle}>
        <thead>
          <tr>
            {['ID', 'Agent', 'Status', 'Steps', 'Task'].map(h => (
              <th key={h} style={thStyle}>{h}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {data.map((r, i) => (
            <tr key={i}>
              <td style={numTd}>{r.id}</td>
              <td style={tdStyle}>{e(r.agent)}</td>
              <td style={tdStyle}><Badge label={r.status} variant={planVariant(r.status)} /></td>
              <td style={numTd}>{r.done_steps}/{r.total_steps}</td>
              <td style={tdStyle}>{e(r.task.substring(0, 60))}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </Panel>
  );
}

function TracesPanel({ data }: { data: Trace[] }) {
  return (
    <Panel title="Traces">
      <table style={tableStyle}>
        <thead>
          <tr>
            {['Turn', 'Tool', 'Dir'].map(h => <th key={h} style={thStyle}>{h}</th>)}
          </tr>
        </thead>
        <tbody>
          {data.map((r, i) => (
            <tr key={i}>
              <td style={numTd}>{r.turn}</td>
              <td style={tdStyle}>{e(r.tool_name ?? '--')}</td>
              <td style={tdStyle}>{e(r.direction)}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </Panel>
  );
}

function MemoryPanel({ data }: { data: MemoryStat[] }) {
  return (
    <Panel title="Memory">
      <table style={tableStyle}>
        <thead>
          <tr>
            {['Tier', 'Kind', 'Count'].map(h => <th key={h} style={thStyle}>{h}</th>)}
          </tr>
        </thead>
        <tbody>
          {data.map((r, i) => (
            <tr key={i}>
              <td style={tdStyle}>{e(r.tier)}</td>
              <td style={tdStyle}>{e(r.kind)}</td>
              <td style={numTd}>{r.count}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </Panel>
  );
}

function PluginsPanel({
  data,
  onViewFailures,
}: {
  data: PluginMetrics;
  onViewFailures: () => void;
}) {
  const failureColor = data.hook_failures_24h > 0 ? '#ff6b6b' : '#4caf50';

  return (
    <Panel title="Plugins" count={data.installed}>
      <div style={{ padding: '12px', display: 'flex', flexDirection: 'column', gap: '10px', fontSize: '12px' }}>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '8px' }}>
          <div style={{ padding: '8px', border: '1px solid #eee', borderRadius: '6px', background: '#fafafa' }}>
            <div style={{ color: '#888', fontSize: '11px' }}>Installed</div>
            <div style={{ color: '#333', fontSize: '18px', fontWeight: 600 }}>{data.installed}</div>
          </div>
          <div style={{ padding: '8px', border: '1px solid #eee', borderRadius: '6px', background: '#fafafa' }}>
            <div style={{ color: '#888', fontSize: '11px' }}>Enabled</div>
            <div style={{ color: '#333', fontSize: '18px', fontWeight: 600 }}>{data.enabled}</div>
          </div>
        </div>
        <table style={{ fontSize: '12px', width: '100%', borderCollapse: 'collapse' }}>
          <tbody>
            <tr>
              <td style={{ color: '#888', padding: '2px 8px 2px 0' }}>Hooks registered</td>
              <td style={{ color: '#ccc', textAlign: 'right' }}>{data.hooks_total}</td>
            </tr>
            <tr>
              <td style={{ color: '#888', padding: '2px 8px 2px 0' }}>Tools registered</td>
              <td style={{ color: '#ccc', textAlign: 'right' }}>{data.tools_total}</td>
            </tr>
            <tr>
              <td style={{ color: '#888', padding: '2px 8px 2px 0' }}>Last 24h hook runs</td>
              <td style={{ color: '#ccc', textAlign: 'right' }}>{data.hook_runs_24h}</td>
            </tr>
            <tr>
              <td style={{ color: '#888', padding: '2px 8px 2px 0' }}>Last 24h failures</td>
              <td style={{ color: failureColor, textAlign: 'right', fontWeight: 600 }}>{data.hook_failures_24h}</td>
            </tr>
          </tbody>
        </table>
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: '8px' }}>
          <span style={{ color: failureColor, fontWeight: 600 }}>
            {data.hook_failures_24h > 0 ? 'Hook failures need review' : 'Hooks look clean'}
          </span>
          <button
            onClick={onViewFailures}
            disabled={data.hook_failures_24h === 0}
            style={{
              ...filterButtonStyle,
              opacity: data.hook_failures_24h === 0 ? 0.5 : 1,
              cursor: data.hook_failures_24h === 0 ? 'not-allowed' : 'pointer',
            }}
          >
            View failures
          </button>
        </div>
        {data.recent_failures.length > 0 && (
          <div style={{ display: 'flex', flexDirection: 'column', gap: '6px' }}>
            {data.recent_failures.slice(0, 3).map((failure, index) => (
              <div
                key={`${failure.timestamp}-${index}`}
                style={{ padding: '8px', borderRadius: '6px', background: '#fff3f3', border: '1px solid #ffd2d2' }}
              >
                <div style={{ color: '#c62828', fontWeight: 600 }}>
                  {failure.phase || 'Hook'} rc={failure.rc}
                </div>
                <div
                  style={{ color: '#666', fontSize: '11px', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}
                  title={failure.hook}
                >
                  {e(failure.hook)}
                </div>
                <div style={{ color: '#888', fontSize: '11px' }}>{e(failure.timestamp)}</div>
              </div>
            ))}
          </div>
        )}
      </div>
    </Panel>
  );
}

function LogsPanel({
  data,
  filter,
  onFilterChange,
}: {
  data: LogEntry[];
  filter: string;
  onFilterChange: (value: string) => void;
}) {
  const filtered = filter === 'all'
    ? data
    : data.filter(row => row.source === filter || row.who === filter || (row as LogEntry & { tag?: string }).tag === filter);

  return (
    <Panel title="Logs" count={filtered.length}>
      <div style={{ padding: '8px 10px', display: 'flex', gap: '6px', borderBottom: '1px solid #f0f0f0' }}>
        <button onClick={() => onFilterChange('all')} style={filterButtonStyle}>All</button>
        <button onClick={() => onFilterChange('audit')} style={filterButtonStyle}>Audit</button>
        <button onClick={() => onFilterChange('plugin-hook')} style={filterButtonStyle}>Plugin hooks</button>
      </div>
      <table style={tableStyle}>
        <thead>
          <tr>
            {['Time', 'Source', 'Who', 'What', 'Detail'].map(h => (
              <th key={h} style={thStyle}>{h}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {filtered.map((r, i) => (
            <tr key={i}>
              <td style={tdStyle}>{e(r.timestamp)}</td>
              <td style={{ ...tdStyle, color: r.source === 'agent' ? '#4fc3f7' : '#ff9800' }}>
                {e(r.source)}
              </td>
              <td style={tdStyle}>{e(r.who)}</td>
              <td style={tdStyle}>{e(r.what)}</td>
              <td
                style={{ ...tdStyle, color: '#888', maxWidth: '200px', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}
                title={String(r.detail)}
              >
                {e(r.detail)}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </Panel>
  );
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
            <span style={{
              width: 8, height: 8, borderRadius: '50%', background: statusColor,
              display: 'inline-block', flexShrink: 0,
            }} />
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

function agentStateDot(state: Agent['state']): string {
  if (state === 'active')  return '#22c55e';
  if (state === 'idle')    return '#f59e0b';
  if (state === 'pending') return '#3b82f6';
  return '#9ca3af'; /* offline */
}

function AgentsPanel({ data }: { data: Agent[] }) {
  return (
    <Panel title="Agent Presence" count={data.length}>
      {data.length === 0 ? (
        <div style={{ padding: '12px', color: '#aaa', fontSize: '12px' }}>
          No agents registered
        </div>
      ) : (
        <table style={tableStyle}>
          <thead>
            <tr>
              {['', 'Name', 'Family', 'State', 'Role', 'Since'].map(h => (
                <th key={h} style={thStyle}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {data.map((a, i) => (
              <tr key={i}>
                <td style={tdStyle}>
                  <span style={{
                    display: 'inline-block', width: 8, height: 8,
                    borderRadius: '50%', background: agentStateDot(a.state),
                  }} />
                </td>
                <td style={tdStyle}>{e(a.name)}</td>
                <td style={tdStyle}>{e(a.family)}</td>
                <td style={tdStyle}>{e(a.state)}</td>
                <td style={tdStyle}>{e(a.role) || '—'}</td>
                <td style={numTd}>
                  {new Date(a.registered_at * 1000).toLocaleTimeString()}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
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

/* ---- Main Dashboard component ---- */

export default function Dashboard() {
  const [data, setData] = useState<DashData | null>(null);
  const [loading, setLoading] = useState(true);
  const [logFilter, setLogFilter] = useState('all');

  const load = useCallback(async () => {
    try {
      const all = await fetch('/api/dashboard').then(r => r.json()) as {
        delegations?: Delegation[];
        metrics?: Metric[];
        traces?: Trace[];
        memory_stats?: MemoryStat[];
        plans?: Plan[];
        logs?: LogEntry[];
        plugins?: PluginMetrics;
        lsp?: LspStatus;
        agents?: Agent[];
        onboard?: OnboardReport;
      };
      // The server-hosted webchat returns shapes the panels can't all consume:
      // `onboard` may be an `{error}` stub (truthy, no `steps`) and
      // `memory_stats` may be an object rather than a flat array. A plain `??`
      // only guards null/undefined, so a wrong-typed-but-truthy value slips
      // through and a panel's `.map`/`.steps.map` throws. Coerce every field to
      // the shape its panel expects so a bad payload renders empty, never blank.
      const arr = <T,>(v: unknown): T[] => (Array.isArray(v) ? (v as T[]) : []);
      const plugins = all.plugins;
      setData({
        delegations: arr<Delegation>(all.delegations),
        metrics: arr<Metric>(all.metrics),
        traces: arr<Trace>(all.traces),
        memory: arr<MemoryStat>(all.memory_stats),
        plans: arr<Plan>(all.plans),
        logs: arr<LogEntry>(all.logs),
        plugins: {
          installed: plugins?.installed ?? 0,
          enabled: plugins?.enabled ?? 0,
          hooks_total: plugins?.hooks_total ?? 0,
          tools_total: plugins?.tools_total ?? 0,
          hook_runs_24h: plugins?.hook_runs_24h ?? 0,
          hook_failures_24h: plugins?.hook_failures_24h ?? 0,
          recent_failures: arr<PluginFailure>(plugins?.recent_failures),
        },
        lsp: all.lsp ?? null,
        agents: arr<Agent>(all.agents),
        // Only accept a real onboard report (one that has a `steps` array);
        // the server's `{error: …}` stub must fall back to null.
        onboard: Array.isArray(all.onboard?.steps) ? (all.onboard as OnboardReport) : null,
      });
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

  return (
    <div style={{ margin: '-24px', height: 'calc(100% + 48px)', display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
      {/* Header row */}
      <div style={{
        padding: '8px 16px', background: '#fafafa', borderBottom: '1px solid #e0e0e0',
        display: 'flex', alignItems: 'center', gap: '12px', flexShrink: 0,
      }}>
        <span style={{ fontSize: '14px', fontWeight: 600, color: '#555' }}>Dashboard</span>
        <button
          onClick={load}
          style={{
            padding: '4px 12px', background: '#fff', color: '#666',
            border: '1px solid #ddd', borderRadius: '4px', cursor: 'pointer', fontSize: '12px',
          }}
        >
          Refresh
        </button>
        {loading && <span style={{ fontSize: '12px', color: '#aaa' }}>Loading…</span>}
      </div>

      {/* 3-column grid */}
      {data && (
        <div style={{
          flex: 1, display: 'grid',
          gridTemplateColumns: '1fr 1fr 1fr',
          gridAutoRows: '1fr',
          gap: '8px', padding: '8px', overflow: 'hidden', background: '#f0f0f0',
        }}>
          <OnboardPanel data={data.onboard} />
          <DelegationsPanel data={data.delegations} />
          <MetricsPanel data={data.metrics} />
          <PlansPanel data={data.plans} />
          <TracesPanel data={data.traces} />
          <MemoryPanel data={data.memory} />
          <PluginsPanel data={data.plugins} onViewFailures={() => setLogFilter('plugin-hook')} />
          <LogsPanel data={data.logs} filter={logFilter} onFilterChange={setLogFilter} />
          {data.lsp && <LspPanel data={data.lsp} />}
          <AgentsPanel data={data.agents} />
        </div>
      )}
    </div>
  );
}
