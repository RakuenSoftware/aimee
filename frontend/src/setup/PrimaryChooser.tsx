import { useCallback, useEffect, useRef, useState } from 'react';

/* Provider chooser — wizard page 1 AND the Agents tab's "add delegate" first
 * window. Four supported providers, two shapes:
 *
 *   API key      → agent.add (Anthropic API, OpenAI-compatible API)
 *   Subscription → server-hosted OAuth CLI login (Claude / Codex "sign in with
 *                  your plan")
 *
 * Two modes select what the add means:
 *   'primary'  (wizard default) — the agent becomes the GLOBAL default (the one
 *              resolve_primary drives): agent.add --default, and the OAuth flow
 *              promotes the vendor agent via agent.set --default.
 *   'delegate' (Agents tab) — a named roster entry, NOT the default: the API
 *              form collects a delegate name + roles, and the OAuth flow leaves
 *              the registered vendor agent unpromoted.
 *
 * Every write goes through an existing endpoint — /api/agents/add,
 * /api/agents/set, and the /api/agents/oauth/* proxies — so there is no new
 * config surface. In primary mode, on success we also stamp the legacy
 * `provider` config breadcrumb so the wizard summary + header chip (which read
 * config, not the agent roster) reflect that a primary now exists.
 *
 * The subscription token is minted, vaulted, and refreshed entirely server-side:
 * only the verification URL, an optional device code, an opaque session handle,
 * and the login state ever reach the browser. */

type ApiKind = 'anthropic' | 'openai';
type SubKind = 'claude-sub' | 'codex-sub';
type Kind = ApiKind | SubKind;

interface ApiSpec {
  kind: ApiKind;
  label: string;
  blurb: string;
  provider: string; // agent provider + `provider` config breadcrumb
  endpoint: string;
  model: string;
  keyHint: string;
}

interface SubSpec {
  kind: SubKind;
  label: string;
  blurb: string;
  vendor: 'claude' | 'codex';
  provider: string; // `provider` config breadcrumb
}

const API_SPECS: ApiSpec[] = [
  {
    kind: 'anthropic',
    label: 'Anthropic API',
    blurb: 'Claude models via an api.anthropic.com key.',
    provider: 'anthropic',
    endpoint: 'https://api.anthropic.com/v1',
    model: 'claude-sonnet-4-6',
    keyHint: 'sk-ant-…',
  },
  {
    kind: 'openai',
    label: 'OpenAI API',
    blurb: 'GPT (or any OpenAI-compatible endpoint) via an API key.',
    provider: 'openai',
    endpoint: 'https://api.openai.com/v1',
    model: 'gpt-5.5',
    keyHint: 'sk-…',
  },
];

const SUB_SPECS: SubSpec[] = [
  {
    kind: 'claude-sub',
    label: 'Claude Subscription',
    blurb: 'Sign in with your Claude plan — no API key, billed to your subscription.',
    vendor: 'claude',
    provider: 'claude',
  },
  {
    kind: 'codex-sub',
    label: 'Codex Subscription',
    blurb: 'Sign in with your ChatGPT/Codex plan — no API key.',
    vendor: 'codex',
    provider: 'codex',
  },
];

const btn = (bg: string): React.CSSProperties => ({
  padding: '7px 16px', borderRadius: 7, border: `1px solid ${bg}`, background: bg,
  color: '#fff', cursor: 'pointer', fontSize: 13.5, fontWeight: 600,
});
const primaryBtn = btn('#2c8f56');
const ghostBtn: React.CSSProperties = {
  padding: '6px 12px', borderRadius: 7, border: '1px solid #ccd', background: '#f4f6fb',
  color: '#446', cursor: 'pointer', fontSize: 13,
};
const input: React.CSSProperties = {
  width: '100%', boxSizing: 'border-box', padding: '7px 9px', borderRadius: 6,
  border: '1px solid #ccd', fontSize: 13, fontFamily: 'ui-monospace, monospace',
};

function csrf(): string {
  try {
    if (typeof window !== 'undefined') return (window as { _csrf?: string })._csrf || '';
  } catch { /* ignore */ }
  return '';
}

