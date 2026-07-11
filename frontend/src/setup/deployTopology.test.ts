import { describe, it, expect } from 'vitest';
import {
  vramToTier,
  roleKeys,
  placementToConfig,
  configToPlacement,
  synthIsTierAOnly,
  placementOptions,
  placementOptionId,
  buildDesiredConfig,
  ALL_ROLE_KEYS,
  ROLES,
  type Placement,
  type HostInfo,
  type DeploySelection,
} from './deployTopology';

/* The page-2 keys the server's /api/config/set allowlist accepts (mirrors
 * src/config_fields.c). Any key placementToConfig emits MUST be in here, or the
 * wizard would POST an un-settable key. */
const ALLOWLISTED = new Set<string>([
  'kb_mode', 'kb_client_url', 'kb_client_bearer_token',
  'llm_embed_backend', 'llm_embed_host', 'llm_embed_gpu', 'llm_embed_tier',
  'llm_rerank_backend', 'llm_rerank_host', 'llm_rerank_gpu', 'llm_rerank_tier', 'llm_rerank_endpoint',
  'llm_synth_backend', 'llm_synth_host', 'llm_synth_gpu', 'llm_synth_tier', 'llm_synth_endpoint',
  'llm_synth_model',
  'embedding_endpoint', 'embedding_model', 'embedding_dim',
]);

describe('vramToTier', () => {
  it('sizes a card to the largest tier it can host', () => {
    expect(vramToTier(16 * 1024)).toBe('small');
    expect(vramToTier(24 * 1024)).toBe('mid'); // e.g. 7900 XTX
    expect(vramToTier(32 * 1024)).toBe('large');
    expect(vramToTier(8 * 1024)).toBe('small'); // tiny GPU still labels small
  });
});

describe('roleKeys', () => {
  it('embed reuses embedding_endpoint; rerank/synth have their own', () => {
    expect(roleKeys('embed').endpoint).toBe('embedding_endpoint');
    expect(roleKeys('rerank').endpoint).toBe('llm_rerank_endpoint');
    expect(roleKeys('synth').endpoint).toBe('llm_synth_endpoint');
    expect(roleKeys('synth').backend).toBe('llm_synth_backend');
  });
});

describe('placementToConfig only emits allowlisted keys', () => {
  it('every produced key is settable via /api/config/set', () => {
    const samples: Placement[] = [
      { backend: 'local', tier: 'mid', host: 'smoothnas', gpu: '0' },
      { backend: 'external', endpoint: 'https://e' },
      { backend: 'off' },
    ];
    for (const { role } of ROLES) {
      for (const p of samples) {
        for (const k of Object.keys(placementToConfig(role, p))) {
          expect(ALLOWLISTED.has(k), `${role}: ${k} not allowlisted`).toBe(true);
        }
      }
    }
    for (const k of ALL_ROLE_KEYS) expect(ALLOWLISTED.has(k)).toBe(true);
  });
});

describe('placementToConfig clears stale sibling fields', () => {
  it('external clears tier/host/gpu; local clears endpoint; off clears all', () => {
    expect(placementToConfig('rerank', { backend: 'external', endpoint: 'https://r' })).toEqual({
      llm_rerank_backend: 'external', llm_rerank_endpoint: 'https://r',
      llm_rerank_tier: '', llm_rerank_host: '', llm_rerank_gpu: '',
    });
    expect(placementToConfig('embed', { backend: 'local', tier: 'large', host: 'h', gpu: '1' })).toEqual({
      llm_embed_backend: 'local', llm_embed_tier: 'large', llm_embed_host: 'h', llm_embed_gpu: '1',
      embedding_endpoint: '',
    });
    expect(placementToConfig('synth', { backend: 'off' })).toEqual({
      llm_synth_backend: 'off', llm_synth_tier: '', llm_synth_host: '', llm_synth_gpu: '', llm_synth_endpoint: '',
    });
  });
});

