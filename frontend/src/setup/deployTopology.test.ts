import { describe, it, expect } from 'vitest';
import {
  ALL_TOPOLOGY_KEYS,
  SYNTHESIS_MODELS,
  buildDesiredConfig,
  configToEmbedder,
  configToSynthesis,
  embedderChangeImpact,
  embedderToConfig,
  imageHasLlamaCpp,
  synthesisToConfig,
  type DeploySelection,
  type EmbedderChoice,
} from './deployTopology';

/* The page-2 keys the server's /api/config/set allowlist accepts (mirrors
 * src/modules/config/config_fields.c). Any key this page emits MUST be in here, or
 * the wizard would POST an un-settable key.
 *
 * The llm_*_backend / llm_synth_tier / llm_synth_host / llm_synth_gpu keys that used
 * to be here are GONE: they placed the retired aimee-llm container, and the aimee-kb
 * image variant now encodes that choice. */
const ALLOWLISTED = new Set<string>([
  'kb_mode', 'kb_client_url', 'kb_client_bearer_token',
  'embedder_model', 'embedder_url', 'embedder_api_key', 'embedder_dims',
  'synthesis_endpoint', 'synthesis_model', 'synthesis_api_key',
]);

const CATALOG: EmbedderChoice[] = [
  { id: 'bekko-a25m', dim: 384, context: 8192, pooling: 'mean', local: true, prefixed: false },
  { id: 'nomic-embed-text-v2-moe', dim: 768, context: 2048, pooling: 'mean', local: true, prefixed: true },
];

describe('the key allowlist', () => {
  it('every key this page can emit is settable', () => {
    for (const k of ALL_TOPOLOGY_KEYS) expect(ALLOWLISTED.has(k)).toBe(true);
  });

  it('names no retired placement key', () => {
    // The schema and the UI move together: these were deleted from config_fields.c,
    // so a UI that still emitted them would POST keys the server rejects.
    const retired = /^(llm_embed_|llm_synth_)/;
    for (const k of ALL_TOPOLOGY_KEYS) expect(retired.test(k)).toBe(false);
  });
});

describe('embedderToConfig', () => {
  it('a bundled model writes its identity and pins no width', () => {
    const out = embedderToConfig({ kind: 'bundled', model: 'bekko-a25m' });
    expect(out.embedder_model).toBe('bekko-a25m');
    expect(out.embedder_url).toBe('');
    // The kb derives width from the registry (pinned > recorded > probed). Pinning
    // here would create a second place to be wrong, and a stale pin fails startup.
    expect('embedder_dims' in out).toBe(false);
  });

  it('an external endpoint carries its width and key, and clears the model', () => {
    const out = embedderToConfig({
      kind: 'external', endpoint: 'https://emb.x/v1', apiKey: 'k', dims: '2560',
    });
    expect(out.embedder_url).toBe('https://emb.x/v1');
    expect(out.embedder_api_key).toBe('k');
    expect(out.embedder_dims).toBe('2560');
    expect(out.embedder_model).toBe('');
  });

  it('never emits a blank embedder_dims — it is a CFG_INT key', () => {
    const out = embedderToConfig({ kind: 'external', endpoint: 'https://e', apiKey: '', dims: '' });
    expect('embedder_dims' in out).toBe(false);
  });

  it('switching bundled -> external leaves no stale model behind', () => {
    const out = embedderToConfig({ kind: 'external', endpoint: 'https://e', apiKey: '', dims: '768' });
    expect(out.embedder_model).toBe('');
  });
});

describe('synthesisToConfig', () => {
  it('off clears everything — a real state, not a missing one', () => {
    const out = synthesisToConfig({ kind: 'off' });
    expect(out.synthesis_endpoint).toBe('');
    expect(out.synthesis_model).toBe('');
    expect(out.synthesis_api_key).toBe('');
  });

  it('a bundled model sets the model and leaves the endpoint EMPTY', () => {
    // The container entrypoint starts llama-server and sets the loopback endpoint
    // itself. Writing a 127.0.0.1 URL here would hardcode a port it owns.
    const out = synthesisToConfig({ kind: 'bundled', model: 'gemma-4-E4B-it' });
    expect(out.synthesis_model).toBe('gemma-4-E4B-it');
    expect(out.synthesis_endpoint).toBe('');
  });

  it('an external endpoint carries its key and clears the bundled model', () => {
    const out = synthesisToConfig({ kind: 'external', endpoint: 'https://api.x/v1', apiKey: 'sk' });
    expect(out.synthesis_endpoint).toBe('https://api.x/v1');
    expect(out.synthesis_api_key).toBe('sk');
    expect(out.synthesis_model).toBe('');
  });
});

describe('configToEmbedder', () => {
  it('a non-empty URL IS external — no separate selector to disagree with', () => {
    const sel = configToEmbedder({ embedder_url: 'https://e', embedder_dims: 1024 });
    expect(sel.kind).toBe('external');
    if (sel.kind === 'external') expect(sel.dims).toBe('1024');
  });

  it('no URL reads as the bundled model', () => {
    const sel = configToEmbedder({ embedder_model: 'bekko-a25m' });
    expect(sel).toEqual({ kind: 'bundled', model: 'bekko-a25m' });
  });
});

