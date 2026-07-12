import { useCallback, useEffect, useRef, useState } from 'react';

/* Setup-wizard summary panel — server-orchestrated deploy.
 *
 * When aimee-server runs with the Docker socket mounted (AIMEE_DEPLOY_ENABLED), it
 * can bring up the managed sibling services (postgres + aimee-kb + aimee-llm) from
 * the wizard config via `docker compose up -d`. This panel drives that from the
 * finish screen: it checks whether orchestration is available (GET
 * /api/deploy/status → 503 when disabled), shows the current service states, and
 * offers a "Deploy" button (POST /api/deploy/apply) that runs on a server
 * background thread while the panel polls status until it settles.
 *
 * Only meaningful for a LOCAL knowledge base — a remote KB deploys nothing, so the
 * wizard renders nothing here. When orchestration is disabled the panel falls back
 * to the copy-paste compose command, so the operator is never left without a path. */

function csrf(): string {
  try {
    if (typeof window !== 'undefined') return (window as { _csrf?: string })._csrf || '';
  } catch {
    /* ignore */
  }
  return '';
}

async function api(path: string, init?: RequestInit): Promise<Response> {
  return fetch(path, {
    ...init,
    headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrf(), ...(init?.headers || {}) },
  });
}

interface DeployStatus {
  enabled: boolean;
  running: boolean;
  last_exit: number | null;
  output: string;
  ps: string;
}

export interface Svc {
  name: string;
  state: string;
}

/* docker compose ps --format json emits either a JSON array or newline-delimited
 * objects, depending on the compose version. Parse defensively; return [] when the
 * shape is unrecognized (the raw output stays available in the log). */
export function parsePs(ps: string): Svc[] {
  const pick = (o: Record<string, unknown>): Svc => ({
    name: String(o.Name ?? o.Service ?? ''),
    state: String(o.State ?? o.Status ?? ''),
  });
  const trimmed = ps.trim();
  if (!trimmed) return [];
  try {
    const j = JSON.parse(trimmed);
    if (Array.isArray(j)) return j.map(pick).filter((s) => s.name);
  } catch {
    /* fall through to NDJSON */
  }
  const out: Svc[] = [];
  for (const line of trimmed.split('\n')) {
    const l = line.trim();
    if (!l) continue;
    try {
      out.push(pick(JSON.parse(l)));
    } catch {
      /* skip unparseable line */
    }
  }
  return out.filter((s) => s.name);
}

