import { useCallback, useEffect, useState } from 'react';

/* Wizard — Connection (git hosts). Authenticate aimee-server to the git hosts it
 * will clone from, so the next step (Workspaces & projects) can enumerate + clone
 * private repos. Three ways in, all via the /api/git/* routes (which forward to
 * aimee-server's sealed vault — tokens/keys are write-only, never read back):
 *
 *  • OAuth device flow — GitHub, GitLab, or Gitea/Forgejo (needs an OAuth App
 *    client ID for the chosen provider/host; the client ID is public).
 *  • Per-host access token — any host/provider (incl. Bitbucket).
 *  • SSH private key — for git over SSH.
 *
 * Optional step: public repos clone without any of this. GitHub uses its dedicated
 * /api/git/oauth/github/* routes; GitLab/Gitea use the generic
 * /api/git/oauth/device/* routes with a {provider, host} selector. */

type OAuthProvider = 'github' | 'gitlab' | 'gitea';

interface ProviderMeta {
  label: string;
  needsHost: boolean;
  defaultHost: string;
  appHelp: React.ReactNode;
}

const PROVIDERS: Record<OAuthProvider, ProviderMeta> = {
  github: {
    label: 'GitHub',
    needsHost: false,
    defaultHost: 'github.com',
    appHelp: (
      <>
        Create a GitHub OAuth App (
        <a href="https://github.com/settings/developers" target="_blank" rel="noreferrer">github.com/settings/developers</a>
        {' '}→ New OAuth App → enable <b>Device flow</b>), then paste its <b>Client ID</b>.
      </>
    ),
  },
  gitlab: {
    label: 'GitLab',
    needsHost: true,
    defaultHost: 'gitlab.com',
    appHelp: (
      <>
        Create a GitLab application (User Settings → Applications, or Admin → Applications) with the{' '}
        <b>read_api</b> + <b>read_repository</b> scopes and no redirect URI, then paste its{' '}
        <b>Application ID</b>.
      </>
    ),
  },
  gitea: {
    label: 'Gitea / Forgejo',
    needsHost: true,
    defaultHost: '',
    appHelp: (
      <>
        Create an OAuth2 application (Settings → Applications) on your Gitea/Forgejo instance, then
        paste its <b>Client ID</b>. Requires Gitea ≥ 1.19 (device flow support).
      </>
    ),
  },
};

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

export interface ConnectHostsProps {
  /** Continue to the next wizard step. */
  onDone: () => void;
  /** Called whenever the connected-host set changes, with the new count (lets the
   * wizard refresh its readiness/summary without a reload). */
  onHostsChanged?: (count: number) => void;
}

interface Pending {
  code: string;
  uri: string;
  provider: OAuthProvider;
  host: string;
}

