import { useEffect, useState } from 'react';
import ProjectPicker from '../components/ProjectPicker';
import { useSessions } from '../SessionContext';

/* In-app VSCode, bound to the active SESSION's project. The per-user code-server
 * (served by aimee-server, reverse-proxied at /vscode/) is rooted at the user's
 * workspace. To stay consistent with the agent — which works in the session's
 * isolated worktree (a sibling on aimee/session/<sid> off the default branch) —
 * the editor opens that same worktree via ?folder=<dir>, resolved server-side
 * (/api/git/session-dir). Before the session has an aimee id (no turn yet) it
 * falls back to the project checkout. The browser never sees the editor's
 * loopback port or any git credential — creds stay server-side in the vault. */
async function api(path: string, init?: RequestInit): Promise<Response> {
  return fetch(path, {
    ...init,
    headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '', ...(init?.headers || {}) },
  });
}

export default function Editor() {
  const { active, patchSession } = useSessions();
  const projectRoot = active?.projectRoot || '';
  const aimeeSid = active?.aimeeSid || '';
  const binding = JSON.stringify([projectRoot, aimeeSid]);
  const [workspace, setWorkspace] = useState({ binding: '', folder: '', error: '' });
  const folder = workspace.binding === binding ? workspace.folder : '';
  const error = workspace.binding === binding ? workspace.error : '';

  // Hydrated tab names may be display basenames. Resolve the full registered
  // identity from the authenticated owner's registry, never by stripping an
  // owner or trusting a client-supplied checkout path as workspace authority.
  useEffect(() => {
    let cancelled = false;
    setWorkspace({ binding, folder: '', error: '' });
    if (!projectRoot) return;
    void (async () => {
      try {
        const response = await api('/api/git/projects');
        if (!response.ok) throw new Error('registry unavailable');
        const registry = await response.json();
        const project = typeof registry.root === 'string' && Array.isArray(registry.projects)
          ? registry.projects.find((name: unknown) => typeof name === 'string' && `${registry.root}/${name}` === projectRoot)
          : undefined;
        if (!project) throw new Error('project not registered');
        if (cancelled) return;
        let dir = projectRoot;
        if (aimeeSid) {
          const resolved = await api('/api/git/session-dir', {
            method: 'POST', body: JSON.stringify({ project, session_id: aimeeSid }),
          });
          if (!resolved.ok) throw new Error('session workspace unavailable');
          const data = await resolved.json();
          if (typeof data.dir !== 'string' || !data.dir) throw new Error('missing session workspace');
          dir = data.dir;
        }
        if (!cancelled) setWorkspace({ binding, folder: dir, error: '' });
      } catch {
        if (!cancelled) setWorkspace({ binding, folder: '', error: 'Could not resolve this session’s editor workspace.' });
      }
    })();
    return () => { cancelled = true; };
  }, [projectRoot, aimeeSid, binding]);

  // Re-key the iframe on folder change so code-server reloads at the new folder.
  const src = folder ? `/vscode/?folder=${encodeURIComponent(folder)}` : '/vscode/';

  return (
    <div style={{ height: '100%', display: 'flex', flexDirection: 'column' }}>
      <ProjectPicker
        key={active?.id}
        value={active?.projectRoot ?? ''}
        onChange={sel => {
          const r = sel ? `${sel.root}/${sel.project}` : '';
          if (active) patchSession(active.id, { projectRoot: r, projectName: sel?.project ?? '' });
        }}
      />
      {error ? <div role="alert" style={{ padding: 16 }}>{error}</div> : folder ? (
        <iframe
          key={folder}
          title="VSCode"
          src={src}
          style={{ flex: 1, width: '100%', height: '100%', border: 'none' }}
          sandbox="allow-scripts allow-same-origin allow-forms allow-popups allow-modals allow-downloads"
          allow="clipboard-read; clipboard-write"
        />
      ) : (
        <div style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center', color: 'var(--sg-text-faint)', fontFamily: 'system-ui' }}>
          {projectRoot ? 'Resolving editor workspace…' : 'Select or clone a project for this session to open it in the editor.'}
        </div>
      )}
    </div>
  );
}