describe('configToPlacement round-trips placementToConfig', () => {
  const cases: { role: Parameters<typeof roleKeys>[0]; p: Placement }[] = [
    { role: 'embed', p: { backend: 'local', tier: 'large', host: 'box', gpu: '0' } },
    { role: 'rerank', p: { backend: 'local', tier: 'cpu', host: 'box', gpu: '' } },
    { role: 'synth', p: { backend: 'external', endpoint: 'https://s' } },
    { role: 'synth', p: { backend: 'off' } },
  ];
  for (const { role, p } of cases) {
    it(`${role} ${p.backend}`, () => {
      expect(configToPlacement(placementToConfig(role, p), role)).toEqual(p);
    });
  }

  it('an unset backend defaults to local cpu (simplest working choice)', () => {
    expect(configToPlacement({}, 'embed')).toEqual({ backend: 'local', tier: 'cpu', host: '', gpu: '' });
  });
});

describe('synthIsTierAOnly', () => {
  it('is true only for the synthesizer on CPU', () => {
    expect(synthIsTierAOnly('synth', { backend: 'local', tier: 'cpu', host: 'h', gpu: '' })).toBe(true);
    expect(synthIsTierAOnly('synth', { backend: 'local', tier: 'mid', host: 'h', gpu: '0' })).toBe(false);
    expect(synthIsTierAOnly('embed', { backend: 'local', tier: 'cpu', host: 'h', gpu: '' })).toBe(false);
    expect(synthIsTierAOnly('synth', { backend: 'external', endpoint: 'x' })).toBe(false);
  });
});

describe('placementOptions / placementOptionId', () => {
  const host: HostInfo = {
    name: 'smoothnas', kind: 'local',
    gpus: [{ index: 0, name: 'AMD 7900 XTX', vendor: 'amd', vram_mb: 24 * 1024 }],
  };
  it('offers CPU, each GPU (tier from VRAM), External, and Off; GPU pins the host', () => {
    const opts = placementOptions(host);
    expect(opts.map((o) => o.id)).toEqual(['cpu', 'gpu:0', 'external', 'off']);
    const gpu = opts.find((o) => o.id === 'gpu:0')!;
    expect(gpu.placement).toEqual({ backend: 'local', tier: 'mid', host: 'smoothnas', gpu: '0' });
    expect(gpu.label).toContain('mid');
  });
  it('placementOptionId selects the matching control (incl. off, so it round-trips)', () => {
    expect(placementOptionId({ backend: 'local', tier: 'cpu', host: 'h', gpu: '' })).toBe('cpu');
    expect(placementOptionId({ backend: 'local', tier: 'mid', host: 'h', gpu: '0' })).toBe('gpu:0');
    expect(placementOptionId({ backend: 'external', endpoint: 'x' })).toBe('external');
    expect(placementOptionId({ backend: 'off' })).toBe('off');
    // The 'off' id resolves back to an off placement via the options list, so a
    // stored off role is never silently re-enabled as cpu.
    const off = placementOptions(host).find((o) => o.id === 'off')!;
    expect(off.placement).toEqual({ backend: 'off' });
  });
  it('degrades to CPU + External + Off when a host has no GPUs', () => {
    expect(placementOptions({ name: 'l', kind: 'local', gpus: [] }).map((o) => o.id)).toEqual(['cpu', 'external', 'off']);
  });
});

/* The three example topologies the operator described — assert the exact keys
 * that would be persisted match the shipped deploy-env contract. */