describe('configToSynthesis', () => {
  it('endpoint wins', () => {
    const sel = configToSynthesis({ synthesis_endpoint: 'https://a/v1', synthesis_api_key: 'k' });
    expect(sel).toEqual({ kind: 'external', endpoint: 'https://a/v1', apiKey: 'k' });
  });

  it('model with no endpoint is the bundled model', () => {
    expect(configToSynthesis({ synthesis_model: 'gemma-4-E2B-it' }))
      .toEqual({ kind: 'bundled', model: 'gemma-4-E2B-it' });
  });

  it('neither is off, not unconfigured', () => {
    expect(configToSynthesis({})).toEqual({ kind: 'off' });
  });
});

describe('embedderChangeImpact', () => {
  it('the first choice on a fresh install is free', () => {
    expect(embedderChangeImpact('', 'bekko-a25m', CATALOG)).toBe('none');
  });

  it('the same model again is free', () => {
    expect(embedderChangeImpact('bekko-a25m', 'bekko-a25m', CATALOG)).toBe('none');
  });

  it('384 -> 768 rebuilds the columns as well as the corpus', () => {
    // This is the bekko -> nomic case, and the reason they are separate images.
    expect(embedderChangeImpact('bekko-a25m', 'nomic-embed-text-v2-moe', CATALOG))
      .toBe('reembed+schema');
  });

  it('same width, different model still re-embeds', () => {
    const same: EmbedderChoice[] = [
      { id: 'a', dim: 384, context: 512, pooling: 'mean', local: true, prefixed: false },
      { id: 'b', dim: 384, context: 512, pooling: 'cls', local: true, prefixed: true },
    ];
    // Pooling and prefixes are part of the vector space even when the columns fit.
    expect(embedderChangeImpact('a', 'b', same)).toBe('reembed');
  });

  it('an unknown width does not promise a schema rebuild it cannot confirm', () => {
    expect(embedderChangeImpact('bekko-a25m', 'mystery', CATALOG)).toBe('reembed');
  });
});

describe('imageHasLlamaCpp', () => {
  it('is true only when the image says so', () => {
    expect(imageHasLlamaCpp({ aimee_with_llamacpp: '1' })).toBe(true);
    expect(imageHasLlamaCpp({ aimee_with_llamacpp: 'true' })).toBe(true);
    expect(imageHasLlamaCpp({ aimee_with_llamacpp: '0' })).toBe(false);
  });

  it('an absent key reads as NOT available', () => {
    // Better to point an operator at an external endpoint that works than at a local
    // model that never loads.
    expect(imageHasLlamaCpp({})).toBe(false);
  });
});

describe('buildDesiredConfig', () => {
  const local = (over: Partial<DeploySelection> = {}): DeploySelection => ({
    kbMode: 'local',
    kbUrl: '',
    kbBearer: '',
    embedder: { kind: 'bundled', model: 'bekko-a25m' },
    synthesis: { kind: 'off' },
    ...over,
  });

  it('a remote kb writes ONLY the kb_* keys', () => {
    const out = buildDesiredConfig({
      ...local(), kbMode: 'remote', kbUrl: 'https://kb.x', kbBearer: 't',
    });
    expect(out).toEqual({
      kb_mode: 'remote', kb_client_url: 'https://kb.x', kb_client_bearer_token: 't',
    });
    // Mirrors deploy-env's early return: a remote kb deploys nothing locally.
    expect('embedder_model' in out).toBe(false);
    expect('synthesis_endpoint' in out).toBe(false);
  });

  it('a local kb writes both roles', () => {
    const out = buildDesiredConfig(local({
      synthesis: { kind: 'bundled', model: 'gemma-4-E4B-it' },
    }));
    expect(out.kb_mode).toBe('local');
    expect(out.embedder_model).toBe('bekko-a25m');
    expect(out.synthesis_model).toBe('gemma-4-E4B-it');
  });

  it('emits no key outside the allowlist', () => {
    const cases: DeploySelection[] = [
      local(),
      local({ embedder: { kind: 'external', endpoint: 'https://e', apiKey: 'k', dims: '2560' } }),
      local({ synthesis: { kind: 'external', endpoint: 'https://s/v1', apiKey: 'sk' } }),
      local({ synthesis: { kind: 'bundled', model: 'gemma-4-E2B-it' } }),
    ];
    for (const sel of cases) {
      for (const k of Object.keys(buildDesiredConfig(sel))) {
        expect(ALLOWLISTED.has(k)).toBe(true);
      }
    }
  });
});

describe('SYNTHESIS_MODELS', () => {
  it('offers exactly the two models the entrypoint can fetch', () => {
    // deploy/container/aimee-kb-entrypoint.sh maps these ids to ggml-org GGUF repos.
    // An id offered here that the entrypoint does not know becomes a selection that
    // silently never starts, so the two lists have to agree.
    expect(SYNTHESIS_MODELS.map((m) => m.id).sort())
      .toEqual(['gemma-4-E2B-it', 'gemma-4-E4B-it']);
  });
});