export default function DeployPanel({ kbMode }: { kbMode: 'local' | 'remote' }) {
  const [loading, setLoading] = useState(true);
  const [status, setStatus] = useState<DeployStatus | null>(null);
  const [applying, setApplying] = useState(false);
  const [err, setErr] = useState('');
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);

  const loadStatus = useCallback(async () => {
    try {
      const r = await api('/api/deploy/status', { method: 'GET' });
      if (r.status === 503) {
        setStatus({ enabled: false, running: false, last_exit: null, output: '', ps: '' });
        return false;
      }
      const d = await r.json();
      if (r.ok) {
        const s: DeployStatus = {
          enabled: !!d.enabled,
          running: !!d.running,
          last_exit: d.last_exit ?? null,
          output: String(d.output ?? ''),
          ps: String(d.ps ?? ''),
        };
        setStatus(s);
        return s.running;
      }
    } catch {
      setStatus({ enabled: false, running: false, last_exit: null, output: '', ps: '' });
    }
    return false;
  }, []);

  // Poll while a deploy is running; stop once it settles.
  const poll = useCallback(async () => {
    const stillRunning = await loadStatus();
    if (stillRunning) {
      timer.current = setTimeout(poll, 3000);
    } else {
      setApplying(false);
    }
  }, [loadStatus]);

  useEffect(() => {
    if (kbMode !== 'local') {
      setLoading(false);
      return;
    }
    (async () => {
      const running = await loadStatus();
      setLoading(false);
      if (running) {
        setApplying(true);
        timer.current = setTimeout(poll, 3000);
      }
    })();
    return () => {
      if (timer.current) clearTimeout(timer.current);
    };
  }, [kbMode, loadStatus, poll]);

  async function deploy() {
    setErr('');
    setApplying(true);
    try {
      const r = await api('/api/deploy/apply', { method: 'POST' });
      const d = await r.json().catch(() => ({}));
      if (!r.ok) {
        setErr(d.error || 'could not start the deploy');
        setApplying(false);
        return;
      }
      await loadStatus();
      timer.current = setTimeout(poll, 3000);
    } catch {
      setErr('aimee-server unavailable');
      setApplying(false);
    }
  }

  if (kbMode !== 'local' || loading || !status) return null;

  // Orchestration off: hand the operator the one compose command instead.
  if (!status.enabled) {
    return (
      <div style={box}>
        <div style={{ fontWeight: 700, fontSize: 13, marginBottom: 4 }}>Bring up the stack</div>
        <div style={{ fontSize: 12, color: '#556', lineHeight: 1.5 }}>
          This server can’t launch containers itself. Start the knowledge base + LLM alongside it with:
          <pre style={pre}>docker compose -f compose.server.yaml up -d</pre>
        </div>
      </div>
    );
  }

  const svcs = parsePs(status.ps);
  const settledOk = !status.running && status.last_exit === 0;
  const settledErr = !status.running && status.last_exit !== null && status.last_exit !== 0;

  return (
    <div style={box}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 6 }}>
        <div style={{ fontWeight: 700, fontSize: 13 }}>Deploy the knowledge base + LLM</div>
        <button style={applying || status.running ? btnBusy : btn} disabled={applying || status.running}
          onClick={deploy}>
          {status.running ? 'Deploying…' : applying ? 'Starting…' : svcs.length ? 'Re-deploy' : 'Deploy'}
        </button>
      </div>
      <div style={{ fontSize: 12, color: '#556', lineHeight: 1.5, marginBottom: svcs.length ? 8 : 0 }}>
        aimee-server brings up postgres + aimee-kb + aimee-llm for you via Docker — no extra commands.
      </div>

      {svcs.length > 0 && (
        <div style={{ display: 'grid', gap: 3, marginBottom: 8 }}>
          {svcs.map((s) => (
            <div key={s.name} style={{ display: 'flex', gap: 8, fontSize: 12.5 }}>
              <span aria-hidden>{/running|up|healthy/i.test(s.state) ? '🟢' : '⚪'}</span>
              <span style={{ fontFamily: 'monospace', minWidth: 130 }}>{s.name}</span>
              <span style={{ color: '#667' }}>{s.state}</span>
            </div>
          ))}
        </div>
      )}

      {status.running && <div style={{ fontSize: 12, color: '#8a5a00' }}>⏳ Bringing services up (image pulls can take a few minutes)…</div>}
      {settledOk && <div style={{ fontSize: 12, color: '#2c8f56' }}>✅ Stack is up.</div>}
      {settledErr && <div style={{ fontSize: 12, color: '#c62828' }}>⛔ Deploy exited with code {status.last_exit}. See the log below.</div>}
      {err && <div style={{ fontSize: 12, color: '#c62828' }}>{err}</div>}

      {status.output.trim() && (
        <details style={{ marginTop: 6 }}>
          <summary style={{ cursor: 'pointer', fontSize: 12, color: '#667' }}>Deploy log</summary>
          <pre style={pre}>{status.output}</pre>
        </details>
      )}
    </div>
  );
}

const box: React.CSSProperties = {
  border: '1px solid #dde', borderRadius: 8, padding: '10px 12px', marginBottom: 14, background: '#f8fafc',
};
const pre: React.CSSProperties = {
  whiteSpace: 'pre-wrap', wordBreak: 'break-word', background: '#eef1f6', borderRadius: 6,
  padding: '8px 10px', fontSize: 11.5, margin: '6px 0 0', maxHeight: 220, overflow: 'auto',
};
const btn: React.CSSProperties = {
  padding: '6px 14px', borderRadius: 7, border: '1px solid #2c6', background: '#2c8f56',
  color: '#fff', cursor: 'pointer', fontSize: 13, fontWeight: 600,
};
const btnBusy: React.CSSProperties = { ...btn, background: '#9aa', borderColor: '#9aa', cursor: 'default' };
