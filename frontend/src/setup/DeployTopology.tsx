import { useEffect, useMemo, useState } from 'react';
import { Button, useToast } from '@rakuensoftware/smoothgui';
import { loadConfig, saveConfigValue, type ConfigMap } from './configApi';
import { isRestartKey } from './wizardSteps';
import {
  SYNTHESIS_MODELS,
  buildDesiredConfig,
  configToEmbedder,
  configToSynthesis,
  embedderChangeImpact,
  type EmbedderChoice,
  type EmbedderSelection,
  type SynthesisSelection,
} from './deployTopology';

/* Wizard — Deploy topology (local knowledge base only). Two choices: which
 * EMBEDDER turns text into vectors, and which SYNTHESIS model writes curation and
 * summaries. Every write goes through the existing /api/config/set allowlist (no
 * new backend).
 *
 * There is no host or GPU picker any more. Those placed the retired aimee-llm
 * container; the aimee-kb image variant now decides which embedder is baked in and
 * whether llama.cpp ships alongside, so placement is a deployment-time fact rather
 * than a wizard question.
 *
 * The knowledge-base local/remote choice lives in the preceding KnowledgeBase step;
 * this step is only shown for kb_mode='local'.
 *
 * Self-contained like PrimaryChooser: it loads config, guards every save (Toast +
 * stay put on failure), and reports the restart-class keys it changed so the
 * wizard can list them on its summary. */

function csrf(): string {
  try {
    if (typeof window !== 'undefined') return (window as { _csrf?: string })._csrf || '';
  } catch {
    /* ignore */
  }
  return '';
}

