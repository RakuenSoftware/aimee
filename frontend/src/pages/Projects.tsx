import { useEffect, useState, useCallback } from 'react';
import { Panel, Button } from '@rakuensoftware/smoothgui';
import ConnectHosts from '../setup/ConnectHosts';
import { useSessions } from '../SessionContext';
import { groupProjectsByOrg, parseOwnerOnly, repoAlreadyCloned, type CloneKbAnnotations, type GitProjectsResponse, type OwnerRef, type ProjectDetail, type ProjectDeleteResponse } from '../setup/ownerUrl';
import { notifySetupUpdated } from '../setup/setupState';

/* Git projects (webchat-git WP-F2). The page is focused on managing repos: it
 * lists the user's cloned projects, connects new ones, and runs per-project git
 * ops — all via /api/git/* (the server forwards to /v1/workspace/* with the
 * user's webuser: identity; creds live only in the sealed vault). The connect
 * field also accepts an owner/org URL (e.g. github.com/RakuenSoftware): it then
 * enumerates the owner's repositories and bulk-clones the selected ones,
 * mirroring the wizard's Workspaces step.
 *
 * Connecting a git ACCOUNT (OAuth / token / SSH key) is a one-click launch of
 * the shared wizard flow (ConnectHosts) in a modal — the same UI as onboarding,
 * so there is a single source of truth for host auth rather than a duplicate. */

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

