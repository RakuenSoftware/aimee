/* In-app VSCode (webchat-git WP-J). Embeds the per-user code-server — served by
 * aimee-server and reverse-proxied at /vscode/ (same origin) — in an iframe so
 * the editor lives inside the webchat shell alongside Chat/Projects. The browser
 * never sees the editor's loopback port or any git credential: webchat ensures
 * the editor and proxies, and the creds stay server-side in the sealed vault.
 *
 * The editor is server-gated (AIMEE_WEBCHAT_EDITOR + a code-server build); when
 * it is off, /vscode/ returns 503 and the iframe shows that page. Same-origin,
 * so framing is allowed and no cross-origin credential exposure is possible. */
export default function Editor() {
  return (
    <div style={{ height: '100%', display: 'flex', flexDirection: 'column' }}>
      <iframe
        title="VSCode"
        src="/vscode/"
        style={{ flex: 1, width: '100%', height: '100%', border: 'none' }}
        /* code-server needs scripts + same-origin; it is served from our own
         * origin so this is not a cross-origin grant. Clipboard helps copy/paste
         * in the editor + terminal. */
        sandbox="allow-scripts allow-same-origin allow-forms allow-popups allow-modals allow-downloads"
        allow="clipboard-read; clipboard-write"
      />
    </div>
  );
}
