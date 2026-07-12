import { useCallback, useEffect, useMemo, useState } from 'react';
import { useToast } from '@rakuensoftware/smoothgui';
import { loadConfig, saveConfigValue, type ConfigMap } from './configApi';
import { isRestartKey } from './wizardSteps';
import {
  ROLES,
  type Role,
  type Placement,
  type HostInfo,
  placementOptions,
  placementOptionId,
  configToPlacement,
  synthIsTierAOnly,
  buildDesiredConfig,
} from './deployTopology';

/* Wizard — Deploy topology (local knowledge base only). Places the three LLM
 * roles (embedder / reranker / synthesizer) onto the page-2 config record (per-role
 * llm_* keys), which `aimee config deploy-env` translates to the compose stack.
 * Every write goes through the existing /api/config/set allowlist (no new backend).
 * All local roles share ONE aimee-llm container on the chosen host.
 *
 * The knowledge-base local/remote choice lives in the preceding KnowledgeBase step;
 * this step is only shown for kb_mode='local', so it no longer renders the KB fork.
 *
 * Self-contained like PrimaryChooser: it loads config + GET /api/hosts, guards
 * every save (Toast + stay put on failure), and reports the restart-class keys it
 * changed so the wizard can list them on its summary. */

function csrf(): string {
  try {
    if (typeof window !== 'undefined') return (window as { _csrf?: string })._csrf || '';
  } catch {
    /* ignore */
  }
  return '';
}

async function fetchHosts(fetchImpl?: typeof fetch): Promise<HostInfo[]> {
  const f = fetchImpl ?? fetch;
  try {
    const r = await f('/api/hosts', { headers: { 'X-CSRF-Token': csrf() } });
    const d = (await r.json()) as { hosts?: HostInfo[] };
    return Array.isArray(d.hosts) ? d.hosts : [];
  } catch {
    return [];
  }
}

export interface DeployTopologyProps {
  /** Called after every changed key is persisted; the argument is the set of
   * changed keys that only take effect after a server restart. */
  onSaved: (restartKeys: string[]) => void | Promise<void>;
  /** Injected in tests (vitest node env has no real network). */
  fetchImpl?: typeof fetch;
}

interface RoleUi {
  optionId: string; // from placementOptions(host): 'cpu' | 'gpu:<i>' | 'external'
  endpoint: string; // external endpoint text
}

