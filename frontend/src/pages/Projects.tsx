import { useEffect, useState, useCallback } from 'react';
import { Panel } from '@rakuensoftware/smoothgui';

/* Git projects (webchat-git WP-F2). Lists the user's cloned repos, connects a
 * new one, and runs per-project git ops — all via /api/git/* (the server forwards
 * to /v1/workspace/* with the user's webuser: identity; creds live only in the
 * sealed vault). */

const READ_OPS = ['status', 'log', 'diff', 'branch'] as const;
const REMOTE_OPS = ['fetch', 'pull', 'push'] as const;

async function api(path: string, init?: RequestInit): Promise<Response> {
  return fetch(path, {
    ...init,
    headers: {
      'Content-Type': 'application/json',
      'X-CSRF-Token': window._csrf || '',
      ...(init?.headers || {}),
    },
  });
}

const btn: React.CSSProperties = {
  padding: '3px 10px', borderRadius: '4px', border: '1px solid #ccc', background: '#fff',
  color: '#444', cursor: 'pointer', fontSize: '12px',
};
const input: React.CSSProperties = {
  padding: '6px 8px', borderRadius: '4px', border: '1px solid #ccc', fontSize: '13px',
};

export default function Projects() {
  const [projects, setProjects] = useState<string[]>([]);
  const [selected, setSelected] = useState<string>('');
  const [output, setOutput] = useState<string>('');
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string>('');
  const [url, setUrl] = useState('');
  const [name, setName] = useState('');
  const [commitMsg, setCommitMsg] = useState('');
  const [branch, setBranch] = useState('');

  const loadProjects = useCallback(async () => {
    setErr('');
    try {
      const r = await api('/api/git/projects', { method: 'GET' });
      const d = await r.json();
      if (!r.ok) { setErr(d.error || 'failed to list projects'); return; }
      const ps: string[] = d.projects || [];
      setProjects(ps);
      setSelected(s => (s && ps.includes(s) ? s : ps[0] || ''));
    } catch {
      setErr('aimee-server unavailable');
    }
  }, []);

  useEffect(() => { loadProjects(); }, [loadProjects]);

  async function connect() {
    if (!url.trim()) return;
    setBusy(true); setErr(''); setOutput('');
    try {
      const r = await api('/api/git/clone', {
        method: 'POST',
        body: JSON.stringify({ url: url.trim(), name: name.trim() || undefined }),
      });
      const d = await r.json();
      if (!r.ok) { setErr(d.error || 'clone failed'); }
      else { setUrl(''); setName(''); await loadProjects(); setSelected(d.name || ''); }
    } finally { setBusy(false); }
  }

  async function runOp(op: string, extra?: Record<string, unknown>) {
    if (!selected) return;
    setBusy(true); setErr(''); setOutput('');
    try {
      const r = await api('/api/git/op', {
        method: 'POST',
        body: JSON.stringify({ project: selected, op, ...extra }),
      });
      const d = await r.json();
      if (!r.ok) { setErr(d.error || `${op} failed`); }
      else { setOutput(d.output || `(${op}: ok)`); }
    } finally { setBusy(false); }
  }

  return (
    <div style={{ padding: '16px', display: 'flex', flexDirection: 'column', gap: '12px', height: '100%', overflow: 'auto' }}>
      <Panel title="Connect a repository">
        <div style={{ padding: '12px', display: 'flex', gap: '8px', flexWrap: 'wrap', alignItems: 'center' }}>
          <input style={{ ...input, flex: 2, minWidth: '260px' }} placeholder="git remote URL (https or ssh)"
            value={url} onChange={e => setUrl(e.target.value)} />
          <input style={{ ...input, flex: 1, minWidth: '120px' }} placeholder="name (optional)"
            value={name} onChange={e => setName(e.target.value)} />
          <button style={{ ...btn, background: '#234', color: '#8cf', borderColor: '#456' }}
            disabled={busy || !url.trim()} onClick={connect}>Clone</button>
        </div>
      </Panel>

      <Panel title="Projects" count={projects.length}>
        <div style={{ padding: '12px', display: 'flex', flexDirection: 'column', gap: '10px' }}>
          {projects.length === 0 ? (
            <div style={{ color: '#888', fontSize: '13px' }}>No projects yet — connect a repository above.</div>
          ) : (
            <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap' }}>
              {projects.map(p => (
                <button key={p} onClick={() => { setSelected(p); setOutput(''); setErr(''); }}
                  style={{ ...btn, background: p === selected ? '#234' : '#fff', color: p === selected ? '#8cf' : '#444',
                           borderColor: p === selected ? '#456' : '#ccc' }}>
                  {p}
                </button>
              ))}
            </div>
          )}

          {selected && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '8px' }}>
              <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap', alignItems: 'center' }}>
                {READ_OPS.map(op => (
                  <button key={op} style={btn} disabled={busy} onClick={() => runOp(op)}>{op}</button>
                ))}
                {REMOTE_OPS.map(op => (
                  <button key={op} style={{ ...btn, borderColor: '#a96' }} disabled={busy} onClick={() => runOp(op)}>{op}</button>
                ))}
              </div>
              <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap', alignItems: 'center' }}>
                <input style={{ ...input, flex: 1, minWidth: '180px' }} placeholder="commit message"
                  value={commitMsg} onChange={e => setCommitMsg(e.target.value)} />
                <button style={btn} disabled={busy || !commitMsg.trim()}
                  onClick={() => { runOp('commit', { message: commitMsg }); setCommitMsg(''); }}>commit</button>
                <input style={{ ...input, width: '140px' }} placeholder="branch"
                  value={branch} onChange={e => setBranch(e.target.value)} />
                <button style={btn} disabled={busy || !branch.trim()}
                  onClick={() => runOp('checkout', { branch })}>checkout</button>
              </div>
            </div>
          )}

          {err && <div style={{ color: '#c62828', fontSize: '12px' }}>{err}</div>}
          {output && (
            <pre style={{ fontSize: '12px', background: '#0d1117', color: '#c9d1d9', padding: '10px',
                          borderRadius: '6px', overflow: 'auto', maxHeight: '320px', whiteSpace: 'pre-wrap' }}>
              {output}
            </pre>
          )}
          {busy && <div style={{ color: '#888', fontSize: '12px' }}>working…</div>}
        </div>
      </Panel>
    </div>
  );
}