describe('traced example topologies', () => {
  it('large embedder + cpu reranker + external synth', () => {
    const written = {
      ...placementToConfig('embed', { backend: 'local', tier: 'large', host: 'box', gpu: '0' }),
      ...placementToConfig('rerank', { backend: 'local', tier: 'cpu', host: 'box', gpu: '' }),
      ...placementToConfig('synth', { backend: 'external', endpoint: 'https://synth' }),
    };
    expect(written.llm_embed_backend).toBe('local');
    expect(written.llm_embed_tier).toBe('large');
    expect(written.llm_rerank_backend).toBe('local');
    expect(written.llm_rerank_tier).toBe('cpu');
    expect(written.llm_synth_backend).toBe('external');
    expect(written.llm_synth_endpoint).toBe('https://synth');
  });

  it('cpu embedder + mid reranker + small synth (one shared local container)', () => {
    const written = {
      ...placementToConfig('embed', { backend: 'local', tier: 'cpu', host: 'box', gpu: '' }),
      ...placementToConfig('rerank', { backend: 'local', tier: 'mid', host: 'box', gpu: '0' }),
      ...placementToConfig('synth', { backend: 'local', tier: 'small', host: 'box', gpu: '0' }),
    };
    expect(written.llm_embed_tier).toBe('cpu');
    expect(written.llm_rerank_tier).toBe('mid');
    expect(written.llm_synth_tier).toBe('small');
    // all local → same host (single aimee-llm container)
    expect(written.llm_embed_host).toBe('box');
    expect(written.llm_rerank_host).toBe('box');
    expect(written.llm_synth_host).toBe('box');
  });
});

describe('buildDesiredConfig (the full save map)', () => {
  const localSel = (over: Partial<DeploySelection> = {}): DeploySelection => ({
    kbMode: 'local',
    kbUrl: '',
    kbBearer: '',
    placements: {
      embed: { backend: 'local', tier: 'large', host: 'box', gpu: '0' },
      rerank: { backend: 'local', tier: 'cpu', host: 'box', gpu: '' },
      synth: { backend: 'external', endpoint: 'https://synth' },
    },
    embedModel: '',
    embedDim: '',
    synthModel: 'aimee-synth',
    ...over,
  });

  it('local KB writes kb_mode + every role placement + synth model', () => {
    const m = buildDesiredConfig(localSel());
    expect(m.kb_mode).toBe('local');
    expect(m.llm_embed_backend).toBe('local');
    expect(m.llm_embed_tier).toBe('large');
    expect(m.llm_rerank_tier).toBe('cpu');
    expect(m.llm_synth_backend).toBe('external');
    expect(m.llm_synth_endpoint).toBe('https://synth');
    expect(m.llm_synth_model).toBe('aimee-synth');
    // no external embedder → no embedding_model/dim written
    expect('embedding_model' in m).toBe(false);
  });

  it('external embedder writes embedding_endpoint + model + dim', () => {
    const m = buildDesiredConfig(
      localSel({
        placements: {
          embed: { backend: 'external', endpoint: 'https://emb' },
          rerank: { backend: 'off' },
          synth: { backend: 'off' },
        },
        embedModel: 'pplx-embed',
        embedDim: '1024',
      }),
    );
    expect(m.embedding_endpoint).toBe('https://emb');
    expect(m.embedding_model).toBe('pplx-embed');
    expect(m.embedding_dim).toBe('1024');
  });

  it('a blank embedding_dim is omitted (never POSTed as "" to the int field)', () => {
    const m = buildDesiredConfig(
      localSel({
        placements: {
          embed: { backend: 'external', endpoint: 'https://emb' },
          rerank: { backend: 'off' },
          synth: { backend: 'off' },
        },
        embedModel: 'e',
        embedDim: '   ',
      }),
    );
    expect('embedding_dim' in m).toBe(false);
    expect(m.embedding_model).toBe('e');
  });

  it('remote KB writes ONLY kb_* and skips all LLM placement (section B hidden)', () => {
    const m = buildDesiredConfig({
      ...localSel(),
      kbMode: 'remote',
      kbUrl: 'https://kb.example:8760',
      kbBearer: 'secret',
    });
    expect(m).toEqual({
      kb_mode: 'remote',
      kb_client_url: 'https://kb.example:8760',
      kb_client_bearer_token: 'secret',
    });
    expect(Object.keys(m).some((k) => k.startsWith('llm_'))).toBe(false);
  });
});
