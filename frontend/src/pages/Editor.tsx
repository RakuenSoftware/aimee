import { useState } from 'react';
import ProjectPicker from '../components/ProjectPicker';
import type { ProjectSelection } from '../components/ProjectPicker';

/* In-app VSCode (webchat-git WP-J), bound to this tab's selected project. The
 * per-user code-server (served by aimee-server, reverse-proxied at /vscode/) is
 * rooted at the user's workspace; selecting a project opens its folder via
 * ?folder=<root>/<project>. The browser never sees the editor's loopback port or
 * any git credential — creds stay server-side in the sealed vault. */
export default function Editor() {
  const [sel, setSel] = useState<ProjectSelection | null>(null);

  const folder = sel ? `${sel.root}/${sel.project}` : '';
  // Re-key the iframe on folder change so code-server reloads at the new folder.
  const src = folder ? `/vscode/?folder=${encodeURIComponent(folder)}` : '/vscode/';

  return (
    <div style={{ height: '100%', display: 'flex', flexDirection: 'column' }}>
      <ProjectPicker storageKey="aimee_editor_project" onChange={setSel} />
      {sel ? (
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
          Select or clone a project above to open it in the editor.
        </div>
      )}
    </div>
  );
}
