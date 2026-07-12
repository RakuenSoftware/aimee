import { useCallback, useEffect, useState } from 'react';

/* Wizard — Workspaces & projects. A workspace is your collection of projects: the
 * repos under an owner/org (e.g. github.com/RakuenSoftware). Point at that owner,
 * list its repositories, pick the ones you want, and bulk-clone them into your
 * workspace. Provider-agnostic (GitHub, GitLab, Gitea/Forgejo, Bitbucket) via the
 * server's host descriptor table.
 *
 * All calls go through the existing /api/git/* proxy:
 *   GET  /api/git/org-repos?host=&owner=   → { provider, repos:[{name,clone_url,…}] }
 *   POST /api/git/clone-org {host,owner,repos:[{name,clone_url}]} → { results:[…] }
 *   POST /api/git/credentials {host,token} (optional, for a private/self-hosted org)
 *   GET  /api/git/projects → already-cloned projects (so re-runs are clear). */

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
    headers: {
      'Content-Type': 'application/json',
      'X-CSRF-Token': csrf(),
      ...(init?.headers || {}),
    },
  });
}

interface Repo {
  name: string;
  clone_url: string;
  ssh_url?: string;
  private?: boolean;
}

interface CloneResult {
  name: string;
  ok: boolean;
  project?: string | null;
  error?: string | null;
}

/** Parse "github.com/RakuenSoftware", "https://github.com/RakuenSoftware/", or
 * "host owner" into {host, owner}. Returns null when either part is missing. */
