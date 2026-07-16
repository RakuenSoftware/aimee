import { useEffect, useState, useCallback } from 'react';
import { Panel } from '@rakuensoftware/smoothgui';
import { useSessions } from '../SessionContext';
import { groupProjectsByOrg, parseOwnerOnly, repoAlreadyCloned, type CloneKbAnnotations, type GitProjectsResponse, type OwnerRef, type ProjectDetail, type ProjectDeleteResponse } from '../setup/ownerUrl';

/* Git projects (webchat-git WP-F2). Lists the user's cloned repos, connects a
 * new one, and runs per-project git ops — all via /api/git/* (the server forwards
 * to /v1/workspace/* with the user's webuser: identity; creds live only in the
 * sealed vault). The connect field also accepts an owner/org URL (e.g.
 * github.com/RakuenSoftware): it then enumerates the owner's repositories and
 * bulk-clones the selected ones, mirroring the wizard's Workspaces step. */

const READ_OPS = ['status', 'log', 'diff', 'branch'] as const;
const REMOTE_OPS = ['fetch', 'pull', 'push'] as const;
// Hover help for each git op button.
const OP_HELP: Record<string, string> = {
  status: 'Show the working-tree status of this project.',
  log: 'Show recent commit history.',
  diff: 'Show uncommitted changes.',
  branch: 'List branches.',
  fetch: 'Fetch updates from the remote.',
  pull: 'Pull and merge remote changes.',
  push: 'Push local commits to the remote.',
};

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
  const { active } = useSessions();
  const [projects, setProjects] = useState<string[]>([]);
  const [details, setDetails] = useState<ProjectDetail[]>([]);
  const [selected, setSelected] = useState<string>('');
  const [output, setOutput] = useState<string>('');
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string>('');
  const [url, setUrl] = useState('');
  const [name, setName] = useState('');
  const [token, setToken] = useState('');
  const [commitMsg, setCommitMsg] = useState('');
  const [prTitle, setPrTitle] = useState('');
  const [branch, setBranch] = useState('');
  const [hosts, setHosts] = useState<string[]>([]);
  const [credHost, setCredHost] = useState('');
  const [credToken, setCredToken] = useState('');
  const [credSSHKey, setCredSSHKey] = useState('');
  const [ghCode, setGhCode] = useState('');
  const [ghUri, setGhUri] = useState('');
  const [ghConfigured, setGhConfigured] = useState(false);
  const [ghClientId, setGhClientId] = useState('');
  // Owner/org bulk-clone (URL parsed as an owner with no repo segment).
  const [orgRef, setOrgRef] = useState<OwnerRef | null>(null);
  const [orgRepos, setOrgRepos] = useState<{ name: string; clone_url: string; private?: boolean }[]>([]);
  const [orgSelected, setOrgSelected] = useState<Record<string, boolean>>({});
  const [orgProvider, setOrgProvider] = useState('');
  const [orgResults, setOrgResults] = useState<({ name: string; ok: boolean; error?: string | null } & CloneKbAnnotations)[]>([]);
  // Post-clone notices (org placement, kb indexing) for the single-repo form.
  const [cloneNotes, setCloneNotes] = useState<string[]>([]);
  // Delete flow: the ref pending confirmation, the typed-ref gate, and whether
  // the last attempt aborted on an unavailable knowledge service (503 → offer
  // retry / explicit force, which itself needs a second click to confirm).
  const [delRef, setDelRef] = useState('');
  const [delTyped, setDelTyped] = useState('');
  const [delKbDown, setDelKbDown] = useState(false);
  const [delForceArmed, setDelForceArmed] = useState(false);
  const [notice, setNotice] = useState('');

  const loadGhConfig = useCallback(async () => {
    try {
      const r = await api('/api/git/oauth/github/config', { method: 'GET' });
      const d = await r.json();
      if (r.ok) { setGhConfigured(!!d.configured); setGhClientId(d.client_id || ''); }
    } catch { /* leave unconfigured */ }
  }, []);

  async function saveGhConfig() {
    if (!ghClientId.trim()) return;
    setBusy(true); setErr('');
    try {
      const r = await api('/api/git/oauth/github/config', {
        method: 'POST', body: JSON.stringify({ client_id: ghClientId.trim() }),
      });
      const d = await r.json();
      if (!r.ok) setErr(d.error || 'could not save client ID');
      else await loadGhConfig();
    } finally { setBusy(false); }
  }

  const loadHosts = useCallback(async () => {
    try {
      const r = await api('/api/git/credentials', { method: 'GET' });
      const d = await r.json();
      if (r.ok) setHosts(d.hosts || []);
    } catch { /* server unavailable — leave list empty */ }
  }, []);

  const loadProjects = useCallback(async () => {
    setErr('');
    try {
      const r = await api('/api/git/projects', { method: 'GET' });
      const d: GitProjectsResponse = await r.json();
      if (!r.ok) { setErr(d.error || 'failed to list projects'); return; }
      const ps: string[] = d.projects || [];
      setProjects(ps);
      setDetails(d.details || []);
      setSelected(s => (s && ps.includes(s) ? s : ps[0] || ''));
    } catch {
      setErr('aimee-server unavailable');
    }
  }, []);

  useEffect(() => { loadProjects(); loadHosts(); loadGhConfig(); }, [loadProjects, loadHosts, loadGhConfig]);

  // GitHub device-flow: once a user code is shown, poll until the user authorizes.
  useEffect(() => {
    if (!ghCode) return;
    let alive = true;
    const id = setInterval(async () => {
      try {
        const r = await api('/api/git/oauth/github/poll', { method: 'POST' });
        const d = await r.json();
        if (!alive) return;
        if (d.status === 'done') { setGhCode(''); setGhUri(''); await loadHosts(); }
        else if (d.status === 'error') { setGhCode(''); setGhUri(''); setErr(d.error || 'GitHub sign-in failed'); }
      } catch { /* transient; keep polling */ }
    }, 5000);
    return () => { alive = false; clearInterval(id); };
  }, [ghCode, loadHosts]);

  async function startGithub() {
    setErr('');
    try {
      const r = await api('/api/git/oauth/github/start', { method: 'POST' });
      const d = await r.json();
      if (!r.ok) { setErr(d.error || 'GitHub sign-in unavailable'); return; }
      setGhCode(d.user_code || ''); setGhUri(d.verification_uri || 'https://github.com/login/device');
    } catch { setErr('aimee-server unavailable'); }
  }

  async function addCred() {
    if (!credHost.trim() || !credToken.trim()) return;
    setBusy(true); setErr('');
    try {
      const r = await api('/api/git/credentials', {
        method: 'POST',
        body: JSON.stringify({ host: credHost.trim(), token: credToken.trim() }),
      });
      const d = await r.json();
      if (!r.ok) { setErr(d.error || 'could not save credential'); }
      else { setCredHost(''); setCredToken(''); await loadHosts(); }
    } finally { setBusy(false); }
  }

  async function addSSHKey() {
    if (!credSSHKey.trim()) return;
    setBusy(true); setErr('');
    try {
      const r = await api('/api/git/sshkey', {
        method: 'POST',
        body: JSON.stringify({ ssh_key: credSSHKey }),
      });
      const d = await r.json().catch(() => ({}));
      // Always drop the key from component state once it has left the browser —
      // never leave private-key material sitting in React state / the textarea.
      setCredSSHKey('');
      if (!r.ok) { setErr(d.error || 'could not save SSH key'); }
      else { setErr('SSH key saved'); }
    } finally { setBusy(false); }
  }

  async function removeSSHKey() {
    setBusy(true); setErr('');
    try {
      const r = await api('/api/git/sshkey', { method: 'DELETE' });
      if (!r.ok) { const d = await r.json().catch(() => ({})); setErr(d.error || 'could not clear SSH key'); }
      else { setCredSSHKey(''); setErr('SSH key cleared'); }
    } finally { setBusy(false); }
  }

  async function removeCred(host: string) {
    setBusy(true); setErr('');
    try {
      const r = await api('/api/git/credentials', {
        method: 'DELETE',
        body: JSON.stringify({ host }),
      });
      if (!r.ok) { const d = await r.json(); setErr(d.error || 'could not remove credential'); }
      else { await loadHosts(); }
    } finally { setBusy(false); }
  }

  async function connect() {
    if (!url.trim()) return;
    // An owner/org URL (no repo segment) enumerates instead of cloning.
    const owner = parseOwnerOnly(url);
    if (owner) { await listOrgRepos(owner); return; }
    setBusy(true); setErr(''); setOutput(''); setCloneNotes([]);
    try {
      const r = await api('/api/git/clone', {
        method: 'POST',
        body: JSON.stringify({ url: url.trim(), name: name.trim() || undefined, token: token.trim() || undefined }),
      });
      const d = await r.json();
      if (!r.ok) { setErr(d.error || 'clone failed'); }
      else {
        setUrl(''); setName(''); setToken('');
        const notes: string[] = [];
        if (d.org_note) notes.push(d.org_note);
        if (d.kb_indexed === false) notes.push(`not indexed in the knowledge base — ${d.kb_reason || 'knowledge service unavailable'}`);
        setCloneNotes(notes);
        await loadProjects(); await loadHosts(); setSelected(d.name || '');
      }
    } finally { setBusy(false); }
  }

  // Enumerate an owner/org's repositories (wizard parity: /api/git/org-repos).
  async function listOrgRepos(owner: OwnerRef) {
    setBusy(true); setErr(''); setOutput(''); setOrgResults([]); setCloneNotes([]);
    try {
      // Save the token first (private/self-hosted org) so enumeration is authed.
      if (token.trim()) {
        await api('/api/git/credentials', {
          method: 'POST',
          body: JSON.stringify({ host: owner.host, token: token.trim() }),
        }).catch(() => undefined);
        setToken('');
        await loadHosts();
      }
      const q = `host=${encodeURIComponent(owner.host)}&owner=${encodeURIComponent(owner.owner)}`;
      const r = await api(`/api/git/org-repos?${q}`, { method: 'GET' });
      const d = await r.json().catch(() => ({}));
      if (!r.ok) { setErr(d.error || `could not list repositories (${r.status})`); setOrgRef(null); setOrgRepos([]); return; }
      const list: { name: string; clone_url: string; private?: boolean }[] = d.repos || [];
      setOrgRef(owner);
      setOrgRepos(list);
      setOrgProvider(d.provider || '');
      // Default: select every repo not already cloned.
      const sel: Record<string, boolean> = {};
      for (const repo of list) sel[repo.name] = !repoAlreadyCloned(repo, owner.owner, projects, details);
      setOrgSelected(sel);
      if (list.length === 0) setErr('No repositories found for that owner.');
    } catch { setErr('aimee-server unavailable'); } finally { setBusy(false); }
  }

  async function cloneOrgSelected() {
    if (!orgRef) return;
    const chosen = orgRepos.filter(r => orgSelected[r.name]).map(r => ({ name: r.name, clone_url: r.clone_url }));
    if (chosen.length === 0) { setErr('Select at least one repository.'); return; }
    setBusy(true); setErr('');
    try {
      const r = await api('/api/git/clone-org', {
        method: 'POST',
        body: JSON.stringify({ host: orgRef.host, owner: orgRef.owner, repos: chosen }),
      });
      const d = await r.json().catch(() => ({}));
      if (!r.ok) { setErr(d.error || `clone failed (${r.status})`); return; }
      setOrgResults(d.results || []);
      setUrl('');
      await loadProjects();
    } catch { setErr('aimee-server unavailable'); } finally { setBusy(false); }
  }

  function openDelete(ref: string) {
    setDelRef(ref); setDelTyped(''); setDelKbDown(false); setDelForceArmed(false); setErr(''); setNotice('');
  }

  function closeDelete() {
    setDelRef(''); setDelTyped(''); setDelKbDown(false); setDelForceArmed(false);
  }

  async function deleteProject(force: boolean) {
    if (!delRef || delTyped !== delRef) return;
    setBusy(true); setErr(''); setNotice('');
    try {
      const r = await api('/api/git/projects/delete', {
        method: 'POST',
        body: JSON.stringify(force ? { ref: delRef, force: true } : { ref: delRef }),
      });
      const d: ProjectDeleteResponse = await r.json().catch(() => ({}));
      if (r.ok) {
        const kb = d.kb_status === 'retained'
          ? 'knowledge retained — other users still hold this repo'
          : d.kb_status === 'forced'
            ? 'forced — knowledge orphaned until the knowledge service returns'
            : 'knowledge purged';
        setNotice(`Deleted ${delRef} — ${kb}.`);
        closeDelete();
        await loadProjects();
      } else if (r.status === 503) {
        // kb-first ordering aborted the delete: the clone is intact. Offer a
        // retry, or an explicit force (second click) that leaves the knowledge
        // orphaned until the service returns.
        setDelKbDown(true); setDelForceArmed(false);
        setErr(d.error || 'knowledge service unavailable — retry, or force delete');
      } else {
        // A non-kb error must disarm any pending force confirmation — the
        // armed state only ever applies to the 503 kb-down flow it came from.
        setDelKbDown(false); setDelForceArmed(false);
        setErr(d.error || `delete failed (${r.status})`);
      }
    } catch { setDelForceArmed(false); setErr('aimee-server unavailable'); } finally { setBusy(false); }
  }

  async function runOp(op: string, extra?: Record<string, unknown>): Promise<boolean> {
    if (!selected) return false;
    setBusy(true); setErr(''); setOutput('');
    try {
      // When operating on the active session's project, act on that session's
      // isolated worktree (the same tree its agent edits), not the shared base.
      const session_id = active && active.projectName === selected ? active.aimeeSid : '';
      const r = await api('/api/git/op', {
        method: 'POST',
        body: JSON.stringify({ project: selected, op, session_id, ...extra }),
      });
      const d = await r.json();
      if (!r.ok) { setErr(d.error || `${op} failed`); return false; }
      setOutput(d.output || `(${op}: ok)`);
      return true;
    } catch { return false; } finally { setBusy(false); }
  }

  return (
    <div style={{ padding: '16px', display: 'flex', flexDirection: 'column', gap: '12px', height: '100%', overflow: 'auto' }}>
      <Panel title="Connect a repository">
        <div style={{ padding: '12px', display: 'flex', flexDirection: 'column', gap: '8px' }}>
          <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap', alignItems: 'center' }}>
            <input style={{ ...input, flex: 2, minWidth: '260px' }}
              placeholder="repo URL — or an owner/org (e.g. github.com/RakuenSoftware) to clone its repos"
              value={url} onChange={e => setUrl(e.target.value)}
              onKeyDown={e => { if (e.key === 'Enter' && url.trim() && !busy) connect(); }} />
            <input style={{ ...input, flex: 1, minWidth: '120px' }} placeholder="name (optional)"
              value={name} onChange={e => setName(e.target.value)} />
            <button style={{ ...btn, background: '#234', color: '#8cf', borderColor: '#456' }}
              disabled={busy || !url.trim()} onClick={connect}
              title="Clone the repository, or if you entered an owner/org URL, list its repositories to bulk-clone.">
              {parseOwnerOnly(url) ? 'List repositories' : 'Clone'}
            </button>
          </div>
          <input style={{ ...input, width: '100%' }} type="password" autoComplete="off"
            placeholder="access token — only for a private repo/org (GitHub/Gitea/GitLab…); saved server-side per host"
            value={token} onChange={e => setToken(e.target.value)} />

          {/* Owner/org enumeration: pick the repos, bulk-clone as projects. */}
          {orgRef && orgRepos.length > 0 && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '6px' }}>
              <div style={{ display: 'flex', alignItems: 'center', gap: '10px' }}>
                <div style={{ fontSize: '12.5px', fontWeight: 700 }}>
                  {orgRef.host}/{orgRef.owner}: {orgRepos.length} repositor{orgRepos.length === 1 ? 'y' : 'ies'}
                  {orgProvider ? ` · ${orgProvider}` : ''}
                </div>
                <button style={{ ...btn, border: 'none', color: '#3a6ea5', padding: 0 }}
                  title="Select every repository that is not already cloned."
                  onClick={() => setOrgSelected(Object.fromEntries(orgRepos.map(r =>
                    [r.name, !repoAlreadyCloned(r, orgRef.owner, projects, details)])))}>all</button>
                <button style={{ ...btn, border: 'none', color: '#3a6ea5', padding: 0 }}
                  title="Deselect all repositories."
                  onClick={() => setOrgSelected({})}>none</button>
              </div>
              <div style={{ display: 'flex', flexDirection: 'column', gap: '2px', maxHeight: '220px', overflow: 'auto',
                            border: '1px solid #ddd', borderRadius: '6px', padding: '8px' }}>
                {orgRepos.map(repo => {
                  const already = repoAlreadyCloned(repo, orgRef.owner, projects, details);
                  return (
                    <label key={repo.name} style={{ display: 'flex', alignItems: 'center', gap: '8px', fontSize: '13px', cursor: already ? 'default' : 'pointer' }}>
                      <input type="checkbox" disabled={already} checked={!already && !!orgSelected[repo.name]}
                        onChange={e => setOrgSelected(p => ({ ...p, [repo.name]: e.target.checked }))} />
                      <span style={{ fontFamily: 'monospace', flex: 1, color: already ? '#999' : undefined }}>{repo.name}</span>
                      {repo.private && <span style={{ fontSize: '10.5px', color: '#8a5a00' }}>private</span>}
                      {already && <span style={{ fontSize: '11px', color: '#2a7' }}>cloned</span>}
                    </label>
                  );
                })}
              </div>
              <div>
                <button style={{ ...btn, background: '#234', color: '#8cf', borderColor: '#456' }}
                  disabled={busy || orgRepos.filter(r => orgSelected[r.name]).length === 0}
                  title="Clone the checked repositories as new projects."
                  onClick={cloneOrgSelected}>
                  Clone selected ({orgRepos.filter(r => orgSelected[r.name]).length})
                </button>
              </div>
            </div>
          )}
          {orgResults.length > 0 && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '3px' }}>
              {orgResults.map(res => (
                <div key={res.name} style={{ display: 'flex', alignItems: 'center', gap: '8px', fontSize: '12.5px' }}>
                  <span aria-hidden>{res.ok ? '✅' : '⛔'}</span>
                  <span style={{ fontFamily: 'monospace', flex: 1 }}>{res.name}</span>
                  {res.ok && res.org_note && <span style={{ fontSize: '11px', color: '#8a5a00' }}>{res.org_note}</span>}
                  {res.ok && res.kb_indexed === false &&
                    <span style={{ fontSize: '11px', color: '#8a5a00' }}>not indexed — {res.kb_reason || 'knowledge service unavailable'}</span>}
                  {!res.ok && <span style={{ color: '#c62828' }}>{res.error || 'failed'}</span>}
                </div>
              ))}
            </div>
          )}
          {cloneNotes.length > 0 && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '3px' }}>
              {cloneNotes.map(n => <div key={n} style={{ fontSize: '12px', color: '#8a5a00' }}>{n}</div>)}
            </div>
          )}
        </div>
      </Panel>

      <Panel title="Git accounts" count={hosts.length}>
        <div style={{ padding: '12px', display: 'flex', flexDirection: 'column', gap: '10px' }}>
          <div style={{ color: '#888', fontSize: '12px' }}>
            Access tokens aimee-server uses to reach each git host (one per host, any provider). Tokens are stored server-side and never shown again.
          </div>
          <div style={{ display: 'flex', alignItems: 'center', gap: '10px', flexWrap: 'wrap' }}>
            <button style={{ ...btn, background: ghConfigured ? '#24292e' : '#888', color: '#fff', borderColor: '#24292e' }}
              disabled={busy || !!ghCode || !ghConfigured} onClick={startGithub}
              title="Authorize aimee via GitHub device flow and store the token server-side.">Sign in with GitHub</button>
            {ghCode && (
              <span style={{ fontSize: '13px', color: '#444' }}>
                Go to <a href={ghUri} target="_blank" rel="noreferrer">{ghUri}</a> and enter code{' '}
                <code style={{ background: '#eee', padding: '2px 6px', borderRadius: '4px', fontWeight: 600 }}>{ghCode}</code>
                <span style={{ color: '#888' }}> — waiting…</span>
              </span>
            )}
          </div>
          <details style={{ fontSize: '12px', color: '#666' }} open={!ghConfigured}>
            <summary style={{ cursor: 'pointer' }}>
              GitHub OAuth App {ghConfigured ? '(configured ✓ — click to change)' : '— set this to enable the button'}
            </summary>
            <div style={{ marginTop: '6px' }}>
              <div style={{ marginBottom: '4px' }}>
                Create a GitHub OAuth App (<a href="https://github.com/settings/developers" target="_blank" rel="noreferrer">github.com/settings/developers</a> → New OAuth App → enable <b>Device flow</b>), then paste its <b>Client ID</b> here:
              </div>
              <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap', alignItems: 'center' }}>
                <input style={{ ...input, flex: 1, minWidth: '200px' }} placeholder="GitHub OAuth App Client ID (e.g. Iv1.xxxxxxxx)"
                  value={ghClientId} onChange={e => setGhClientId(e.target.value)} />
                <button style={btn} disabled={busy || !ghClientId.trim()} onClick={saveGhConfig}
                  title="Save the GitHub OAuth App Client ID that enables device-flow sign-in.">Save</button>
              </div>
            </div>
          </details>
          {hosts.length > 0 && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
              {hosts.map(h => (
                <div key={h} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                  <span style={{ fontSize: '13px', fontFamily: 'monospace', flex: 1 }}>{h}</span>
                  <span style={{ fontSize: '11px', color: '#2a7' }}>● token set</span>
                  <button style={{ ...btn, borderColor: '#d99', color: '#c33' }} disabled={busy}
                    title="Delete the stored access token for this git host."
                    onClick={() => removeCred(h)}>remove</button>
                </div>
              ))}
            </div>
          )}
          <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap', alignItems: 'center' }}>
            <input style={{ ...input, flex: 1, minWidth: '160px' }} placeholder="host (e.g. github.com, gitea.you.com)"
              value={credHost} onChange={e => setCredHost(e.target.value)} />
            <input style={{ ...input, flex: 2, minWidth: '200px' }} type="password" autoComplete="off"
              placeholder="access token" value={credToken} onChange={e => setCredToken(e.target.value)} />
            <button style={{ ...btn, background: '#234', color: '#8cf', borderColor: '#456' }}
              disabled={busy || !credHost.trim() || !credToken.trim()} onClick={addCred}
              title="Store this access token server-side for the given host.">Save</button>
          </div>
          <details style={{ marginTop: '10px', fontSize: '12px', color: '#aaa' }}>
            <summary style={{ cursor: 'pointer' }}>SSH private key (for git over SSH)</summary>
            <div style={{ marginTop: '6px' }}>
              <div style={{ marginBottom: '6px' }}>
                Paste an <b>unencrypted</b> OpenSSH/PEM private key (no passphrase). It is sealed
                server-side in your encrypted vault and never shown again — no vault unlock needed.
              </div>
              <textarea style={{ ...input, width: '100%', minHeight: '110px', fontFamily: 'monospace' }}
                placeholder={'-----BEGIN OPENSSH PRIVATE KEY-----\n…\n-----END OPENSSH PRIVATE KEY-----'}
                value={credSSHKey} onChange={e => setCredSSHKey(e.target.value)} />
              <div style={{ display: 'flex', gap: '8px', marginTop: '6px', flexWrap: 'wrap' }}>
                <button style={{ ...btn, background: '#234', color: '#8cf', borderColor: '#456' }}
                  disabled={busy || !credSSHKey.trim()} onClick={addSSHKey}
                  title="Seal this private key in the server vault for git-over-SSH.">Save SSH key</button>
                <button style={{ ...btn, borderColor: '#d99', color: '#c33' }} disabled={busy}
                  title="Remove the stored SSH private key from the server vault."
                  onClick={removeSSHKey}>Clear SSH key</button>
              </div>
            </div>
          </details>
        </div>
      </Panel>

      <Panel title="Projects" count={projects.length}>
        <div style={{ padding: '12px', display: 'flex', flexDirection: 'column', gap: '10px' }}>
          {projects.length === 0 ? (
            <div style={{ color: '#888', fontSize: '13px' }}>No projects yet — connect a repository above.</div>
          ) : (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '8px' }}>
              {groupProjectsByOrg(projects, details).map(g => (
                <div key={g.org || '(ungrouped)'} style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
                  <div style={{ fontSize: '11px', fontWeight: 700, color: '#888', textTransform: 'uppercase', letterSpacing: '0.5px' }}>
                    {g.org || 'ungrouped'}
                  </div>
                  <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap' }}>
                    {g.refs.map(p => (
                      <span key={p} style={{ display: 'inline-flex' }}>
                        <button onClick={() => { setSelected(p); setOutput(''); setErr(''); }}
                          title="Select this project to run git operations on it."
                          style={{ ...btn, background: p === selected ? '#234' : '#fff', color: p === selected ? '#8cf' : '#444',
                                   borderColor: p === selected ? '#456' : '#ccc', borderRadius: '4px 0 0 4px' }}>
                          {p}
                        </button>
                        <button title={`Delete ${p}`} disabled={busy} onClick={() => openDelete(p)}
                          style={{ ...btn, borderColor: '#d99', color: '#c33', borderRadius: '0 4px 4px 0', borderLeft: 'none' }}>
                          ×
                        </button>
                      </span>
                    ))}
                  </div>
                </div>
              ))}
            </div>
          )}

          {delRef && (
            <div style={{ border: '1px solid #d99', borderRadius: '6px', padding: '10px', background: '#fff7f7',
                          display: 'flex', flexDirection: 'column', gap: '8px' }}>
              <div style={{ fontSize: '13px', fontWeight: 600, color: '#c33' }}>Delete {delRef}?</div>
              <div style={{ fontSize: '12px', color: '#844' }}>
                This removes the clone and all indexed knowledge. Type{' '}
                <code style={{ background: '#fee', padding: '1px 5px', borderRadius: '4px' }}>{delRef}</code> to confirm.
              </div>
              <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap', alignItems: 'center' }}>
                <input style={{ ...input, flex: 1, minWidth: '180px', fontFamily: 'monospace' }} placeholder={delRef}
                  value={delTyped} onChange={e => setDelTyped(e.target.value)} />
                {!delKbDown && (
                  <button style={{ ...btn, background: '#c33', color: '#fff', borderColor: '#a22' }}
                    disabled={busy || delTyped !== delRef} onClick={() => deleteProject(false)}
                    title="Permanently delete the clone and all indexed knowledge for this project.">Delete</button>
                )}
                <button style={btn} disabled={busy} onClick={closeDelete}>Cancel</button>
              </div>
              {delKbDown && (
                <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap', alignItems: 'center' }}>
                  <button style={btn} disabled={busy || delTyped !== delRef}
                    title="Retry the delete now that the knowledge service may be back."
                    onClick={() => deleteProject(false)}>Retry</button>
                  <button style={{ ...btn, borderColor: '#d99', color: '#c33' }} disabled={busy || delTyped !== delRef}
                    onClick={() => { if (delForceArmed) deleteProject(true); else setDelForceArmed(true); }}>
                    {delForceArmed
                      ? 'Click again to confirm force delete'
                      : 'Force delete (leaves knowledge orphaned until the knowledge service returns)'}
                  </button>
                </div>
              )}
            </div>
          )}

          {selected && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '8px' }}>
              <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap', alignItems: 'center' }}>
                {READ_OPS.map(op => (
                  <button key={op} style={btn} disabled={busy} title={OP_HELP[op]} onClick={() => runOp(op)}>{op}</button>
                ))}
                {REMOTE_OPS.map(op => (
                  <button key={op} style={{ ...btn, borderColor: '#a96' }} disabled={busy} title={OP_HELP[op]} onClick={() => runOp(op)}>{op}</button>
                ))}
              </div>
              <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap', alignItems: 'center' }}>
                <input style={{ ...input, flex: 1, minWidth: '180px' }} placeholder="commit message"
                  value={commitMsg} onChange={e => setCommitMsg(e.target.value)} />
                <button style={btn} disabled={busy || !commitMsg.trim()}
                  title="Commit staged changes with the entered message."
                  onClick={() => { runOp('commit', { message: commitMsg }); setCommitMsg(''); }}>commit</button>
                <input style={{ ...input, width: '140px' }} placeholder="branch"
                  value={branch} onChange={e => setBranch(e.target.value)} />
                <button style={btn} disabled={busy || !branch.trim()}
                  title="Switch to (or create) the named branch."
                  onClick={() => runOp('checkout', { branch })}>checkout</button>
              </div>
              <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap', alignItems: 'center' }}>
                <input style={{ ...input, flex: 1, minWidth: '180px' }}
                  placeholder="PR title (optional — empty fills from commits)"
                  value={prTitle} onChange={e => setPrTitle(e.target.value)} />
                <button style={{ ...btn, borderColor: '#7a7' }} disabled={busy}
                  title="Open a GitHub pull request for the pushed branch"
                  onClick={async () => { if (await runOp('pr', { message: prTitle })) setPrTitle(''); }}>open PR</button>
              </div>
            </div>
          )}

          {err && <div style={{ color: '#c62828', fontSize: '12px' }}>{err}</div>}
          {notice && <div style={{ color: '#2a7', fontSize: '12px' }}>{notice}</div>}
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