export default function DeployTopology({ onSaved, fetchImpl }: DeployTopologyProps) {
  const toast = useToast();
  const [cfg, setCfg] = useState<ConfigMap>({});
  const [hosts, setHosts] = useState<HostInfo[]>([]);
  const [loaded, setLoaded] = useState(false);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');

  const [hostName, setHostName] = useState('');
  const [roleUi, setRoleUi] = useState<Record<Role, RoleUi>>({
    embed: { optionId: 'cpu', endpoint: '' },
    rerank: { optionId: 'cpu', endpoint: '' },
    synth: { optionId: 'cpu', endpoint: '' },
  });
  const [synthModel, setSynthModel] = useState('');
  const [embedModel, setEmbedModel] = useState('');
  const [embedDim, setEmbedDim] = useState('');

  // Load config + host inventory once on mount.
  useEffect(() => {
    let alive = true;
    (async () => {
      const [c, h] = await Promise.all([loadConfig({ fetchImpl }), fetchHosts(fetchImpl)]);
      if (!alive) return;
      setCfg(c);
      setHosts(h);
      setSynthModel(String(c.llm_synth_model ?? ''));
      setEmbedModel(String(c.embedding_model ?? ''));
      setEmbedDim(c.embedding_dim == null ? '' : String(c.embedding_dim));

      // Seed the host from any local role that names one, else the local host.
      // Fall back to an AVAILABLE host if the recorded one is no longer in the
      // inventory — otherwise `host` is undefined and every local role silently
      // resolves to CPU, so a Save would downgrade saved GPU placements.
      const localOrFirst = h.find((x) => x.kind === 'local')?.name || h[0]?.name || '';
      const recorded =
        String(c.llm_embed_host ?? '') ||
        String(c.llm_rerank_host ?? '') ||
        String(c.llm_synth_host ?? '');
      const seededHost = recorded && h.some((x) => x.name === recorded) ? recorded : localOrFirst;
      setHostName(seededHost);

      const ui = {} as Record<Role, RoleUi>;
      for (const { role } of ROLES) {
        const p = configToPlacement(c, role);
        ui[role] = {
          optionId: placementOptionId(p),
          endpoint: p.backend === 'external' ? p.endpoint : '',
        };
      }
      setRoleUi(ui);
      setLoaded(true);
    })();
    return () => {
      alive = false;
    };
  }, [fetchImpl]);

  const host = useMemo(() => hosts.find((h) => h.name === hostName), [hosts, hostName]);
  const options = useMemo(() => placementOptions(host), [host]);

  // Resolve a role's UI selection to a concrete Placement (host-aware).
  const resolvePlacement = useCallback(
    (role: Role): Placement => {
      const ui = roleUi[role];
      if (ui.optionId === 'external') return { backend: 'external', endpoint: ui.endpoint.trim() };
      const opt = options.find((o) => o.id === ui.optionId) ?? options[0];
      return opt.placement;
    },
    [roleUi, options],
  );

  const setRole = (role: Role, patch: Partial<RoleUi>) =>
    setRoleUi((p) => ({ ...p, [role]: { ...p[role], ...patch } }));

  // When the host changes, a previously chosen GPU may not exist — fall back to CPU.
  useEffect(() => {
    if (!loaded) return;
    setRoleUi((prev) => {
      let changed = false;
      const next = { ...prev };
      for (const { role } of ROLES) {
        const id = prev[role].optionId;
        if (id !== 'external' && !options.some((o) => o.id === id)) {
          next[role] = { ...prev[role], optionId: 'cpu' };
          changed = true;
        }
      }
      return changed ? next : prev;
    });
  }, [options, loaded]);

  async function save() {
    setSaving(true);
    setError('');

    // Build the full desired {key: value} map for the current selection. This step
    // is local-only, so kb_mode is fixed to 'local' (the KnowledgeBase step already
    // recorded the choice; re-asserting it is idempotent).
    const desired = buildDesiredConfig({
      kbMode: 'local',
      kbUrl: '',
      kbBearer: '',
      placements: { embed: resolvePlacement('embed'), rerank: resolvePlacement('rerank'), synth: resolvePlacement('synth') },
      embedModel,
      embedDim,
      synthModel,
    });

    // Persist only what changed (mirrors SetupWizard.saveStep); abort + Toast on
    // the first failure, keeping the operator on the page with input intact.
    const savedCfg: ConfigMap = { ...cfg };
    const restart = new Set<string>();
    for (const [key, value] of Object.entries(desired)) {
      const original = cfg[key] == null ? '' : String(cfg[key]);
      if (value === original) continue;
      const coerced: unknown = key === 'embedding_dim' ? (value === '' ? '' : Number(value)) : value;
      const res = await saveConfigValue(key, coerced);
      if (!res.ok) {
        setError(`Couldn’t save ${key}: ${res.error ?? 'unknown error'}`);
        toast.error(`Couldn’t save ${key}: ${res.error ?? 'unknown error'}`);
        setSaving(false);
        setCfg(savedCfg); // keep whatever already succeeded
        return;
      }
      savedCfg[key] = res.value !== undefined ? res.value : coerced;
      if (isRestartKey(key)) restart.add(key);
    }

    setCfg(savedCfg);
    setSaving(false);
    toast.success('Deploy topology saved');
    await onSaved(Array.from(restart));
  }

  if (!loaded) {
    return <div style={{ fontSize: 13, color: '#667', padding: '8px 0' }}>Loading hosts…</div>;
  }

  return (
    <div style={{ display: 'grid', gap: 16, marginBottom: 8 }}>
      <section style={{ display: 'grid', gap: 10 }}>
        <div style={sectionTitle}>LLM placement</div>
        <Field label="Host for the local LLM container">
          <select style={input} value={hostName} onChange={(e) => setHostName(e.target.value)}>
            {hosts.map((h) => (
              <option key={h.name} value={h.name}>
                {h.name} ({h.kind}){h.gpus.length ? ` · ${h.gpus.length} GPU${h.gpus.length > 1 ? 's' : ''}` : ' · CPU only'}
              </option>
            ))}
          </select>
        </Field>
        {host?.error && (
          <div style={{ fontSize: 11.5, color: '#a33' }}>Probe error on {host.name}: {host.error} — CPU still available.</div>
        )}
        <div style={{ fontSize: 11.5, color: '#778', marginTop: -2 }}>
          All GPU/CPU-placed roles run on one aimee-llm container on this host.
        </div>

        {ROLES.map(({ role, label, blurb }) => {
          const ui = roleUi[role];
          const p = resolvePlacement(role);
          const tierA = synthIsTierAOnly(role, p);
          return (
            <div key={role} style={roleCard}>
              <div style={{ fontSize: 13.5, fontWeight: 700 }}>{label}</div>
              <div style={{ fontSize: 11.5, color: '#778', marginBottom: 4 }}>{blurb}</div>
              <select style={input} value={ui.optionId} onChange={(e) => setRole(role, { optionId: e.target.value })}>
                {options.map((o) => (
                  <option key={o.id} value={o.id}>{o.label}</option>
                ))}
              </select>
              {ui.optionId === 'external' && (
                <input style={{ ...input, marginTop: 6 }} value={ui.endpoint}
                  onChange={(e) => setRole(role, { endpoint: e.target.value })}
                  placeholder={role === 'embed' ? 'https://embedder.example/v1' : 'https://llm.example/v1'} />
              )}
              {role === 'embed' && ui.optionId === 'external' && (
                <div style={{ display: 'grid', gap: 6, marginTop: 6 }}>
                  <input style={input} value={embedModel} onChange={(e) => setEmbedModel(e.target.value)} placeholder="embedding model" />
                  <input style={input} value={embedDim} onChange={(e) => setEmbedDim(e.target.value)} placeholder="embedding dim (e.g. 1024)" inputMode="numeric" />
                </div>
              )}
              {role === 'embed' && ui.optionId !== 'external' && ui.optionId !== 'off' && (
                <div style={{ fontSize: 11, color: '#889', marginTop: 4 }}>Dimension auto-detected from the tier at runtime.</div>
              )}
              {role === 'synth' && p.backend !== 'off' && ui.optionId !== 'external' && (
                <input style={{ ...input, marginTop: 6 }} value={synthModel}
                  onChange={(e) => setSynthModel(e.target.value)} placeholder="synth model" />
              )}
              {tierA && (
                <div style={{ fontSize: 11, color: '#8a5a00', marginTop: 4 }}>CPU runs the Tier-A synth model only.</div>
              )}
            </div>
          );
        })}
      </section>

      {error && (
        <div style={{ fontSize: 12.5, color: '#a33', background: '#fdeaea', border: '1px solid #f2c4c4', borderRadius: 6, padding: '8px 10px' }}>
          {error}
        </div>
      )}

      <div>
        <button style={primaryBtn} disabled={saving} onClick={save}>
          {saving ? 'Saving…' : 'Save & continue'}
        </button>
      </div>
    </div>
  );
}

const sectionTitle: React.CSSProperties = { fontSize: 14, fontWeight: 700, color: '#233' };
const roleCard: React.CSSProperties = {
  display: 'grid', gap: 2, padding: '10px 12px', borderRadius: 9, border: '1px solid #dde', background: '#fbfcfe',
};
const input: React.CSSProperties = {
  width: '100%', boxSizing: 'border-box', padding: '7px 9px', borderRadius: 6,
  border: '1px solid #ccd', fontSize: 13, fontFamily: 'ui-monospace, monospace',
};
const primaryBtn: React.CSSProperties = {
  padding: '7px 16px', borderRadius: 7, border: '1px solid #2c6', background: '#2c8f56',
  color: '#fff', cursor: 'pointer', fontSize: 13.5, fontWeight: 600,
};

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <label style={{ display: 'block' }}>
      <div style={{ fontSize: 12.5, fontWeight: 600, marginBottom: 3 }}>{label}</div>
      {children}
    </label>
  );
}