export default function ConnectHosts({ onDone, onHostsChanged }: ConnectHostsProps) {
  const [hosts, setHosts] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState('');
  const [msg, setMsg] = useState('');

  const [credHost, setCredHost] = useState('');
  const [credToken, setCredToken] = useState('');
  const [sshKey, setSshKey] = useState('');

  // OAuth (device flow) — provider-generic.
  const [provider, setProvider] = useState<OAuthProvider>('github');
  const [oauthHost, setOauthHost] = useState('');
  const [clientId, setClientId] = useState('');
  const [configured, setConfigured] = useState(false);
  const [pending, setPending] = useState<Pending | null>(null);

  const meta = PROVIDERS[provider];
  // The host this provider's flow targets (github ignores host; gitlab defaults).
  const effHost = provider === 'github' ? '' : oauthHost.trim() || meta.defaultHost;

  const loadHosts = useCallback(async () => {
    try {
      const r = await api('/api/git/credentials', { method: 'GET' });
      const d = await r.json();
      if (r.ok) {
        const hs: string[] = d.hosts || [];
        setHosts(hs);
        onHostsChanged?.(hs.length);
      }
    } catch {
      /* server unavailable — leave list empty */
    }
  }, [onHostsChanged]);

  // Config (client ID) for the current provider/host. GitHub has its own route;
  // GitLab/Gitea share the device route with a provider+host query.
  const loadConfig = useCallback(async () => {
    setConfigured(false);
    setClientId('');
    try {
      const path =
        provider === 'github'
          ? '/api/git/oauth/github/config'
          : `/api/git/oauth/device/config?provider=${provider}&host=${encodeURIComponent(effHost)}`;
      const r = await api(path, { method: 'GET' });
      const d = await r.json();
      if (r.ok) {
        setConfigured(!!d.configured);
        setClientId(d.client_id || '');
      }
    } catch {
      /* leave unconfigured */
    }
  }, [provider, effHost]);

  useEffect(() => {
    loadHosts();
  }, [loadHosts]);
  useEffect(() => {
    loadConfig();
  }, [loadConfig]);

  // Device-flow poll: once a user code is shown, poll the right endpoint until the
  // user authorizes (or it errors).
  useEffect(() => {
    if (!pending) return;
    let alive = true;
    const id = setInterval(async () => {
      try {
        const r =
          pending.provider === 'github'
            ? await api('/api/git/oauth/github/poll', { method: 'POST' })
            : await api('/api/git/oauth/device/poll', {
                method: 'POST',
                body: JSON.stringify({ provider: pending.provider, host: pending.host }),
              });
        const d = await r.json();
        if (!alive) return;
        if (d.status === 'done') {
          setPending(null);
          setMsg(`${PROVIDERS[pending.provider].label} connected.`);
          await loadHosts();
        } else if (d.status === 'error') {
          setPending(null);
          setErr(d.error || 'sign-in failed');
        }
      } catch {
        /* transient; keep polling */
      }
    }, 5000);
    return () => {
      alive = false;
      clearInterval(id);
    };
  }, [pending, loadHosts]);

  async function saveConfig() {
    if (!clientId.trim()) return;
    setBusy(true);
    setErr('');
    try {
      const r =
        provider === 'github'
          ? await api('/api/git/oauth/github/config', {
              method: 'POST',
              body: JSON.stringify({ client_id: clientId.trim() }),
            })
          : await api('/api/git/oauth/device/config', {
              method: 'POST',
              body: JSON.stringify({ provider, host: effHost, client_id: clientId.trim() }),
            });
      const d = await r.json();
      if (!r.ok) setErr(d.error || 'could not save client ID');
      else await loadConfig();
    } finally {
      setBusy(false);
    }
  }

  async function startSignIn() {
    setErr('');
    setMsg('');
    if (meta.needsHost && !effHost) {
      setErr('Enter the host for this provider.');
      return;
    }
    try {
      const r =
        provider === 'github'
          ? await api('/api/git/oauth/github/start', { method: 'POST' })
          : await api('/api/git/oauth/device/start', {
              method: 'POST',
              body: JSON.stringify({ provider, host: effHost }),
            });
      const d = await r.json();
      if (!r.ok) {
        setErr(d.error || 'sign-in unavailable');
        return;
      }
      setPending({
        code: d.user_code || '',
        uri: d.verification_uri || '',
        provider,
        host: effHost,
      });
    } catch {
      setErr('aimee-server unavailable');
    }
  }

  async function addCred() {
    if (!credHost.trim() || !credToken.trim()) return;
    setBusy(true);
    setErr('');
    setMsg('');
    try {
      const r = await api('/api/git/credentials', {
        method: 'POST',
        body: JSON.stringify({ host: credHost.trim(), token: credToken.trim() }),
      });
      const d = await r.json();
      if (!r.ok) setErr(d.error || 'could not save credential');
      else {
        setCredHost('');
        setCredToken('');
        setMsg('Token saved.');
        await loadHosts();
      }
    } finally {
      setBusy(false);
    }
  }

  async function removeCred(host: string) {
    setBusy(true);
    setErr('');
    try {
      const r = await api('/api/git/credentials', {
        method: 'DELETE',
        body: JSON.stringify({ host }),
      });
      if (!r.ok) {
        const d = await r.json().catch(() => ({}));
        setErr(d.error || 'could not remove credential');
      } else {
        await loadHosts();
      }
    } finally {
      setBusy(false);
    }
  }

  async function addSSHKey() {
    if (!sshKey.trim()) return;
    setBusy(true);
    setErr('');
    setMsg('');
    try {
      const r = await api('/api/git/sshkey', {
        method: 'POST',
        body: JSON.stringify({ ssh_key: sshKey }),
      });
      const d = await r.json().catch(() => ({}));
      // Always drop the key from component state once it has left the browser —
      // never leave private-key material sitting in React state / the textarea.
      setSshKey('');
      if (!r.ok) setErr(d.error || 'could not save SSH key');
      else setMsg('SSH key saved.');
    } finally {
      setBusy(false);
    }
  }

  return (
    <div style={{ display: 'grid', gap: 14, marginBottom: 8 }}>
      <div style={{ fontSize: 12.5, color: '#556', lineHeight: 1.5 }}>
        Connect the git hosts aimee should clone from. You can skip this and still clone public
        repositories — connect private hosts here or later. Tokens and keys are stored server-side
        and never shown again.
      </div>

      {/* OAuth device flow */}
      <section style={{ display: 'grid', gap: 8 }}>
        <div style={sectionTitle}>Sign in with OAuth</div>
        <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', alignItems: 'center' }}>
          <select style={{ ...input, width: 150 }} value={provider}
            onChange={(e) => { setProvider(e.target.value as OAuthProvider); setPending(null); setErr(''); setMsg(''); }}>
            {(Object.keys(PROVIDERS) as OAuthProvider[]).map((p) => (
              <option key={p} value={p}>{PROVIDERS[p].label}</option>
            ))}
          </select>
          {meta.needsHost && (
            <input style={{ ...input, flex: 1, minWidth: 160 }}
              placeholder={`host (e.g. ${meta.defaultHost || 'gitea.example.com'})`}
              value={oauthHost} onChange={(e) => setOauthHost(e.target.value)} />
          )}
          <button
            style={{ ...primaryBtn, background: configured ? '#2c8f56' : '#888', borderColor: configured ? '#2c6' : '#888' }}
            disabled={busy || !!pending || !configured}
            onClick={startSignIn}
          >
            Sign in
          </button>
        </div>
        {!configured && !pending && (
          <div style={{ fontSize: 11.5, color: '#a67c00' }}>
            No {meta.label} sign-in is configured on this server yet — set an OAuth App Client ID
            below to enable it.
          </div>
        )}
        {pending && (
          <div style={{ fontSize: 12.5, color: '#444' }}>
            Go to <a href={pending.uri} target="_blank" rel="noreferrer">{pending.uri}</a> and enter{' '}
            <code style={{ background: '#eee', padding: '2px 6px', borderRadius: 4, fontWeight: 600 }}>{pending.code}</code>
            <span style={{ color: '#888' }}> — waiting…</span>
          </div>
        )}
        <details style={{ fontSize: 12, color: '#666' }} open={!configured}>
          <summary style={{ cursor: 'pointer' }}>
            {meta.label} OAuth App {configured ? '(configured ✓ — click to change)' : '— set this to enable sign-in'}
          </summary>
          <div style={{ marginTop: 6 }}>
            <div style={{ marginBottom: 4 }}>{meta.appHelp}</div>
            <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', alignItems: 'center' }}>
              <input style={{ ...input, flex: 1, minWidth: 200 }} placeholder={`${meta.label} OAuth App Client ID`}
                value={clientId} onChange={(e) => setClientId(e.target.value)} />
              <button style={ghostBtn} disabled={busy || !clientId.trim() || (meta.needsHost && !effHost)}
                onClick={saveConfig}>Save</button>
            </div>
          </div>
        </details>
      </section>

      {/* Per-host access token */}
      <section style={{ display: 'grid', gap: 8 }}>
        <div style={sectionTitle}>Access token</div>
        <div style={{ fontSize: 11.5, color: '#778' }}>
          An HTTPS access token for any host/provider (GitHub, GitLab, Gitea, Bitbucket…). One per host.
        </div>
        <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', alignItems: 'center' }}>
          <input style={{ ...input, flex: 1, minWidth: 160 }} placeholder="host (e.g. github.com, gitlab.com)"
            value={credHost} onChange={(e) => setCredHost(e.target.value)} />
          <input style={{ ...input, flex: 2, minWidth: 200 }} type="password" autoComplete="off"
            placeholder="access token" value={credToken} onChange={(e) => setCredToken(e.target.value)} />
          <button style={ghostBtn} disabled={busy || !credHost.trim() || !credToken.trim()} onClick={addCred}>Save</button>
        </div>
        {hosts.length > 0 && (
          <div style={{ display: 'grid', gap: 4 }}>
            {hosts.map((h) => (
              <div key={h} style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                <span style={{ fontSize: 13, fontFamily: 'monospace', flex: 1 }}>{h}</span>
                <span style={{ fontSize: 11, color: '#2a7' }}>● token set</span>
                <button style={{ ...ghostBtn, borderColor: '#d99', color: '#c33' }} disabled={busy}
                  onClick={() => removeCred(h)}>remove</button>
              </div>
            ))}
          </div>
        )}
      </section>

      {/* SSH key */}
      <details style={{ fontSize: 12, color: '#666' }}>
        <summary style={{ cursor: 'pointer' }}>SSH private key (for git over SSH)</summary>
        <div style={{ marginTop: 6 }}>
          <div style={{ marginBottom: 6 }}>
            Paste an <b>unencrypted</b> OpenSSH/PEM private key (no passphrase). It is sealed
            server-side in your encrypted vault and never shown again.
          </div>
          <textarea style={{ ...input, width: '100%', minHeight: 100, fontFamily: 'monospace' }}
            placeholder={'-----BEGIN OPENSSH PRIVATE KEY-----\n…\n-----END OPENSSH PRIVATE KEY-----'}
            value={sshKey} onChange={(e) => setSshKey(e.target.value)} />
          <div style={{ marginTop: 6 }}>
            <button style={ghostBtn} disabled={busy || !sshKey.trim()} onClick={addSSHKey}>Save SSH key</button>
          </div>
        </div>
      </details>

      {err && <div style={{ fontSize: 12.5, color: '#c62828' }}>{err}</div>}
      {msg && <div style={{ fontSize: 12.5, color: '#2c8f56' }}>{msg}</div>}

      <div>
        <button style={primaryBtn} onClick={onDone}>
          {hosts.length > 0 ? 'Continue' : 'Continue without connecting'}
        </button>
      </div>
    </div>
  );
}

const sectionTitle: React.CSSProperties = { fontSize: 14, fontWeight: 700, color: '#233' };
const input: React.CSSProperties = {
  boxSizing: 'border-box', padding: '7px 9px', borderRadius: 6,
  border: '1px solid #ccd', fontSize: 13, fontFamily: 'ui-monospace, monospace',
};
const primaryBtn: React.CSSProperties = {
  padding: '7px 16px', borderRadius: 7, border: '1px solid #2c6', background: '#2c8f56',
  color: '#fff', cursor: 'pointer', fontSize: 13.5, fontWeight: 600,
};
const ghostBtn: React.CSSProperties = {
  padding: '6px 12px', borderRadius: 7, border: '1px solid #ccd', background: '#f4f6fb',
  color: '#446', cursor: 'pointer', fontSize: 13,
};
