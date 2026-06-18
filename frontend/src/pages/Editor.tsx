import ProjectPicker from '../components/ProjectPicker';
import { useSessions } from '../SessionContext';

/* In-app VSCode, bound to the active SESSION's project. The per-user code-server
 * (served by aimee-server, reverse-proxied at /vscode/) is rooted at the user's
 * workspace; the session's project opens its folder via ?folder=<root>/<project>.
 * The browser never sees the editor's loopback port or any git credential — creds
 * stay server-side in the sealed vault. Picking a project here binds it to the
 * session, so Chat/Workflows see the same project. */
export default function Editor() {
  const { active, patchSession } = useSessions();
  const folder = active?.projectRoot || '';
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
