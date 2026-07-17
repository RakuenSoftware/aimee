import { useEffect, useState } from 'react';
import { Button, useToast } from '@rakuensoftware/smoothgui';
import { loadConfig, saveConfigValue, type ConfigMap } from './configApi';
import { isRestartKey } from './wizardSteps';
import { buildDesiredConfig, type KbMode } from './deployTopology';

/* Wizard step 2 — Knowledge base. The fork that shapes the rest of the wizard:
 *
 *  • Local  — deploy an aimee-kb on this instance. The following Deploy-topology
 *             + Shared-store (DB2) steps configure it.
 *  • Remote — connect to an existing aimee-kb (kb_client_url + bearer token).
 *             Nothing is deployed here, so the wizard skips deploy topology + DB2.
 *
 * It writes only the kb_* keys (via buildDesiredConfig's remote branch, or a bare
 * kb_mode='local' write), guards every save (Toast + stay put on failure), and
 * reports both the chosen mode and the restart-class keys it changed so the wizard
 * can update its visible steps + restart summary. Self-contained like
 * PrimaryChooser / DeployTopology. */

export interface KnowledgeBaseProps {
  /** Called after the KB choice is persisted, with the restart-class keys changed
   * and the chosen mode (so the wizard can recompute which steps to show). */
  onSaved: (restartKeys: string[], kbMode: KbMode) => void | Promise<void>;
  /** Injected in tests (vitest node env has no real network). */
  fetchImpl?: typeof fetch;
}

export default function KnowledgeBase({ onSaved, fetchImpl }: KnowledgeBaseProps) {
  const toast = useToast();
  const [cfg, setCfg] = useState<ConfigMap>({});
  const [loaded, setLoaded] = useState(false);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');

  const [kbMode, setKbMode] = useState<KbMode>('local');
  const [kbUrl, setKbUrl] = useState('');
  const [kbBearer, setKbBearer] = useState('');

  useEffect(() => {
    let alive = true;
    (async () => {
      const c = await loadConfig({ fetchImpl });
      if (!alive) return;
      setCfg(c);
      setKbMode(String(c.kb_mode ?? 'local') === 'remote' ? 'remote' : 'local');
      setKbUrl(String(c.kb_client_url ?? ''));
      setKbBearer(String(c.kb_client_bearer_token ?? ''));
      setLoaded(true);
    })();
    return () => {
      alive = false;
    };
  }, [fetchImpl]);

  async function save() {
    setSaving(true);
    setError('');

    // Remote writes kb_mode + the client url/token; local writes just kb_mode
    // (the deploy-topology + DB2 steps handle the rest). Reuse buildDesiredConfig's
    // remote branch so the key mapping stays single-sourced; for local, only
    // kb_mode is relevant here. Placement fields are irrelevant to the kb_* keys.
    const desired: Record<string, string> =
      kbMode === 'remote'
        ? buildDesiredConfig({
            kbMode: 'remote',
            kbUrl,
            kbBearer,
            placements: {
              embed: { backend: 'off' },
              rerank: { backend: 'off' },
              synth: { backend: 'off' },
            },
            embedModel: '',
            embedDim: '',
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
    return <div style={{ fontSize: 13, color: '#667', padding: '8px 0' }}>Loading…</div>;
  }

  const remote = kbMode === 'remote';

  return (
    <div style={{ display: 'grid', gap: 14, marginBottom: 8 }}>
      <div style={{ fontSize: 12.5, color: '#556', lineHeight: 1.5 }}>
        aimee needs a knowledge base for memory + search. Deploy one here, or point at an existing one.
      </div>

      <section style={{ display: 'grid', gap: 8 }}>
        <label style={radioRow}>
          <input type="radio" checked={!remote} onChange={() => setKbMode('local')} />
          <span>Deploy a local knowledge base (recommended)</span>
        </label>
        <label style={radioRow}>
          <input type="radio" checked={remote} onChange={() => setKbMode('remote')} />
          <span>Connect to an existing aimee-kb</span>
        </label>

        {remote ? (
          <div style={{ display: 'grid', gap: 8, paddingLeft: 24 }}>
            <div style={{ fontSize: 11.5, color: '#778' }}>
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
          <div style={{ fontSize: 11.5, color: '#778', paddingLeft: 24 }}>
            The next steps place the embedder + reranker + synthesizer and set the shared store.
          </div>
        )}
      </section>

      {error && (
        <div style={{ fontSize: 12.5, color: '#a33', background: '#fdeaea', border: '1px solid #f2c4c4', borderRadius: 6, padding: '8px 10px' }}>
          {error}
        </div>
      )}

      <div>
        <Button variant="primary" disabled={saving} onClick={save}>
          {saving ? 'Saving…' : 'Save & continue'}
        </Button>
      </div>
    </div>
  );
}

const radioRow: React.CSSProperties = { display: 'flex', alignItems: 'center', gap: 8, fontSize: 13, cursor: 'pointer' };
const input: React.CSSProperties = {
  width: '100%', boxSizing: 'border-box', padding: '7px 9px', borderRadius: 6,
  border: '1px solid #ccd', fontSize: 13, fontFamily: 'ui-monospace, monospace',
};

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <label style={{ display: 'block' }}>
      <div style={{ fontSize: 12.5, fontWeight: 600, marginBottom: 3 }}>{label}</div>
      {children}
    </label>
  );
}