const input: React.CSSProperties = {
  padding: '6px 8px', borderRadius: '4px', border: '1px solid var(--sg-border-medium)', fontSize: '13px',
};
const modalBackdrop: React.CSSProperties = {
  position: 'fixed', inset: 0, zIndex: 1000, background: 'rgba(10,10,18,0.55)',
  display: 'flex', alignItems: 'center', justifyContent: 'center', padding: '16px',
};
const modalCard: React.CSSProperties = {
  width: 'min(560px, 100%)', maxHeight: '86vh', overflow: 'auto', background: 'var(--sg-surface)',
  borderRadius: '12px', border: '1px solid var(--sg-border-medium)', boxShadow: '0 12px 40px rgba(0,0,0,0.3)',
  padding: '20px 22px', fontFamily: 'system-ui', color: 'var(--sg-text)',
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
  // The "Connect git account" modal — runs the shared wizard auth flow.
  const [connectOpen, setConnectOpen] = useState(false);
  // Owner/org bulk-clone (URL parsed as an owner with no repo segment).
  const [orgRef, setOrgRef] = useState<OwnerRef | null>(null);
  const [orgRepos, setOrgRepos] = useState<{ name: string; clone_url: string; private?: boolean }[]>([]);
  const [orgSelected, setOrgSelected] = useState<Record<string, boolean>>({});
  const [orgProvider, setOrgProvider] = useState('');
  const [orgResults, setOrgResults] = useState<({ name: string; ok: boolean; error?: string | null } & CloneKbAnnotations)[]>([]);
  // Post-clone notices (org placement, kb indexing) for the single-repo form.
  const [cloneNotes, setCloneNotes] = useState<string[]>([]);
  // Delete flow: the ref pending confirmation and the typed-ref gate. The
  // delete is local to this environment — it removes the clone, and never
  // touches aimee-kb — so there is no knowledge-service failure to force past.
  const [delRef, setDelRef] = useState('');
  const [delTyped, setDelTyped] = useState('');
  const [notice, setNotice] = useState('');

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

  useEffect(() => { loadProjects(); loadHosts(); }, [loadProjects, loadHosts]);

  // Close the Connect-account modal on Escape (works regardless of focus).
  useEffect(() => {
    if (!connectOpen) return;
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') setConnectOpen(false); };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [connectOpen]);

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
        notifySetupUpdated();
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
      notifySetupUpdated();
    } catch { setErr('aimee-server unavailable'); } finally { setBusy(false); }
  }

  function openDelete(ref: string) {
    setDelRef(ref); setDelTyped(''); setErr(''); setNotice('');
  }

  function closeDelete() {
    setDelRef(''); setDelTyped('');
  }

  async function deleteProject() {
    if (!delRef || delTyped !== delRef) return;
    setBusy(true); setErr(''); setNotice('');
    try {
      const r = await api('/api/git/projects/delete', {
        method: 'POST',
        body: JSON.stringify({ ref: delRef }),
      });
      const d: ProjectDeleteResponse = await r.json().catch(() => ({}));
      if (r.ok) {
        setNotice(`Deleted ${delRef}.`);
        closeDelete();
        await loadProjects();
        notifySetupUpdated();
      } else {
        setErr(d.error || `delete failed (${r.status})`);
      }
    } catch { setErr('aimee-server unavailable'); } finally { setBusy(false); }
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
            <Button variant="primary" size="sm"
              disabled={busy || !url.trim()} onClick={connect}
              title="Clone the repository, or if you entered an owner/org URL, list its repositories to bulk-clone.">
              {parseOwnerOnly(url) ? 'List repositories' : 'Clone'}
            </Button>
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
                <Button variant="ghost" size="sm" style={{ color: 'var(--sg-info)', padding: 0 }}
                  title="Select every repository that is not already cloned."
                  onClick={() => setOrgSelected(Object.fromEntries(orgRepos.map(r =>
                    [r.name, !repoAlreadyCloned(r, orgRef.owner, projects, details)])))}>all</Button>
                <Button variant="ghost" size="sm" style={{ color: 'var(--sg-info)', padding: 0 }}
                  title="Deselect all repositories."
                  onClick={() => setOrgSelected({})}>none</Button>
              </div>
              <div style={{ display: 'flex', flexDirection: 'column', gap: '2px', maxHeight: '220px', overflow: 'auto',
                            border: '1px solid var(--sg-border-medium)', borderRadius: '6px', padding: '8px' }}>
                {orgRepos.map(repo => {
                  const already = repoAlreadyCloned(repo, orgRef.owner, projects, details);
                  return (
                    <label key={repo.name} style={{ display: 'flex', alignItems: 'center', gap: '8px', fontSize: '13px', cursor: already ? 'default' : 'pointer' }}>
                      <input type="checkbox" disabled={already} checked={!already && !!orgSelected[repo.name]}
                        onChange={e => setOrgSelected(p => ({ ...p, [repo.name]: e.target.checked }))} />
                      <span style={{ fontFamily: 'monospace', flex: 1, color: already ? 'var(--sg-text-hint)' : undefined }}>{repo.name}</span>
                      {repo.private && <span style={{ fontSize: '10.5px', color: 'var(--sg-warning-dark)' }}>private</span>}
                      {already && <span style={{ fontSize: '11px', color: 'var(--sg-success-dark)' }}>cloned</span>}
                    </label>
                  );
                })}
              </div>
              <div>
                <Button variant="primary" size="sm"
                  disabled={busy || orgRepos.filter(r => orgSelected[r.name]).length === 0}
                  title="Clone the checked repositories as new projects."
                  onClick={cloneOrgSelected}>
                  Clone selected ({orgRepos.filter(r => orgSelected[r.name]).length})
                </Button>
              </div>
            </div>
          )}
          {orgResults.length > 0 && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '3px' }}>
              {orgResults.map(res => (
                <div key={res.name} style={{ display: 'flex', alignItems: 'center', gap: '8px', fontSize: '12.5px' }}>
                  <span aria-hidden>{res.ok ? '✅' : '⛔'}</span>
                  <span style={{ fontFamily: 'monospace', flex: 1 }}>{res.name}</span>
                  {res.ok && res.org_note && <span style={{ fontSize: '11px', color: 'var(--sg-warning-dark)' }}>{res.org_note}</span>}
                  {res.ok && res.kb_indexed === false &&
                    <span style={{ fontSize: '11px', color: 'var(--sg-warning-dark)' }}>not indexed — {res.kb_reason || 'knowledge service unavailable'}</span>}
                  {!res.ok && <span style={{ color: 'var(--sg-danger-dark)' }}>{res.error || 'failed'}</span>}
                </div>
              ))}
            </div>
          )}
          {cloneNotes.length > 0 && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '3px' }}>
              {cloneNotes.map(n => <div key={n} style={{ fontSize: '12px', color: 'var(--sg-warning-dark)' }}>{n}</div>)}
            </div>
          )}
        </div>
      </Panel>

      <Panel title="Git accounts" count={hosts.length}>
        <div style={{ padding: '12px', display: 'flex', flexDirection: 'column', gap: '10px' }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: '10px', flexWrap: 'wrap' }}>
            <Button variant="primary" size="sm"
              title="Connect a git account (OAuth, access token, or SSH key) via the setup wizard."
              onClick={() => setConnectOpen(true)}>+ Connect git account</Button>
            <span style={{ color: 'var(--sg-text-faint)', fontSize: '12px' }}>
              OAuth sign-in, an access token, or an SSH key — stored server-side, never shown again.
            </span>
          </div>
          {hosts.length > 0 && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
              {hosts.map(h => (
                <div key={h} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                  <span style={{ fontSize: '13px', fontFamily: 'monospace', flex: 1 }}>{h}</span>
                  <span style={{ fontSize: '11px', color: 'var(--sg-success-dark)' }}>● token set</span>
                  <Button variant="danger" size="sm" disabled={busy}
                    title="Delete the stored access token for this git host."
                    onClick={() => removeCred(h)}>remove</Button>
                </div>
              ))}
            </div>
          )}
        </div>
      </Panel>

      <Panel title="Projects" count={projects.length}>
        <div style={{ padding: '12px', display: 'flex', flexDirection: 'column', gap: '10px' }}>
          {projects.length === 0 ? (
            <div style={{ color: 'var(--sg-text-faint)', fontSize: '13px' }}>No projects yet — connect a repository above.</div>
          ) : (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '8px' }}>
              {groupProjectsByOrg(projects, details).map(g => (
                <div key={g.org || '(ungrouped)'} style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
                  <div style={{ fontSize: '11px', fontWeight: 700, color: 'var(--sg-text-faint)', textTransform: 'uppercase', letterSpacing: '0.5px' }}>
                    {g.org || 'ungrouped'}
                  </div>
                  <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap' }}>
                    {g.refs.map(p => (
                      <span key={p} style={{ display: 'inline-flex' }}>
                        <Button onClick={() => { setSelected(p); setOutput(''); setErr(''); }}
                          variant={p === selected ? 'primary' : 'default'} size="sm"
                          title="Select this project to run git operations on it."
                          style={{ borderRadius: '4px 0 0 4px' }}>
                          {p}
                        </Button>
                        <Button variant="danger" size="sm" title={`Delete ${p}`} disabled={busy} onClick={() => openDelete(p)}
                          style={{ borderRadius: '0 4px 4px 0', borderLeft: 'none' }}>
                          ×
                        </Button>
                      </span>
                    ))}
                  </div>
                </div>
              ))}
            </div>
          )}

          {delRef && (
            <div style={{ border: '1px solid var(--sg-danger)', borderRadius: '6px', padding: '10px', background: 'var(--sg-danger-bg)',
                          display: 'flex', flexDirection: 'column', gap: '8px' }}>
              <div style={{ fontSize: '13px', fontWeight: 600, color: 'var(--sg-danger)' }}>Delete {delRef}?</div>
              <div style={{ fontSize: '12px', color: 'var(--sg-danger-dark)' }}>
                This removes the clone from this server. Type{' '}
                <code style={{ background: 'var(--sg-danger-bg)', padding: '1px 5px', borderRadius: '4px' }}>{delRef}</code> to confirm.
              </div>
              <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap', alignItems: 'center' }}>
                <input style={{ ...input, flex: 1, minWidth: '180px', fontFamily: 'monospace' }} placeholder={delRef}
                  value={delTyped} onChange={e => setDelTyped(e.target.value)} />
                <Button variant="danger" size="sm"
                  disabled={busy || delTyped !== delRef} onClick={() => deleteProject()}
                  title="Permanently delete this project's clone from this server.">Delete</Button>
                <Button size="sm" disabled={busy} onClick={closeDelete}>Cancel</Button>
              </div>
            </div>
          )}

          {selected && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '8px' }}>
              <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap', alignItems: 'center' }}>
                {READ_OPS.map(op => (
                  <Button key={op} size="sm" disabled={busy} title={OP_HELP[op]} onClick={() => runOp(op)}>{op}</Button>
                ))}
                {REMOTE_OPS.map(op => (
                  <Button key={op} size="sm" style={{ borderColor: 'var(--sg-warning-dark)' }} disabled={busy} title={OP_HELP[op]} onClick={() => runOp(op)}>{op}</Button>
                ))}
              </div>
              <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap', alignItems: 'center' }}>
                <input style={{ ...input, flex: 1, minWidth: '180px' }} placeholder="commit message"
                  value={commitMsg} onChange={e => setCommitMsg(e.target.value)} />
                <Button size="sm" disabled={busy || !commitMsg.trim()}
                  title="Commit staged changes with the entered message."
                  onClick={() => { runOp('commit', { message: commitMsg }); setCommitMsg(''); }}>commit</Button>
                <input style={{ ...input, width: '140px' }} placeholder="branch"
                  value={branch} onChange={e => setBranch(e.target.value)} />
                <Button size="sm" disabled={busy || !branch.trim()}
                  title="Switch to (or create) the named branch."
                  onClick={() => runOp('checkout', { branch })}>checkout</Button>
              </div>
              <div style={{ display: 'flex', gap: '6px', flexWrap: 'wrap', alignItems: 'center' }}>
                <input style={{ ...input, flex: 1, minWidth: '180px' }}
                  placeholder="PR title (optional — empty fills from commits)"
                  value={prTitle} onChange={e => setPrTitle(e.target.value)} />
                <Button size="sm" style={{ borderColor: 'var(--sg-success)' }} disabled={busy}
                  title="Open a GitHub pull request for the pushed branch"
                  onClick={async () => { if (await runOp('pr', { message: prTitle })) setPrTitle(''); }}>open PR</Button>
              </div>
            </div>
          )}

          {err && <div style={{ color: 'var(--sg-danger-dark)', fontSize: '12px' }}>{err}</div>}
          {notice && <div style={{ color: 'var(--sg-success-dark)', fontSize: '12px' }}>{notice}</div>}
          {output && (
            <pre style={{ fontSize: '12px', background: 'var(--sg-dark-surface)', color: 'var(--sg-border-medium)', padding: '10px',
                          borderRadius: '6px', overflow: 'auto', maxHeight: '320px', whiteSpace: 'pre-wrap' }}>
              {output}
            </pre>
          )}
          {busy && <div style={{ color: 'var(--sg-text-faint)', fontSize: '12px' }}>working…</div>}
        </div>
      </Panel>

      {/* Connect a git account: the shared wizard auth flow, one click away. */}
      {connectOpen && (
        <div style={modalBackdrop} onClick={() => setConnectOpen(false)}>
          <div style={modalCard} role="dialog" aria-modal="true" aria-label="Connect git account" onClick={e => e.stopPropagation()}>
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: '10px' }}>
              <strong style={{ fontSize: '17px' }}>Connect a git account</strong>
              <button aria-label="Close" title="Close" onClick={() => setConnectOpen(false)}
                style={{ background: 'none', border: 'none', color: 'var(--sg-text-hint)', cursor: 'pointer', fontSize: '20px', lineHeight: 1 }}>×</button>
            </div>
            <ConnectHosts
              doneLabel="Done"
              onDone={() => setConnectOpen(false)}
              onHostsChanged={() => { loadHosts(); }}
            />
          </div>
        </div>
      )}
    </div>
  );
}