export function parseOwner(input: string): { host: string; owner: string } | null {
  let s = input.trim();
  if (!s) return null;
  s = s.replace(/^[a-z]+:\/\//i, ''); // strip scheme
  s = s.replace(/^git@/i, '').replace(':', '/'); // git@host:owner → host/owner
  const parts = s.split('/').filter(Boolean);
  if (parts.length < 2) return null;
  const host = parts[0].toLowerCase();
  const owner = parts[1];
  if (!host.includes('.') || !owner) return null;
  return { host, owner };
}

export interface ConnectWorkspaceProps {
  /** Continue to the wizard summary. */
  onDone: () => void;
}

export default function ConnectWorkspace({ onDone }: ConnectWorkspaceProps) {
  const [ownerInput, setOwnerInput] = useState('');
  const [token, setToken] = useState('');
  const [repos, setRepos] = useState<Repo[]>([]);
  const [selected, setSelected] = useState<Record<string, boolean>>({});
  const [provider, setProvider] = useState('');
  const [results, setResults] = useState<CloneResult[]>([]);
  const [projects, setProjects] = useState<string[]>([]);
  const [listing, setListing] = useState(false);
  const [cloning, setCloning] = useState(false);
  const [err, setErr] = useState('');

  const loadProjects = useCallback(async () => {
    try {
      const r = await api('/api/git/projects', { method: 'GET' });
      const d = await r.json();
      if (r.ok) setProjects(d.projects || []);
    } catch {
      /* server unavailable — leave empty */
    }
  }, []);

  useEffect(() => {
    loadProjects();
  }, [loadProjects]);

  async function listRepos() {
    const parsed = parseOwner(ownerInput);
    if (!parsed) {
      setErr('Enter an owner like github.com/RakuenSoftware');
      return;
    }
    setListing(true);
    setErr('');
    setResults([]);
    try {
      // Save the token first (for a private/self-hosted org) so enumeration is authed.
      if (token.trim()) {
        await api('/api/git/credentials', {
          method: 'POST',
          body: JSON.stringify({ host: parsed.host, token: token.trim() }),
        }).catch(() => undefined);
        setToken('');
      }
      const q = `host=${encodeURIComponent(parsed.host)}&owner=${encodeURIComponent(parsed.owner)}`;
      const r = await api(`/api/git/org-repos?${q}`, { method: 'GET' });
      const d = await r.json().catch(() => ({}));
      if (!r.ok) {
        setErr(d.error || `could not list repositories (${r.status})`);
        setRepos([]);
        return;
      }
      const list: Repo[] = d.repos || [];
      setRepos(list);
      setProvider(d.provider || '');
      // Default: select every repo not already cloned.
      const sel: Record<string, boolean> = {};
      for (const repo of list) sel[repo.name] = !projects.includes(repo.name);
      setSelected(sel);
      if (list.length === 0) setErr('No repositories found for that owner.');
    } catch {
      setErr('aimee-server unavailable');
      setRepos([]);
    } finally {
      setListing(false);
    }
  }

  async function cloneSelected() {
    const parsed = parseOwner(ownerInput);
    if (!parsed) return;
    const chosen = repos.filter((r) => selected[r.name]).map((r) => ({ name: r.name, clone_url: r.clone_url }));
    if (chosen.length === 0) {
      setErr('Select at least one repository.');
      return;
    }
    setCloning(true);
    setErr('');
    try {
      const r = await api('/api/git/clone-org', {
        method: 'POST',
        body: JSON.stringify({ host: parsed.host, owner: parsed.owner, repos: chosen }),
      });
      const d = await r.json().catch(() => ({}));
      if (!r.ok) {
        setErr(d.error || `clone failed (${r.status})`);
        return;
      }
      setResults(d.results || []);
      await loadProjects();
    } catch {
      setErr('aimee-server unavailable');
    } finally {
      setCloning(false);
    }
  }

  const selectedCount = repos.filter((r) => selected[r.name]).length;
  const toggleAll = (on: boolean) => {
    const sel: Record<string, boolean> = {};
    for (const r of repos) sel[r.name] = on;
    setSelected(sel);
  };

  return (
    <div style={{ display: 'grid', gap: 14, marginBottom: 8 }}>
      <div style={{ fontSize: 12.5, color: '#556', lineHeight: 1.5 }}>
        A <b>workspace</b> is your collection of projects — the repositories under an owner or org.
        Point at one, pick the repos, and clone them in.
      </div>

      <section style={{ display: 'grid', gap: 8 }}>
        <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', alignItems: 'center' }}>
          <input style={{ ...input, flex: 2, minWidth: 240 }}
            placeholder="owner URL (e.g. github.com/RakuenSoftware)"
            value={ownerInput} onChange={(e) => setOwnerInput(e.target.value)} />
          <button style={primaryBtn} disabled={listing || !ownerInput.trim()} onClick={listRepos}>
            {listing ? 'Listing…' : 'List repositories'}
          </button>
        </div>
        <input style={{ ...input, width: '100%' }} type="password" autoComplete="off"
          placeholder="access token — only for a private/self-hosted org; saved server-side per host"
          value={token} onChange={(e) => setToken(e.target.value)} />
      </section>

      {repos.length > 0 && (
        <section style={{ display: 'grid', gap: 6 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
            <div style={{ fontSize: 12.5, fontWeight: 700 }}>
              {repos.length} repositor{repos.length === 1 ? 'y' : 'ies'}{provider ? ` · ${provider}` : ''}
            </div>
            <button style={linkBtn} onClick={() => toggleAll(true)}>all</button>
            <button style={linkBtn} onClick={() => toggleAll(false)}>none</button>
          </div>
          <div style={{ display: 'grid', gap: 2, maxHeight: 220, overflow: 'auto', border: '1px solid #eee', borderRadius: 6, padding: 8 }}>
            {repos.map((repo) => {
              const already = projects.includes(repo.name);
              return (
                <label key={repo.name} style={{ display: 'flex', alignItems: 'center', gap: 8, fontSize: 13, cursor: 'pointer' }}>
                  <input type="checkbox" checked={!!selected[repo.name]}
                    onChange={(e) => setSelected((p) => ({ ...p, [repo.name]: e.target.checked }))} />
                  <span style={{ fontFamily: 'monospace', flex: 1 }}>{repo.name}</span>
                  {repo.private && <span style={{ fontSize: 10.5, color: '#8a5a00', background: '#fff6e6', border: '1px solid #f0d8a8', borderRadius: 4, padding: '0 5px' }}>private</span>}
                  {already && <span style={{ fontSize: 11, color: '#2a7' }}>● cloned</span>}
                </label>
              );
            })}
          </div>
          <div>
            <button style={primaryBtn} disabled={cloning || selectedCount === 0} onClick={cloneSelected}>
              {cloning ? 'Cloning…' : `Clone selected (${selectedCount})`}
            </button>
          </div>
        </section>
      )}

      {results.length > 0 && (
        <section style={{ display: 'grid', gap: 3 }}>
          <div style={{ fontSize: 12.5, fontWeight: 700 }}>Clone results</div>
          {results.map((res) => (
            <div key={res.name} style={{ display: 'flex', alignItems: 'center', gap: 8, fontSize: 12.5 }}>
              <span aria-hidden>{res.ok ? '✅' : '⛔'}</span>
              <span style={{ fontFamily: 'monospace', flex: 1 }}>{res.name}</span>
              {!res.ok && <span style={{ color: '#c62828' }}>{res.error || 'failed'}</span>}
            </div>
          ))}
        </section>
      )}

      {projects.length > 0 && (
        <div style={{ fontSize: 12, color: '#667' }}>
          Workspace has {projects.length} project{projects.length > 1 ? 's' : ''}: {projects.join(', ')}
        </div>
      )}

      {err && <div style={{ fontSize: 12.5, color: '#c62828' }}>{err}</div>}

      <div>
        <button style={{ ...primaryBtn, background: '#2c8f56' }} onClick={onDone}>
          {projects.length > 0 ? 'Done' : 'Continue'}
        </button>
      </div>
    </div>
  );
}

const input: React.CSSProperties = {
  boxSizing: 'border-box', padding: '7px 9px', borderRadius: 6,
  border: '1px solid #ccd', fontSize: 13, fontFamily: 'ui-monospace, monospace',
};
const primaryBtn: React.CSSProperties = {
  padding: '7px 16px', borderRadius: 7, border: '1px solid #2c6', background: '#2c8f56',
  color: '#fff', cursor: 'pointer', fontSize: 13.5, fontWeight: 600,
};
const linkBtn: React.CSSProperties = {
  background: 'none', border: 'none', color: '#3a6ea5', cursor: 'pointer', fontSize: 12, padding: 0,
};
