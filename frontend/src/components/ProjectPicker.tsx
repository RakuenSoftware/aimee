import { useCallback, useEffect, useState } from 'react';
import { Button, Picker } from '@rakuensoftware/smoothgui';
import type { GitProjectsResponse } from '../setup/ownerUrl';
import { notifySetupUpdated } from '../setup/setupState';

/* The session owns the selected project. Loading or refreshing the options must
 * never change that binding; only an explicit selection or clone emits a change. */

export interface ProjectSelection {
  project: string;
  root: string;
}

async function api(path: string, init?: RequestInit): Promise<Response> {
  return fetch(path, {
    ...init,
    headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '', ...(init?.headers || {}) },
  });
}

const input: React.CSSProperties = { padding: '5px 8px', borderRadius: 4, border: '1px solid var(--sg-border-medium)', fontSize: 13 };

export default function ProjectPicker({ value, onChange }: {
  /* Absolute checkout path, matching Session.projectRoot (including any org). */
  value: string;
  onChange: (sel: ProjectSelection | null) => void;
}) {
  const [projects, setProjects] = useState<string[]>([]);
  const [root, setRoot] = useState('');
  const [cloneOpen, setCloneOpen] = useState(false);
  const [url, setUrl] = useState('');
  const [token, setToken] = useState('');
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState('');

  const load = useCallback(async () => {
    try {
      const r = await api('/api/git/projects', { method: 'GET' });
      const d: GitProjectsResponse = await r.json();
      if (!r.ok) { setErr(d.error || 'failed to list projects'); return; }
      const ps: string[] = d.projects || [];
      const rootPath = d.root || '';
      setProjects(ps);
      setRoot(rootPath);
      return rootPath;
    } catch { setErr('aimee-server unavailable'); }
  }, []);

  useEffect(() => { void load(); }, [load]);

  function select(project: string, rootPath = root) {
    onChange(project && rootPath ? { project, root: rootPath } : null);
  }

  async function clone() {
    if (!url.trim()) return;
    setBusy(true); setErr('');
    try {
      const r = await api('/api/git/clone', {
        method: 'POST',
        body: JSON.stringify({ url: url.trim(), token: token.trim() || undefined }),
      });
      const d = await r.json();
      if (!r.ok) { setErr(d.error || 'clone failed'); return; }
      setUrl(''); setToken(''); setCloneOpen(false);
      const rootPath = await load();
      if (d.name && rootPath) select(d.name, rootPath);
      notifySetupUpdated();
    } finally { setBusy(false); }
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 6, padding: '8px 12px', background: 'var(--sg-surface-alt)', borderBottom: '1px solid var(--sg-border)' }}>
      <Picker
        label="Project"
        emptyLabel="— none —"
        options={projects.map(p => ({ value: `${root}/${p}`, label: p }))}
        value={value}
        onChange={path => select(projects.find(project => `${root}/${project}` === path) ?? '')}
        error={err}
        actions={
          <Button size="md" onClick={() => setCloneOpen(o => !o)}>
            {cloneOpen ? 'Cancel' : '+ Clone repo'}
          </Button>
        }
      />
      {cloneOpen && (
        <div style={{ display: 'flex', gap: 8, alignItems: 'center', flexWrap: 'wrap' }}>
          <input style={{ ...input, flex: 2, minWidth: 260 }} placeholder="git remote URL (https or ssh)"
            value={url} onChange={e => setUrl(e.target.value)} />
          <input style={{ ...input, flex: 1, minWidth: 160 }} type="password" autoComplete="off"
            placeholder="access token (private repos)" value={token} onChange={e => setToken(e.target.value)} />
          <Button variant="primary" size="md"
            disabled={busy || !url.trim()} onClick={clone}>Clone</Button>
        </div>
      )}
    </div>
  );
}
