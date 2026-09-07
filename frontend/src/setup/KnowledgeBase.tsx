import { useEffect, useState } from 'react';
import { Button, useToast } from '@rakuensoftware/smoothgui';
import { loadConfig, saveConfigValue, type ConfigMap } from './configApi';
import { isRestartKey } from './wizardSteps';
import { type KbMode } from './deployTopology';

/* Optional connection to an existing shared KB, configured from Settings. */

/* Where a setup code is redeemed. A default rather than a constant: a hosted
 * aimee is not required to be ours, and someone running their own should not
 * have to patch a binary to point at it. */
export const DEFAULT_CLOUD_ENDPOINT = 'https://api.aimee.rakuensoftware.com';

/** Which source the operator picked. Cloud and Remote both persist
 *  kb_mode='remote'; they differ only in how the URL and key are obtained. */
type KbSource = 'none' | 'cloud' | 'remote';

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

  const [source, setSource] = useState<KbSource>('none');

  const [code, setCode] = useState('');
  const [redeeming, setRedeeming] = useState(false);
  const [redeemed, setRedeemed] = useState('');
  /* Not a visible field. The cloud option asks for exactly one thing, a code;
   * turning the endpoint into an input turns that into a decision. It stays
   * overridable as a prop so someone hosting their own is not stuck. */
  const cloudEndpoint = cloudEndpointProp ?? DEFAULT_CLOUD_ENDPOINT;

  /* Cloud and Remote are the same persisted mode; only the UI differs. */
  const kbMode: KbMode = source === 'none' ? 'none' : 'remote';
  const [kbUrl, setKbUrl] = useState('');
  const [kbBearer, setKbBearer] = useState('');
  const [serviceIdentity, setServiceIdentity] = useState('');

  useEffect(() => {
    let alive = true;
    (async () => {
      const c = await loadConfig({ fetchImpl });
      if (!alive) return;
      setCfg(c);
      setSource(String(c.kb_mode) !== 'none' && (String(c.kb_client_url ?? '').trim() || c.kb_connection_string === true) ? 'remote' : 'none');
      setKbUrl(String(c.kb_client_url ?? ''));
      setKbBearer(''); // Secrets are presence flags; never echo or save those flags as credentials.
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

    const endpoint = kbUrl.trim();
    const enrollment = endpoint.startsWith('aimee://') || (!endpoint && cfg.kb_connection_string === true);
    const desired: Record<string, string> = {};
    if (kbMode === 'none') {
      Object.assign(desired, { kb_client_url: '', kb_connection_string: '', kb_client_bearer_token: '', kb_service_identity_token: '' });
    } else {
      if (enrollment) {
        if (endpoint) desired.kb_connection_string = endpoint;
        desired.kb_client_url = '';
      } else {
        desired.kb_client_url = endpoint;
        desired.kb_connection_string = '';
      }
      if (kbBearer.trim()) desired.kb_client_bearer_token = kbBearer.trim();
      if (serviceIdentity.trim()) desired.kb_service_identity_token = serviceIdentity.trim();
    }
    // Activate the connection only after its credentials have been accepted.
    desired.kb_mode = kbMode;

    // Persist only what changed (mirrors DeployTopology.save); abort + Toast on
    // the first failure, keeping the operator on the step with input intact.
    const savedCfg: ConfigMap = { ...cfg };
    const restart = new Set<string>();
    for (const [key, value] of Object.entries(desired)) {
      const original = cfg[key] == null ? '' : String(cfg[key]);
      if (value === original) continue;
      const res = await saveConfigValue(key, value, { fetchImpl });
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
    setKbBearer('');
    setServiceIdentity('');
    if (enrollment) setKbUrl('');
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
        Personal memory runs on this Server. You can also connect to a shared knowledge base.
      </div>

      <section style={{ display: 'grid', gap: 8 }}>
        <label style={radioRow}>
          <input type="radio" checked={source === 'none'} onChange={() => setSource('none')} />
          <span>Personal memory only</span>
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
              your shared knowledge base. Personal memory and its models stay on this Server.
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
              Connect to a KB deployed separately. This does not change your local models.
            </div>
            <Field label="KB address or enrollment connection string">
              <input style={input} value={kbUrl} onChange={(e) => setKbUrl(e.target.value)}
                autoComplete="off" placeholder={cfg.kb_connection_string === true ? "Enrollment saved; leave blank to keep" : "aimee://kb.example:8745?… or https://kb.example"} />
            </Field>
            <Field label="Bearer token">
              <input style={input} type="password" autoComplete="off" value={kbBearer}
                onChange={(e) => setKbBearer(e.target.value)} placeholder={cfg.kb_client_bearer_token === true ? "Configured; leave blank to keep" : "Bearer from the KB administrator"} />
            </Field>
            <Field label="Service identity token (for enrollment connections)">
              <input style={input} type="password" autoComplete="off" value={serviceIdentity}
                onChange={(e) => setServiceIdentity(e.target.value)}
                placeholder={cfg.kb_service_identity_token === true ? "Configured; leave blank to keep" : "Matching service identity from the KB administrator"} />
            </Field>
          </div>
        ) : (
          <div style={{ fontSize: 11.5, color: 'var(--sg-text-faint)', paddingLeft: 24 }}>
            No shared knowledge connection. Your personal memory remains available locally.
          </div>
        )}
      </section>

      {error && (
        <div style={{ fontSize: 12.5, color: 'var(--sg-danger)', background: 'var(--sg-danger-bg)', border: '1px solid var(--sg-danger-bg)', borderRadius: 6, padding: '8px 10px' }}>
          {error}
        </div>
      )}

      <div>
        <Button variant="primary" disabled={saving || cloudIncomplete || (remote && !kbUrl.trim() && cfg.kb_connection_string !== true)} onClick={save}>
          {saving ? 'Saving…' : 'Save connection'}
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