async function postJSON<T>(url: string, body: unknown): Promise<T> {
  const r = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrf() },
    body: JSON.stringify(body),
  });
  let data: unknown = {};
  try { data = await r.json(); } catch { /* empty body */ }
  const d = (data ?? {}) as { error?: string };
  if (r.status < 200 || r.status >= 300 || d.error) {
    throw new Error(d.error || `request failed (${r.status})`);
  }
  return d as T;
}

// OAuth flow state for a subscription primary.
interface OauthState {
  session: string;
  url: string;
  code: string;          // device code to show (codex), '' otherwise
  needsCodeBack: boolean; // claude: operator pastes a code back
}

export interface PrimaryChooserProps {
  /** Called after the agent is fully configured (primary mode: and set as the
   * global default); the argument is the provider string ('primary' mode stamps
   * it into config as the legacy breadcrumb). */
  onConfigured: (provider: string) => void | Promise<void>;
  /** 'primary' (default): the wizard's add-and-make-default. 'delegate': the
   * Agents tab's add — a named, non-default roster entry. */
  mode?: 'primary' | 'delegate';
}

export default function PrimaryChooser({ onConfigured, mode = 'primary' }: PrimaryChooserProps) {
  const delegate = mode === 'delegate';
  const [selected, setSelected] = useState<Kind | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');

  // API-key form fields (seeded when a card is chosen).
  const [endpoint, setEndpoint] = useState('');
  const [model, setModel] = useState('');
  const [apiKey, setApiKey] = useState('');
  // Delegate mode only: the roster name + delegation roles for the new agent.
  const [name, setName] = useState('');
  const [roles, setRoles] = useState('summarize,format,draft');

  // Subscription OAuth flow.
  const [oauth, setOauth] = useState<OauthState | null>(null);
  const [codeBack, setCodeBack] = useState('');
  const [polling, setPolling] = useState(false);
  const alive = useRef(true);
  useEffect(() => () => { alive.current = false; }, []);

  const apiSpec = API_SPECS.find((s) => s.kind === selected);
  const subSpec = SUB_SPECS.find((s) => s.kind === selected);

  const chooseApi = (spec: ApiSpec) => {
    setSelected(spec.kind);
    setEndpoint(spec.endpoint);
    setModel(spec.model);
    setApiKey('');
    setName(delegate ? `${spec.provider}-delegate` : '');
    setError('');
    setOauth(null);
  };
  const chooseSub = (spec: SubSpec) => {
    setSelected(spec.kind);
    setError('');
    setOauth(null);
    setCodeBack('');
  };
  const reset = () => {
    setSelected(null); setError(''); setOauth(null); setCodeBack('');
    setBusy(false); setPolling(false);
  };

  // --- API-key agent: agent.add (primary mode adds --default) -------------
  const submitApi = async () => {
    if (!apiSpec) return;
    if (!endpoint.trim() || !model.trim() || !apiKey.trim()) {
      setError('Endpoint, model, and API key are all required.');
      return;
    }
    if (delegate && !name.trim()) {
      setError('Give the delegate a name.');
      return;
    }
    setBusy(true);
    setError('');
    try {
      const args = [
        delegate ? name.trim() : apiSpec.provider, endpoint.trim(), model.trim(),
        '--provider', apiSpec.provider,
        '--key', apiKey.trim(),
      ];
      if (delegate) {
        if (roles.trim()) args.push('--roles', roles.trim());
      } else {
        args.push('--default');
      }
      await postJSON('/api/agents/add', { args });
      await onConfigured(apiSpec.provider);
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Could not add the agent.');
      setBusy(false);
    }
  };

  // --- Subscription agent: OAuth login (primary mode then agent.set --default)
  const pollOnce = useCallback(async (vendor: string, session: string): Promise<'pending' | 'authenticated' | 'failed'> => {
    try {
      const r = await postJSON<{ state?: string; agent?: string; error?: string }>(
        '/api/agents/oauth/poll', { vendor, session });
      if (r.state === 'authenticated') {
        // Primary mode: promote the freshly-registered vendor agent to the
        // global primary. A delegate stays an ordinary roster entry.
        if (!delegate) {
          const agent = r.agent || vendor;
          await postJSON('/api/agents/set', { args: [agent, '--default'] });
        }
        return 'authenticated';
      }
      if (r.state === 'failed') { setError(r.error || 'Login failed or timed out.'); return 'failed'; }
      return 'pending';
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Poll failed.');
      return 'failed';
    }
  }, [delegate]);

  const runPoll = useCallback(async (vendor: string, session: string, provider: string) => {
    setPolling(true);
    for (let i = 0; i < 120 && alive.current; i++) {
      const state = await pollOnce(vendor, session);
      if (!alive.current) return;
      if (state === 'authenticated') { setPolling(false); await onConfigured(provider); return; }
      if (state === 'failed') { setPolling(false); return; }
      await new Promise((res) => setTimeout(res, 3000));
    }
    if (alive.current) { setPolling(false); setError('Timed out waiting for sign-in.'); }
  }, [pollOnce, onConfigured]);

  const startSub = async () => {
    if (!subSpec) return;
    setBusy(true);
    setError('');
    try {
      const r = await postJSON<{ url: string; code?: string; session: string; needs_code_back?: boolean }>(
        '/api/agents/oauth/start', { vendor: subSpec.vendor });
      const st: OauthState = {
        session: r.session, url: r.url, code: r.code || '', needsCodeBack: !!r.needs_code_back,
      };
      setOauth(st);
      setBusy(false);
      // Codex surfaces a device code and needs no code-back — start polling
      // immediately. Claude waits for the operator to paste a code back first.
      if (!st.needsCodeBack) void runPoll(subSpec.vendor, st.session, subSpec.provider);
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Could not start sign-in.');
      setBusy(false);
    }
  };

  const submitCodeBack = async () => {
    if (!subSpec || !oauth) return;
    if (!codeBack.trim()) { setError('Paste the code shown on the authorization page.'); return; }
    setBusy(true);
    setError('');
    try {
      await postJSON('/api/agents/oauth/code', {
        vendor: subSpec.vendor, session: oauth.session, code: codeBack.trim(),
      });
      setBusy(false);
      void runPoll(subSpec.vendor, oauth.session, subSpec.provider);
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Could not submit the code.');
      setBusy(false);
    }
  };

  // --- render -------------------------------------------------------------
  if (!selected) {
    return (
      <div style={{ display: 'grid', gap: 10, marginBottom: 14 }}>
        <p style={{ fontSize: 13, color: '#556', margin: '0 0 2px' }}>
          {delegate
            ? 'Pick the provider for this delegate.'
            : 'Pick the primary model aimee drives. You can change it later on the Agents tab.'}
        </p>
        {API_SPECS.map((s) => (
          <button key={s.kind} onClick={() => chooseApi(s)} style={cardStyle}>
            <span style={cardTitle}>{s.label}</span>
            <span style={cardBlurb}>{s.blurb}</span>
          </button>
        ))}
        {SUB_SPECS.map((s) => (
          <button key={s.kind} onClick={() => chooseSub(s)} style={cardStyle}>
            <span style={cardTitle}>{s.label}</span>
            <span style={cardBlurb}>{s.blurb}</span>
          </button>
        ))}
      </div>
    );
  }

  return (
    <div style={{ marginBottom: 14 }}>
      <button onClick={reset} style={{ ...ghostBtn, marginBottom: 12 }} disabled={busy || polling}>
        ← Choose a different {delegate ? 'provider' : 'primary'}
      </button>

      {apiSpec && (
        <div style={{ display: 'grid', gap: 12 }}>
          <div style={{ fontSize: 15, fontWeight: 700 }}>{apiSpec.label}</div>
          {delegate && (
            <>
              <Field label="Delegate name">
                <input style={input} value={name} onChange={(e) => setName(e.target.value)}
                  placeholder={`${apiSpec.provider}-delegate`} />
              </Field>
              <Field label="Roles (comma-separated)">
                <input style={input} value={roles} onChange={(e) => setRoles(e.target.value)} />
              </Field>
            </>
          )}
          <Field label="Endpoint">
            <input style={input} value={endpoint} onChange={(e) => setEndpoint(e.target.value)} placeholder={apiSpec.endpoint} />
          </Field>
          <Field label="Model">
            <input style={input} value={model} onChange={(e) => setModel(e.target.value)} placeholder={apiSpec.model} />
          </Field>
          <Field label="API key">
            <input style={input} type="password" autoComplete="off" value={apiKey}
              onChange={(e) => setApiKey(e.target.value)} placeholder={apiSpec.keyHint} />
          </Field>
          <div style={{ fontSize: 11.5, color: '#778' }}>
            The key is sealed into aimee-server’s vault — it is never stored in the browser or in agents.json.
          </div>
          <div>
            <button style={primaryBtn} disabled={busy} onClick={submitApi}>
              {busy ? 'Saving…' : delegate ? 'Add delegate' : 'Save & set as primary'}
            </button>
          </div>
        </div>
      )}

      {subSpec && (
        <div style={{ display: 'grid', gap: 12 }}>
          <div style={{ fontSize: 15, fontWeight: 700 }}>{subSpec.label}</div>
          {!oauth ? (
            <>
              <p style={{ fontSize: 13, color: '#556', margin: 0, lineHeight: 1.5 }}>
                aimee installs the {subSpec.vendor} CLI on the server and starts a login. You’ll get a
                link to authorize in your browser. This can take up to a minute on first run.
              </p>
              <div>
                <button style={primaryBtn} disabled={busy} onClick={startSub}>
                  {busy ? 'Starting…' : `Sign in with ${subSpec.label}`}
                </button>
              </div>
            </>
          ) : (
            <div style={{ display: 'grid', gap: 10 }}>
              <div style={{ fontSize: 13, color: '#556' }}>
                1. Open this link and authorize:
                <div style={{ marginTop: 4 }}>
                  <a href={oauth.url} target="_blank" rel="noreferrer" style={{ color: '#2c6', wordBreak: 'break-all' }}>
                    {oauth.url}
                  </a>
                </div>
              </div>
              {oauth.code && (
                <div style={{ fontSize: 13, color: '#556' }}>
                  2. Enter this one-time code on that page:{' '}
                  <code style={{ background: '#f1f4f9', padding: '2px 6px', borderRadius: 4, fontWeight: 700 }}>{oauth.code}</code>
                </div>
              )}
              {oauth.needsCodeBack && !polling && (
                <div style={{ display: 'grid', gap: 6 }}>
                  <div style={{ fontSize: 13, color: '#556' }}>
                    {oauth.code ? '3.' : '2.'} After authorizing, paste the code shown back here:
                  </div>
                  <input style={input} value={codeBack} onChange={(e) => setCodeBack(e.target.value)} placeholder="paste code" />
                  <div>
                    <button style={primaryBtn} disabled={busy} onClick={submitCodeBack}>
                      {busy ? 'Submitting…' : 'Submit code'}
                    </button>
                  </div>
                </div>
              )}
              {polling && (
                <div style={{ fontSize: 13, color: '#2c8f56' }}>⏳ Waiting for sign-in to complete…</div>
              )}
            </div>
          )}
        </div>
      )}

      {error && (
        <div style={{ marginTop: 12, fontSize: 12.5, color: '#a33', background: '#fdeaea', border: '1px solid #f2c4c4', borderRadius: 6, padding: '8px 10px' }}>
          {error}
        </div>
      )}
    </div>
  );
}

const cardStyle: React.CSSProperties = {
  display: 'flex', flexDirection: 'column', gap: 3, textAlign: 'left', padding: '11px 13px',
  borderRadius: 9, border: '1px solid #dde', background: '#fbfcfe', cursor: 'pointer',
};
const cardTitle: React.CSSProperties = { fontSize: 14, fontWeight: 700, color: '#233' };
const cardBlurb: React.CSSProperties = { fontSize: 12, color: '#667' };

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <label style={{ display: 'block' }}>
      <div style={{ fontSize: 12.5, fontWeight: 600, marginBottom: 3 }}>{label}</div>
      {children}
    </label>
  );
}
