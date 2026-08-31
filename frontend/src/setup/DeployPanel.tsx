import { useCallback, useEffect, useRef, useState } from 'react';
import { Button } from '@rakuensoftware/smoothgui';

/* Setup-wizard summary panel — server-orchestrated deploy.
 *
 * When aimee-server runs with the Docker socket mounted (AIMEE_DEPLOY_ENABLED), it
 * can bring up the managed sibling service (aimee-kb) from
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

interface Enrollment {
  state: 'ready' | 'paired';
  principal: string;
  tier: 'full';
  mtls: boolean;
  tls_port: number;
  bearer_token?: string;
}

export interface Svc {
  name: string;
  state: string;
}

export function serviceFailed(s: Svc): boolean {
  return /\b(exited|dead|unhealthy|restarting)\b/i.test(s.state);
}

// Something is still coming up, so keep polling and show it as pending rather
// than as done or failed.
//
// This used to match ONLY aimee-llm, because that container was the slow one. It
// is retired, which would have left the predicate permanently false and stopped
// the panel polling while aimee-kb was still starting. Match any service in a
// starting state instead. "restarting" is excluded by serviceFailed, and the
// word boundary keeps it from matching here in the first place.
export function servicePending(s: Svc): boolean {
  return /\b(starting|created|waiting)\b/i.test(s.state) && !serviceFailed(s);
}

/* docker compose ps --format json emits either a JSON array or newline-delimited
 * objects, depending on the compose version. Parse defensively; return [] when the
 * shape is unrecognized (the raw output stays available in the log). */
