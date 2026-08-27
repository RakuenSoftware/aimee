import { useEffect, useState } from 'react';
import { Button, useToast } from '@rakuensoftware/smoothgui';
import { loadConfig, saveConfigValue, type ConfigMap } from './configApi';
import { isRestartKey } from './wizardSteps';
import { buildDesiredConfig, type KbMode } from './deployTopology';

/* Wizard step 2 — Knowledge base. The fork that shapes the rest of the wizard:
 *
 *  • Local  — deploy an aimee-kb on this instance. The following Deploy-topology
 *             + Shared-store (DB2) steps configure it.
 *  • Cloud  — redeem a setup code from a hosted provider. The code is exchanged
 *             for a URL and a key, which is what Remote asks an operator to
 *             paste by hand, so this is Remote with the typing removed and it
 *             saves identical config.
 *  • Remote — connect to an existing aimee-kb (kb_client_url + bearer token).
 *             Nothing is deployed here, so the wizard skips deploy topology + DB2.
 *
 * It writes only the kb_* keys (via buildDesiredConfig's remote branch, or a bare
 * kb_mode='local' write), guards every save (Toast + stay put on failure), and
 * reports both the chosen mode and the restart-class keys it changed so the wizard
 * can update its visible steps + restart summary. Self-contained like
 * PrimaryChooser / DeployTopology. */

/* Where a setup code is redeemed. A default rather than a constant: a hosted
 * aimee is not required to be ours, and someone running their own should not
 * have to patch a binary to point at it. */
export const DEFAULT_CLOUD_ENDPOINT = 'https://api.aimee.rakuensoftware.com';

/** Which source the operator picked. Cloud and Remote both persist
 *  kb_mode='remote'; they differ only in how the URL and key are obtained. */
type KbSource = 'local' | 'cloud' | 'remote';

export interface KnowledgeBaseProps {
  /** Called after the KB choice is persisted, with the restart-class keys changed
   * and the chosen mode (so the wizard can recompute which steps to show). */
  onSaved: (restartKeys: string[], kbMode: KbMode) => void | Promise<void>;
  /** Injected in tests (vitest node env has no real network). */
  fetchImpl?: typeof fetch;
  /** Where a setup code is redeemed. Defaults to aimee cloud. */
  cloudEndpoint?: string;
}

