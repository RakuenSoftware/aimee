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
  const projectName = active?.projectName || '';
  const aimeeSid = active?.aimeeSid || '';
  const [folder, setFolder] = useState('');

  // Resolve the folder to open: the session's worktree when it has one, else the
  // project checkout. Re-resolves when the bound project or session id changes.
  useEffect(() => {
    let cancelled = false;
    if (!projectRoot) { setFolder(''); return; }
    setFolder(projectRoot); // immediate fallback while the worktree resolves
    if (projectName && aimeeSid) {
      api('/api/git/session-dir', {
        method: 'POST',
        body: JSON.stringify({ project: projectName, session_id: aimeeSid }),
      })
        .then(r => (r.ok ? r.json() : null))
        .then(d => { if (!cancelled && d && d.dir) setFolder(d.dir); })
        .catch(() => { /* keep the project-checkout fallback */ });
    }
    return () => { cancelled = true; };
  }, [projectRoot, projectName, aimeeSid]);

  // Re-key the iframe on folder change so code-server reloads at the new folder.
  const src = folder ? `/vscode/?folder=${encodeURIComponent(folder)}` : '/vscode/';

  return (
    <div style={{ height: '100%', display: 'flex', flexDirection: 'column' }}>
      <ProjectPicker
        key={active?.id}
        storageKey={`aimee_session_project_${active?.id ?? ''}`}
        onChange={sel => {
          const r = sel ? `${sel.root}/${sel.project}` : '';
          if (active) patchSession(active.id, { projectRoot: r, projectName: sel?.project ?? '' });
        }}
      />
      {folder ? (
        <iframe
          key={folder}
          title="VSCode"
          src={src}
          style={{ flex: 1, width: '100%', height: '100%', border: 'none' }}
          sandbox="allow-scripts allow-same-origin allow-forms allow-popups allow-modals allow-downloads"
          allow="clipboard-read; clipboard-write"
        />
      ) : (
        <div style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center', color: '#888', fontFamily: 'system-ui' }}>
          Select or clone a project for this session to open it in the editor.
        </div>
      )}
    </div>
  );
}
