import { useEffect, useState } from 'react';
import { Button, useToast } from '@rakuensoftware/smoothgui';
import { loadConfig, saveConfigValue, type ConfigMap } from './configApi';
import { isRestartKey } from './wizardSteps';

/* Wizard step — Shared store (DB2), shown only for a LOCAL knowledge base.
 *
 * A local KB always has a Postgres (DB2 + pgvector) store. There are two ways to
 * provide it:
 *
 *  • Bundled  — the deploy stack brings up its own Postgres automatically (compose
 *               injects AIMEE_DB2_URL). Nothing to enter; db2_url stays blank.
 *  • Existing — point the KB at a Postgres you already run (enter db2_url).
 *
 * So db2_url is only asked for when the operator opts into an existing database —
 * spawning your own KB needs no URL. It writes just db2_url ('' for bundled),
 * guards the save (Toast + stay put on failure), and reports the restart-class
 * keys it changed. Self-contained like KnowledgeBase / DeployTopology. */

export interface SharedStoreProps {
  /** Called after the store choice is persisted, with the restart-class keys
   * changed (so the wizard can update its restart summary). */
  onSaved: (restartKeys: string[]) => void | Promise<void>;
  /** Injected in tests (vitest node env has no real network). */
  fetchImpl?: typeof fetch;
}

type StoreMode = 'bundled' | 'existing';

export default function SharedStore({ onSaved, fetchImpl }: SharedStoreProps) {
  const toast = useToast();
  const [cfg, setCfg] = useState<ConfigMap>({});
  const [loaded, setLoaded] = useState(false);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');

  const [mode, setMode] = useState<StoreMode>('bundled');
  const [db2Url, setDb2Url] = useState('');

  useEffect(() => {
    let alive = true;
    (async () => {
      const c = await loadConfig({ fetchImpl });
      if (!alive) return;
      setCfg(c);
      const url = String(c.db2_url ?? '');
      // A stored db2_url means the operator already pointed at an existing DB;
      // a blank one is the bundled default.
      setMode(url.trim() !== '' ? 'existing' : 'bundled');
      setDb2Url(url);
      setLoaded(true);
    })();
    return () => {
      alive = false;
    };
  }, [fetchImpl]);

  async function save() {
    setSaving(true);
    setError('');

    // Bundled clears db2_url ('' ⇒ deploy stack uses its own Postgres); existing
    // persists the entered URL. Only write when it actually changed.
    const value = mode === 'existing' ? db2Url.trim() : '';
    if (mode === 'existing' && value === '') {
      setError('Enter the Postgres connection URL for your existing database.');
      setSaving(false);
      return;
    }

    const original = cfg.db2_url == null ? '' : String(cfg.db2_url);
    if (value !== original) {
      const res = await saveConfigValue('db2_url', value, { fetchImpl });
      if (!res.ok) {
        setError(`Couldn’t save the store URL: ${res.error ?? 'unknown error'}`);
        toast.error(`Couldn’t save the store URL: ${res.error ?? 'unknown error'}`);
        setSaving(false);
        return;
      }
      setCfg({ ...cfg, db2_url: res.value !== undefined ? String(res.value) : value });
    }

    setSaving(false);
    toast.success('Shared store saved');
    await onSaved(isRestartKey('db2_url') ? ['db2_url'] : []);
  }

  if (!loaded) {
    return <div style={{ fontSize: 13, color: '#667', padding: '8px 0' }}>Loading…</div>;
  }

  const existing = mode === 'existing';

  return (
    <div style={{ display: 'grid', gap: 14, marginBottom: 8 }}>
      <div style={{ fontSize: 12.5, color: '#556', lineHeight: 1.5 }}>
        A local knowledge base stores its memory + embeddings in Postgres (DB2). Let the deploy
        spawn its own, or connect one you already run.
      </div>

      <section style={{ display: 'grid', gap: 8 }}>
        <label style={radioRow}>
          <input type="radio" checked={!existing} onChange={() => setMode('bundled')} />
          <span>Deploy a bundled Postgres (recommended)</span>
        </label>
        <label style={radioRow}>
          <input type="radio" checked={existing} onChange={() => setMode('existing')} />
          <span>Connect to an existing database</span>
        </label>

        {existing ? (
          <div style={{ display: 'grid', gap: 8, paddingLeft: 24 }}>
            <div style={{ fontSize: 11.5, color: '#778' }}>
              Point the knowledge base at a Postgres (with the <code>vector</code> + <code>pg_trgm</code>{' '}
              extensions) you already run. Changing this needs a server restart.
            </div>
            <label style={{ display: 'block' }}>
              <div style={{ fontSize: 12.5, fontWeight: 600, marginBottom: 3 }}>DB2 URL</div>
              <input style={input} value={db2Url} onChange={(e) => setDb2Url(e.target.value)}
                placeholder="postgresql://user:pass@host:5432/aimee_shared" />
            </label>
          </div>
        ) : (
          <div style={{ fontSize: 11.5, color: '#778', paddingLeft: 24 }}>
            The deploy stack brings up its own Postgres automatically — no connection URL needed.
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