async function fetchEmbedders(fetchImpl?: typeof fetch): Promise<EmbedderChoice[]> {
  const f = fetchImpl ?? fetch;
  try {
    const r = await f('/api/embedders', { headers: { 'X-CSRF-Token': csrf() } });
    if (!r.ok) return [];
    const d = (await r.json()) as { embedders?: EmbedderChoice[] };
    return d.embedders ?? [];
  } catch {
    // A deployment whose server predates the endpoint still gets the free-text field;
    // it just does not get a picker.
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

/** The embedder picker's two routes: a model this image bakes, or someone else's
 * endpoint. */
type EmbedderRoute = 'bundled' | 'external';
/** The synthesis picker's options, flattened for a <select>: 'off', 'external',
 * or a bundled model id. */
type SynthRoute = string;

export default function DeployTopology({ onSaved, fetchImpl }: DeployTopologyProps) {
  const toast = useToast();
  const [cfg, setCfg] = useState<ConfigMap>({});
  const [loaded, setLoaded] = useState(false);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');

  const [embedders, setEmbedders] = useState<EmbedderChoice[]>([]);
  const [embedRoute, setEmbedRoute] = useState<EmbedderRoute>('bundled');
  const [embedModel, setEmbedModel] = useState('');
  const [embedUrl, setEmbedUrl] = useState('');
  const [embedKey, setEmbedKey] = useState('');
  const [embedDims, setEmbedDims] = useState('');
  /** The embedder already recorded in config. Empty on a fresh install, which is what
   * makes the first choice free and every later change a confirmed one. */
  const [embedModelSaved, setEmbedModelSaved] = useState('');
  const [confirmText, setConfirmText] = useState('');

  const [synthRoute, setSynthRoute] = useState<SynthRoute>('off');
  const [synthUrl, setSynthUrl] = useState('');
  const [synthKey, setSynthKey] = useState('');

  useEffect(() => {
    let alive = true;
    (async () => {
      const [c, emb] = await Promise.all([loadConfig({ fetchImpl }), fetchEmbedders(fetchImpl)]);
      if (!alive) return;
      setEmbedders(emb);
      setCfg(c);

      const e = configToEmbedder(c);
      setEmbedRoute(e.kind);
      if (e.kind === 'bundled') {
        // An unset embedder_model is NOT "the deployment default" — there is no
        // default downstream. buildDesiredConfig omits the key, deploy-env omits
        // EMBEDDER_MODEL, and the kb refuses to start without one, so the deploy is
        // rejected before anything runs. Seed the shipped local model so an operator
        // who accepts the default gets a working one rather than that refusal.
        //
        // (It used to be worse: the kb served a builtin lexical embedder instead, so
        // the instance came up healthy and answered every search with keyword
        // matching, never using the embedder its image was built around.)
        //
        // embedModelSaved keeps the RAW saved value, so embedderChangeImpact still
        // sees from:'' on a fresh install and charges nothing for this seeding.
        setEmbedModel(e.model || emb.find((x) => x.local)?.id || '');
        setEmbedModelSaved(e.model);
      } else {
        setEmbedUrl(e.endpoint);
        setEmbedKey(e.apiKey);
        setEmbedDims(e.dims);
        setEmbedModelSaved('');
      }

      const s = configToSynthesis(c);
      if (s.kind === 'off') setSynthRoute('off');
      else if (s.kind === 'external') {
        setSynthRoute('external');
        setSynthUrl(s.endpoint);
        setSynthKey(s.apiKey);
      } else setSynthRoute(s.model);

      setLoaded(true);
    })();
    return () => {
      alive = false;
    };
  }, [fetchImpl]);

  /** Only models this image can serve are offered as bundled; the rest are reachable
   * only at an external endpoint, and offering them here would produce a container
   * that refuses to boot. */
  const localEmbedders = useMemo(() => embedders.filter((e) => e.local), [embedders]);

  const embedder: EmbedderSelection = useMemo(
    () =>
      embedRoute === 'external'
        ? { kind: 'external', endpoint: embedUrl, apiKey: embedKey, dims: embedDims }
        : { kind: 'bundled', model: embedModel },
    [embedRoute, embedUrl, embedKey, embedDims, embedModel],
  );

  /* The kb refuses to start without an embedder, and a deploy with none is rejected
   * before any container runs. Saving an incomplete choice only defers that refusal to
   * a later step, so the step will not complete until the selection is a usable one. */
  const embedderChosen =
    embedRoute === 'bundled'
      ? embedModel.trim() !== ''
      : embedUrl.trim() !== '' && embedDims.trim() !== '';

  const synthesis: SynthesisSelection = useMemo(() => {
    if (synthRoute === 'off') return { kind: 'off' };
    if (synthRoute === 'external') return { kind: 'external', endpoint: synthUrl, apiKey: synthKey };
    return { kind: 'bundled', model: synthRoute };
  }, [synthRoute, synthUrl, synthKey]);

  const impact = useMemo(
    () =>
      embedderChangeImpact(embedModelSaved, embedRoute === 'bundled' ? embedModel : '', embedders),
    [embedModelSaved, embedRoute, embedModel, embedders],
  );
  const needsConfirm = impact !== 'none';
  const confirmed = confirmText.trim().toUpperCase() === 'RE-EMBED';

  async function save() {
    // Changing the embedder on a kb that already recorded one invalidates every stored
    // vector. The kb would refuse to start rather than mix spaces, so the wizard says so
    // here instead of handing the operator a broken deployment.
    if (needsConfirm && !confirmed) {
      setError(
        impact === 'reembed+schema'
          ? 'Changing embedder width rebuilds the vector columns AND re-embeds the whole corpus. Type RE-EMBED to confirm.'
          : 'Changing embedder re-embeds the whole corpus (pooling and prefixes define the vector space). Type RE-EMBED to confirm.',
      );
      return;
    }
    if (embedRoute === 'external' && embedDims.trim() === '') {
      setError(
        'An external embedder needs its dimension: the kb sizes the vector columns from it and cannot derive the width of an endpoint it does not serve.',
      );
      return;
    }
    setSaving(true);
    setError('');

    // This step is local-only, so kb_mode is fixed to 'local' (the KnowledgeBase step
    // already recorded the choice; re-asserting it is idempotent).
    const desired = buildDesiredConfig({
      kbMode: 'local',
      kbUrl: '',
      kbBearer: '',
      embedder,
      synthesis,
    });

    // Persist only what changed (mirrors SetupWizard.saveStep); abort + Toast on
    // the first failure, keeping the operator on the page with input intact.
    const savedCfg: ConfigMap = { ...cfg };
    const restart = new Set<string>();
    for (const [key, value] of Object.entries(desired)) {
      const original = cfg[key] == null ? '' : String(cfg[key]);
      if (value === original) continue;
      const coerced: unknown = key === 'embedder_dims' ? (value === '' ? '' : Number(value)) : value;
      // fetchImpl is threaded here too, not just on the load path: without it the save
      // path reaches the global fetch and cannot be exercised in a test, which is how the
      // embedder confirm gate went unverified.
      const res = await saveConfigValue(key, coerced, { fetchImpl });
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
    return <div style={{ fontSize: 13, color: 'var(--sg-text-secondary)', padding: '8px 0' }}>Loading…</div>;
  }

  return (
    <div style={{ display: 'grid', gap: 16, marginBottom: 8 }}>
      <section style={{ display: 'grid', gap: 10 }}>
        <div style={sectionTitle}>Embedder</div>
        <div style={roleCard}>
          <div style={{ fontSize: 11.5, color: 'var(--sg-text-faint)', marginBottom: 4 }}>
            Turns text into vectors. Runs inside the knowledge base unless you point it elsewhere.
          </div>
          <select
            style={input}
            aria-label="embedder"
            value={embedRoute === 'external' ? 'external' : embedModel}
            onChange={(e) => {
              const v = e.target.value;
              setConfirmText('');
              if (v === 'external') setEmbedRoute('external');
              else {
                setEmbedRoute('bundled');
                setEmbedModel(v);
              }
            }}
          >
            <option value="">(deployment default)</option>
            {localEmbedders.map((e) => (
              <option key={e.id} value={e.id}>
                {e.id} — {e.dim}-dim, {e.context} ctx{e.prefixed ? '' : ', no prefixes'}
              </option>
            ))}
            <option value="external">External endpoint</option>
          </select>

          {/* The one-way door, stated at the point of choice rather than in a tooltip. */}
          <div style={{ fontSize: 11, color: 'var(--sg-warning-dark)', marginTop: 6 }}>
            This choice is effectively permanent for this install: the database records the
            vector width and refuses to start if it changes. Moving between a 384-dim and a
            768-dim embedder later means re-embedding everything.
          </div>

          {embedRoute === 'external' && (
            <div style={{ display: 'grid', gap: 6, marginTop: 6 }}>
              <input style={input} value={embedUrl} onChange={(e) => setEmbedUrl(e.target.value)}
                placeholder="https://embedder.example" aria-label="embedder endpoint" />
              <input style={input} value={embedKey} onChange={(e) => setEmbedKey(e.target.value)}
                placeholder="API key (blank if the endpoint needs none)" aria-label="embedder api key" />
              <input style={input} value={embedDims} onChange={(e) => setEmbedDims(e.target.value)}
                placeholder="embedding dimension (required)" inputMode="numeric" aria-label="embedder dimension" />
              <div style={{ fontSize: 11, color: 'var(--sg-text-faint)' }}>
                Required: the kb cannot derive the width of an endpoint it does not serve, and it
                sizes the vector columns from this. Up to 4000.
              </div>
            </div>
          )}
          {embedRoute === 'bundled' && (
            <div style={{ fontSize: 11, color: 'var(--sg-text-faint)', marginTop: 6 }}>
              Dimension comes from the selected model; the kb derives it at runtime.
            </div>
          )}
        </div>
      </section>

      <section style={{ display: 'grid', gap: 10 }}>
        <div style={sectionTitle}>Synthesis</div>
        <div style={roleCard}>
          <div style={{ fontSize: 11.5, color: 'var(--sg-text-faint)', marginBottom: 4 }}>
            Writes the knowledge base’s curation and summaries. Search, recall and indexing work
            without it.
          </div>
          <select style={input} aria-label="synthesis" value={synthRoute}
            onChange={(e) => setSynthRoute(e.target.value)}>
            <option value="off">None — synthesis off</option>
            <option value="external">External endpoint</option>
            {/* Both models are offered unconditionally. Each is its own sidecar image
                (aimee-llm-e2b / aimee-llm-e4b) deployed beside the kb, so the kb tag
                has no bearing on whether local synthesis can run. This used to be
                gated on what the kb image baked. */}
            {SYNTHESIS_MODELS.map((m) => (
              <option key={m.id} value={m.id}>
                {m.label} — runs beside the kb
              </option>
            ))}
          </select>

          {synthRoute === 'off' && (
            <div style={{ fontSize: 11, color: 'var(--sg-text-faint)', marginTop: 6 }}>
              Supported, not a gap: embedding, search, recall and indexing never call this.
              An embedder, by contrast, is required.
            </div>
          )}
          {synthRoute !== 'off' && synthRoute !== 'external' && (
            <div style={{ fontSize: 11, color: 'var(--sg-text-faint)', marginTop: 6 }}>
              {SYNTHESIS_MODELS.find((m) => m.id === synthRoute)?.blurb}{' '}
              Deployed as a sidecar beside the kb and reached over mutual TLS. Its weights are
              baked into that image, so nothing is downloaded at deploy or at run time. Switching
              between the two models later is a container swap; the corpus is untouched.
            </div>
          )}
          {synthRoute === 'external' && (
            <div style={{ display: 'grid', gap: 6, marginTop: 6 }}>
              <input style={input} value={synthUrl} onChange={(e) => setSynthUrl(e.target.value)}
                placeholder="https://llm.example/v1" aria-label="synthesis endpoint" />
              <input style={input} value={synthKey} onChange={(e) => setSynthKey(e.target.value)}
                placeholder="API key (blank if the endpoint needs none)" aria-label="synthesis api key" />
              <div style={{ fontSize: 11, color: 'var(--sg-text-faint)' }}>
                Your notes are sent to whatever answers this URL.
              </div>
            </div>
          )}
        </div>
      </section>

      {needsConfirm && (
        <div style={{ fontSize: 12.5, color: 'var(--sg-warning-dark)', background: 'var(--sg-warning-bg)', border: '1px solid var(--sg-warning-border)', borderRadius: 6, padding: '8px 10px', display: 'grid', gap: 6 }}>
          <div>
            <strong>Changing the embedder invalidates the existing corpus.</strong>{' '}
            {embedModelSaved || '(none)'} → {(embedRoute === 'bundled' ? embedModel : '') || '(none)'}.{' '}
            {impact === 'reembed+schema'
              ? 'The widths differ, so the vector columns are rebuilt and everything is re-embedded.'
              : 'Pooling and prefixes define the vector space, so everything is re-embedded.'}{' '}
            The kb refuses to start until that is done.
          </div>
          <input style={input} value={confirmText} onChange={(e) => setConfirmText(e.target.value)}
            placeholder="type RE-EMBED to confirm" aria-label="confirm re-embed" />
        </div>
      )}
      {error && (
        <div style={{ fontSize: 12.5, color: 'var(--sg-danger)', background: 'var(--sg-danger-bg)', border: '1px solid var(--sg-danger-bg)', borderRadius: 6, padding: '8px 10px' }}>
          {error}
        </div>
      )}

      {!embedderChosen && (
        <div style={{ fontSize: 12.5, color: 'var(--sg-danger)' }}>
          {embedRoute === 'bundled'
            ? 'Choose an embedder. This image bakes none, so use an external endpoint.'
            : 'An external embedder needs both an endpoint and its dimension.'}
        </div>
      )}

      <div>
        <Button variant="primary" disabled={saving || !embedderChosen} onClick={save}>
          {saving ? 'Saving…' : 'Save & continue'}
        </Button>
      </div>
    </div>
  );
}

const sectionTitle: React.CSSProperties = { fontSize: 14, fontWeight: 700, color: 'var(--sg-text)' };
const roleCard: React.CSSProperties = {
  display: 'grid', gap: 2, padding: '10px 12px', borderRadius: 9, border: '1px solid var(--sg-border-medium)', background: 'var(--sg-surface-alt)',
};
const input: React.CSSProperties = {
  width: '100%', boxSizing: 'border-box', padding: '7px 9px', borderRadius: 6,
  border: '1px solid var(--sg-border-medium)', fontSize: 13, fontFamily: 'ui-monospace, monospace',
};