export default function KnowledgeBase({
  onSaved,
  fetchImpl,
  cloudEndpoint: cloudEndpointProp,
}: KnowledgeBaseProps) {
  const toast = useToast();
  const [cfg, setCfg] = useState<ConfigMap>({});
  const [loaded, setLoaded] = useState(false);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');

  const [source, setSource] = useState<KbSource>('local');

  const [code, setCode] = useState('');
  const [redeeming, setRedeeming] = useState(false);
  const [redeemed, setRedeemed] = useState('');
  /* Not a visible field. The cloud option asks for exactly one thing, a code;
   * turning the endpoint into an input turns that into a decision. It stays
   * overridable as a prop so someone hosting their own is not stuck. */
  const cloudEndpoint = cloudEndpointProp ?? DEFAULT_CLOUD_ENDPOINT;

  /* Cloud and Remote are the same persisted mode; only the UI differs. */
  const kbMode: KbMode = source === 'local' ? 'local' : 'remote';
  const [kbUrl, setKbUrl] = useState('');
  const [kbBearer, setKbBearer] = useState('');

  useEffect(() => {
    let alive = true;
    (async () => {
      const c = await loadConfig({ fetchImpl });
      if (!alive) return;
      setCfg(c);
      setSource(String(c.kb_mode ?? 'local') === 'remote' ? 'remote' : 'local');
      setKbUrl(String(c.kb_client_url ?? ''));
      setKbBearer(String(c.kb_client_bearer_token ?? ''));
      setLoaded(true);
    })();
    return () => {
      alive = false;
    };
  }, [fetchImpl]);

  /* Exchange a setup code for the URL and key it stands for.
   *
   * A code is single-use and short-lived, so this must not fire speculatively.
   * It sits behind an explicit button, and a failure leaves the code in the box
   * so a typo can be corrected without burning a fresh one. */
  async function redeem() {
    setRedeeming(true);
    setError('');
    setRedeemed('');
    try {
      const doFetch = fetchImpl ?? fetch;
      const res = await doFetch(`${cloudEndpoint.replace(/\/+$/, '')}/v1/setup/redeem`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ code: code.trim() }),
      });
      const body = (await res.json()) as {
        kb_url?: string;
        bearer?: string;
        tenant?: string;
        error?: string;
      };
      if (!res.ok) throw new Error(body.error ?? 'That code was not accepted.');
      if (!body.kb_url || !body.bearer) {
        throw new Error('The provider did not return a usable knowledge base.');
      }
      setKbUrl(body.kb_url);
      setKbBearer(body.bearer);
      setRedeemed(body.tenant ? `Connected to ${body.tenant}.` : 'Code accepted.');
    } catch (e) {
      const msg = e instanceof Error ? e.message : 'That code was not accepted.';
      setError(msg);
      toast.error(msg);
    } finally {
      setRedeeming(false);
    }
  }

  async function save() {
    setSaving(true);
    setError('');

    // Remote writes kb_mode + the client url/token; local writes just kb_mode
    // (the deploy-topology + DB2 steps handle the rest). Reuse buildDesiredConfig's
    // remote branch so the key mapping stays single-sourced. The embedder and
    // synthesis selections are irrelevant to the kb_* keys — the remote branch
    // returns before reading them — so they are passed at their inert defaults.
    const desired: Record<string, string> =
      kbMode === 'remote'
        ? buildDesiredConfig({
            kbMode: 'remote',
            kbUrl,
            kbBearer,
            embedder: { kind: 'bundled', model: '' },
            synthesis: { kind: 'off' },
          })
        : { kb_mode: 'local' };

    // Persist only what changed (mirrors DeployTopology.save); abort + Toast on
    // the first failure, keeping the operator on the step with input intact.
    const savedCfg: ConfigMap = { ...cfg };
    const restart = new Set<string>();
    for (const [key, value] of Object.entries(desired)) {
      const original = cfg[key] == null ? '' : String(cfg[key]);
      if (value === original) continue;
      const res = await saveConfigValue(key, value);
      if (!res.ok) {
        setError(`Couldn’t save ${key}: ${res.error ?? 'unknown error'}`);
        toast.error(`Couldn’t save ${key}: ${res.error ?? 'unknown error'}`);
        setSaving(false);
        setCfg(savedCfg); // keep whatever already succeeded
        return;
      }
      savedCfg[key] = res.value !== undefined ? String(res.value) : value;
      if (isRestartKey(key)) restart.add(key);
    }

    setCfg(savedCfg);
    setSaving(false);
    toast.success('Knowledge base saved');
    await onSaved(Array.from(restart), kbMode);
  }

  if (!loaded) {
    return <div style={{ fontSize: 13, color: 'var(--sg-text-secondary)', padding: '8px 0' }}>Loading…</div>;
  }

  const remote = source === 'remote';
  const cloud = source === 'cloud';
  /* Nothing to save until a code has actually been exchanged. */
  const cloudIncomplete = cloud && (kbUrl === '' || kbBearer === '');

  return (
    <div style={{ display: 'grid', gap: 14, marginBottom: 8 }}>
      <div style={{ fontSize: 12.5, color: 'var(--sg-text-muted)', lineHeight: 1.5 }}>
        aimee needs a knowledge base for memory + search. Deploy one here, or point at an existing one.
      </div>

      <section style={{ display: 'grid', gap: 8 }}>
        <label style={radioRow}>
          <input type="radio" checked={source === 'local'} onChange={() => setSource('local')} />
          <span>Deploy a local knowledge base (recommended)</span>
        </label>
        <label style={radioRow}>
          <input type="radio" checked={source === 'cloud'} onChange={() => setSource('cloud')} />
          <span>aimee cloud, or another hosted aimee-kb (paste a setup code)</span>
        </label>
        <label style={radioRow}>
          <input type="radio" checked={remote} onChange={() => setSource('remote')} />
          <span>Connect to an existing aimee-kb by hand</span>
        </label>

        {cloud ? (
          <div style={{ display: 'grid', gap: 8, paddingLeft: 24 }}>
            <div style={{ fontSize: 11.5, color: 'var(--sg-text-faint)' }}>
              Paste the code from your welcome email. It is exchanged for the address and key of
              your knowledge base, so nothing is deployed here and the next two steps are skipped.
            </div>
            <Field label="Setup code">
              <input style={input} value={code} onChange={(e) => setCode(e.target.value)}
                placeholder="AIMEE-XXXX-XXXX-XXXX-XXXX-XXXX" autoComplete="off" />
            </Field>
            <div>
              <Button variant="default" disabled={redeeming || code.trim() === ''} onClick={redeem}>
                {redeeming ? 'Redeeming…' : 'Redeem code'}
              </Button>
            </div>
            {redeemed && (
              <div style={{ fontSize: 12, color: 'var(--sg-success)' }}>
                {redeemed} Continue below to finish.
              </div>
            )}
          </div>
        ) : remote ? (
          <div style={{ display: 'grid', gap: 8, paddingLeft: 24 }}>
            <div style={{ fontSize: 11.5, color: 'var(--sg-text-faint)' }}>
              A remote KB deploys nothing here — aimee-server just connects to it. The deploy-topology
              and shared-store steps are skipped.
            </div>
            <Field label="aimee-kb URL">
              <input style={input} value={kbUrl} onChange={(e) => setKbUrl(e.target.value)}
                placeholder="https://kb.example:8760" />
            </Field>
            <Field label="Bearer token">
              <input style={input} type="password" autoComplete="off" value={kbBearer}
                onChange={(e) => setKbBearer(e.target.value)} placeholder="token" />
            </Field>
          </div>
        ) : (
          <div style={{ fontSize: 11.5, color: 'var(--sg-text-faint)', paddingLeft: 24 }}>
            The next steps place the embedder + synthesizer and set the shared store.
          </div>
        )}
      </section>

      {error && (
        <div style={{ fontSize: 12.5, color: 'var(--sg-danger)', background: 'var(--sg-danger-bg)', border: '1px solid var(--sg-danger-bg)', borderRadius: 6, padding: '8px 10px' }}>
          {error}
        </div>
      )}

      <div>
        <Button variant="primary" disabled={saving || cloudIncomplete} onClick={save}>
          {saving ? 'Saving…' : 'Save & continue'}
        </Button>
      </div>
    </div>
  );
}

const radioRow: React.CSSProperties = { display: 'flex', alignItems: 'center', gap: 8, fontSize: 13, cursor: 'pointer' };
const input: React.CSSProperties = {
  width: '100%', boxSizing: 'border-box', padding: '7px 9px', borderRadius: 6,
  border: '1px solid var(--sg-border-medium)', fontSize: 13, fontFamily: 'ui-monospace, monospace',
};

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <label style={{ display: 'block' }}>
      <div style={{ fontSize: 12.5, fontWeight: 600, marginBottom: 3 }}>{label}</div>
      {children}
    </label>
  );
}
