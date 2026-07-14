import { useCallback, useEffect, useRef, useState } from 'react';
import { Picker } from '@rakuensoftware/smoothgui';
import type { GitProjectsResponse } from '../setup/ownerUrl';

/* ProjectPicker — a compact "select or clone a project" control embedded in each
 * tool tab. Each tab passes its own storageKey so its selected project persists
 * independently (per the per-tab project model). onChange fires with the selected
 * project name and the user's workspace root (so the tab can act on it — the
 * editor opens root/<project>, chat sets cwd to it, etc.).
 *
 * The select is the generic smoothgui <Picker>; this component supplies the
 * aimee-specific data (/api/git/projects) and the clone-repo action. */

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

const input: React.CSSProperties = { padding: '5px 8px', borderRadius: 4, border: '1px solid #ccc', fontSize: 13 };
const btn: React.CSSProperties = { padding: '5px 10px', borderRadius: 4, border: '1px solid #ccc', background: '#fff', color: '#444', cursor: 'pointer', fontSize: 13 };

export default function ProjectPicker({ storageKey, onChange }: {
  storageKey: string;
  onChange: (sel: ProjectSelection | null) => void;
}) {
  const [projects, setProjects] = useState<string[]>([]);
  const [root, setRoot] = useState('');
  const [selected, setSelected] = useState<string>(() => localStorage.getItem(storageKey) || '');
  const [cloneOpen, setCloneOpen] = useState(false);
  const [url, setUrl] = useState('');
  const [token, setToken] = useState('');
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState('');

  // onChange goes through a ref so its identity never feeds the load/effect
  // chain: parents pass inline closures that patch their own state, and a
  // load() → onChange → parent re-render → new closure → new load() cycle
  // re-fires the mount effect forever (an unthrottled /api/git/projects loop).
  const onChangeRef = useRef(onChange);
  useEffect(() => { onChangeRef.current = onChange; }, [onChange]);

  const emit = useCallback((project: string, rootPath: string) => {
    onChangeRef.current(project && rootPath ? { project, root: rootPath } : null);
  }, []);

  const load = useCallback(async () => {
    try {
      const r = await api('/api/git/projects', { method: 'GET' });
      const d: GitProjectsResponse = await r.json();
      if (!r.ok) { setErr(d.error || 'failed to list projects'); return; }
      const ps: string[] = d.projects || [];
      const rootPath = d.root || '';
      setProjects(ps);
      setRoot(rootPath);
      // keep the persisted selection if still present, else clear
      const prev = localStorage.getItem(storageKey) || '';
      const next = prev && ps.includes(prev) ? prev : '';
      setSelected(next);
      emit(next, rootPath);
    } catch { setErr('aimee-server unavailable'); }
  }, [emit, storageKey]);

  useEffect(() => { load(); }, [load]);

  function select(p: string) {
    setSelected(p);
    if (p) localStorage.setItem(storageKey, p); else localStorage.removeItem(storageKey);
    emit(p, root);
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
      await load();
      if (d.name) select(d.name);
    } finally { setBusy(false); }
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 6, padding: '8px 12px', background: '#f6f7f9', borderBottom: '1px solid #e3e6ea' }}>
      <Picker
        label="Project"
        emptyLabel="— none —"
        options={projects.map(p => ({ value: p, label: p }))}
        value={selected}
        onChange={select}
        error={err}
        actions={
          <button style={btn} onClick={() => setCloneOpen(o => !o)}>
            {cloneOpen ? 'Cancel' : '+ Clone repo'}
          </button>
        }
      />
      {cloneOpen && (
        <div style={{ display: 'flex', gap: 8, alignItems: 'center', flexWrap: 'wrap' }}>
          <input style={{ ...input, flex: 2, minWidth: 260 }} placeholder="git remote URL (https or ssh)"
            value={url} onChange={e => setUrl(e.target.value)} />
          <input style={{ ...input, flex: 1, minWidth: 160 }} type="password" autoComplete="off"
            placeholder="access token (private repos)" value={token} onChange={e => setToken(e.target.value)} />
          <button style={{ ...btn, background: '#234', color: '#8cf', borderColor: '#456' }}
            disabled={busy || !url.trim()} onClick={clone}>Clone</button>
        </div>
      )}
    </div>
  );
}