export function parsePs(ps: string): Svc[] {
  const pick = (o: Record<string, unknown>): Svc => {
    const state = String(o.State ?? o.Status ?? '');
    const health = String(o.Health ?? '');
    return {
      name: String(o.Name ?? o.Service ?? ''),
      state: health && !state.toLowerCase().includes(health.toLowerCase()) ? `${state} (${health})` : state,
    };
  };
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

export function remoteSetCommand(hostname: string, port: number, bearer: string): string {
  const host = hostname.includes(':') && !hostname.startsWith('[') ? `[${hostname}]` : hostname;
  return `aimee remote set https://${host}:${port} ${bearer}`;
}

export default function DeployPanel({ kbMode }: { kbMode: 'local' | 'remote' }) {
  const [loading, setLoading] = useState(true);
  const [status, setStatus] = useState<DeployStatus | null>(null);
  const [applying, setApplying] = useState(false);
  const [err, setErr] = useState('');
  const [enrollment, setEnrollment] = useState<Enrollment | null>(null);
  const [copied, setCopied] = useState(false);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);

  const loadStatus = useCallback(async (): Promise<DeployStatus | null> => {
    try {
      const r = await api('/api/deploy/status', { method: 'GET' });
      if (r.status === 503) {
        const disabled = { enabled: false, running: false, last_exit: null, output: '', ps: '' };
        setStatus(disabled);
        return disabled;
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
        return s;
      }
    } catch {
      setStatus({ enabled: false, running: false, last_exit: null, output: '', ps: '' });
    }
    return null;
  }, []);

  // The worker stops after the LLM container starts, not after its model assets
  // finish downloading. Keep monitoring a "starting" LLM without leaving the
  // wizard in its blocking Deploying state, so a later container failure is
  // surfaced while this finish screen remains open.
  const poll = useCallback(async () => {
    const current = await loadStatus();
    if (current?.running) {
      timer.current = setTimeout(poll, 3000);
    } else {
      setApplying(false);
      const services = parsePs(current?.ps ?? '');
      if (services.some(servicePending)) timer.current = setTimeout(poll, 5000);
    }
  }, [loadStatus]);

  useEffect(() => {
    if (kbMode !== 'local') {
      setLoading(false);
      return;
    }
    (async () => {
      const current = await loadStatus();
      setLoading(false);
      if (current?.running || parsePs(current?.ps ?? '').some(servicePending)) {
        setApplying(!!current?.running);
        timer.current = setTimeout(poll, 3000);
      }
    })();
    return () => {
      if (timer.current) clearTimeout(timer.current);
    };
  }, [kbMode, loadStatus, poll]);

  async function deploy() {
    if (timer.current) {
      clearTimeout(timer.current);
      timer.current = null;
    }
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
      if (d.enrollment?.state === 'ready' || d.enrollment?.state === 'paired') {
        setEnrollment({
          state: d.enrollment.state,
          principal: String(d.enrollment.principal ?? ''),
          tier: 'full',
          mtls: !!d.enrollment.mtls,
          tls_port: Number(d.enrollment.tls_port) || 8743,
          bearer_token: d.enrollment.bearer_token ? String(d.enrollment.bearer_token) : undefined,
        });
      }
      await loadStatus();
      timer.current = setTimeout(poll, 3000);
    } catch {
      setErr('aimee-server unavailable');
      setApplying(false);
    }
  }

  async function copyEnrollment(command: string) {
    try {
      await navigator.clipboard.writeText(command);
      setCopied(true);
    } catch {
      setErr('Could not copy automatically; select the command below and copy it manually.');
    }
  }

  if (kbMode !== 'local' || loading || !status) return null;

  // Orchestration off: hand the operator the one compose command instead.
  if (!status.enabled) {
    return (
      <div style={box}>
        <div style={{ fontWeight: 700, fontSize: 13, marginBottom: 4 }}>Bring up the stack</div>
        <div style={{ fontSize: 12, color: 'var(--sg-text-muted)', lineHeight: 1.5 }}>
          This server can’t launch containers itself. Start the knowledge base + LLM alongside it with:
          <pre style={pre}>docker compose -f compose.server.yaml up -d</pre>
        </div>
      </div>
    );
  }

  const svcs = parsePs(status.ps);
  const failedSvcs = svcs.filter(serviceFailed);
  const settledOk = !status.running && status.last_exit === 0 && failedSvcs.length === 0;
  const settledErr = !status.running && status.last_exit !== null && status.last_exit !== 0;
  const enrollmentCommand = enrollment?.bearer_token
    ? remoteSetCommand(window.location.hostname, enrollment.tls_port, enrollment.bearer_token)
    : '';

  return (
    <div style={box}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 6 }}>
        <div style={{ fontWeight: 700, fontSize: 13 }}>Deploy the knowledge base + LLM</div>
        <Button variant="primary"
          style={applying || status.running ? { background: 'var(--sg-text-hint)', borderColor: 'var(--sg-text-hint)', cursor: 'default' } : undefined}
          disabled={applying || status.running}
          onClick={deploy}>
          {status.running ? 'Deploying…' : applying ? 'Starting…' : svcs.length ? 'Re-deploy' : 'Deploy'}
        </Button>
      </div>
      <div style={{ fontSize: 12, color: 'var(--sg-text-muted)', lineHeight: 1.5, marginBottom: svcs.length ? 8 : 0 }}>
        aimee-server brings up aimee-kb, including its database, for you via Docker. No extra commands.
      </div>

      {svcs.length > 0 && (
        <div style={{ display: 'grid', gap: 3, marginBottom: 8 }}>
          {svcs.map((s) => (
            <div key={s.name} style={{ display: 'flex', gap: 8, fontSize: 12.5 }}>
              <span aria-hidden>{serviceFailed(s) ? '🔴' : servicePending(s) ? '🟡' : /running|up|healthy/i.test(s.state) ? '🟢' : '⚪'}</span>
              <span style={{ fontFamily: 'monospace', minWidth: 130 }}>{s.name}</span>
              <span style={{ color: 'var(--sg-text-secondary)' }}>{s.state}</span>
            </div>
          ))}
        </div>
      )}

      {status.running && <div style={{ fontSize: 12, color: 'var(--sg-warning-dark)' }}>⏳ Starting KB, then LLM (image pulls can take a few minutes)…</div>}
      {settledOk && <div style={{ fontSize: 12, color: 'var(--sg-success-dark)' }}>
        ✅ Stack containers are up. LLM model downloads may continue in the background.
      </div>}
      {settledErr && <div style={{ fontSize: 12, color: 'var(--sg-danger-dark)' }}>⛔ Deploy exited with code {status.last_exit}. See the log below.</div>}
      {!settledErr && failedSvcs.length > 0 && <div style={{ fontSize: 12, color: 'var(--sg-danger-dark)' }}>
        ⛔ {failedSvcs.map((s) => s.name).join(', ')} failed after startup. Check its container logs.
      </div>}
      {err && <div style={{ fontSize: 12, color: 'var(--sg-danger-dark)' }}>{err}</div>}

      {enrollment?.state === 'ready' && enrollmentCommand && (
        <div style={{ ...pairing, marginTop: 9 }}>
          <div style={{ fontWeight: 700, fontSize: 12.5 }}>Connect your client</div>
          <div style={{ fontSize: 12, color: 'var(--sg-text-muted)', lineHeight: 1.45, marginTop: 3 }}>
            Run this once on a Linux workstation. It pins the server, creates your private key
            locally, enrolls mTLS, and activates full write access for{' '}
            <code>{enrollment.principal}</code>.
          </div>
          <div style={{ display: 'flex', alignItems: 'start', gap: 7 }}>
            <pre style={{ ...pre, flex: 1 }}>{enrollmentCommand}</pre>
            <Button onClick={() => copyEnrollment(enrollmentCommand)}>
              {copied ? 'Copied' : 'Copy command'}
            </Button>
          </div>
          <div style={{ fontSize: 11.5, color: 'var(--sg-text-secondary)', marginTop: 5 }}>
            The bearer alone cannot write; the full grant is bound to the enrolled client certificate.
          </div>
        </div>
      )}
      {enrollment?.state === 'paired' && (
        <div style={{ ...pairing, marginTop: 9, color: 'var(--sg-success-dark)' }}>
          ✅ {enrollment.principal} already has an enrolled mTLS client with full write access.
        </div>
      )}

      {status.output.trim() && (
        <details style={{ marginTop: 6 }}>
          <summary style={{ cursor: 'pointer', fontSize: 12, color: 'var(--sg-text-secondary)' }}>Deploy log</summary>
          <pre style={pre}>{status.output}</pre>
        </details>
      )}
    </div>
  );
}

const box: React.CSSProperties = {
  border: '1px solid var(--sg-border-medium)', borderRadius: 8, padding: '10px 12px', marginBottom: 14, background: 'var(--sg-surface-alt)',
};
const pre: React.CSSProperties = {
  whiteSpace: 'pre-wrap', wordBreak: 'break-word', background: 'var(--sg-surface-sunken)', borderRadius: 6,
  padding: '8px 10px', fontSize: 11.5, margin: '6px 0 0', maxHeight: 220, overflow: 'auto',
};
const pairing: React.CSSProperties = {
  border: '1px solid var(--sg-info-border)', borderRadius: 7, background: 'var(--sg-info-bg)', padding: '8px 10px',
  fontSize: 12,
};
