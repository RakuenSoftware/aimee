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
  embedderChangeImpact,
  rolePlaceable,
} from './deployTopology';

/* The page-2 keys the server's /api/config/set allowlist accepts (mirrors
 * src/config_fields.c). Any key placementToConfig emits MUST be in here, or the
 * wizard would POST an un-settable key. */
const ALLOWLISTED = new Set<string>([
  'kb_mode', 'kb_client_url', 'kb_client_bearer_token',
  'llm_embed_backend', 'llm_embed_host', 'llm_embed_gpu', 'llm_embed_tier',
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
  it('embed reuses embedding_endpoint; synth has its own', () => {
    expect(roleKeys('embed').endpoint).toBe('embedding_endpoint');
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
    expect(placementToConfig('synth', { backend: 'external', endpoint: 'https://r' })).toEqual({
      llm_synth_backend: 'external', llm_synth_endpoint: 'https://r',
      llm_synth_tier: '', llm_synth_host: '', llm_synth_gpu: '',
    });
    // The embedder has nothing to place: it runs inside the kb, so a local placement
    // writes the backend alone. Emitting llm_embed_tier/host/gpu would write keys that
    // no longer exist in config.
    expect(placementToConfig('embed', { backend: 'local', tier: 'large', host: 'h', gpu: '1' })).toEqual({
      llm_embed_backend: 'local', embedding_endpoint: '',
    });
    expect(placementToConfig('synth', { backend: 'off' })).toEqual({
      llm_synth_backend: 'off', llm_synth_tier: '', llm_synth_host: '', llm_synth_gpu: '', llm_synth_endpoint: '',
    });
  });
});

describe('configToPlacement round-trips placementToConfig', () => {
  const cases: { role: Parameters<typeof roleKeys>[0]; p: Placement }[] = [
    // in-container: local carries no tier/host/gpu to round-trip
    { role: 'embed', p: { backend: 'local', tier: '', host: '', gpu: '' } },
    { role: 'synth', p: { backend: 'local', tier: 'cpu', host: 'box', gpu: '' } },
    { role: 'synth', p: { backend: 'external', endpoint: 'https://s' } },
    { role: 'synth', p: { backend: 'off' } },
  ];
  for (const { role, p } of cases) {
    it(`${role} ${p.backend}`, () => {
      expect(configToPlacement(placementToConfig(role, p), role)).toEqual(p);
    });
  }

  it('an unset backend defaults to local (simplest working choice)', () => {
    // For the embedder that means in-container, with no tier to default: the kb serves
    // it, so a fresh install needs no placement decision at all.
    expect(configToPlacement({}, 'embed')).toEqual({ backend: 'local', tier: '', host: '', gpu: '' });
    expect(configToPlacement({}, 'synth')).toEqual({ backend: 'local', tier: 'cpu', host: '', gpu: '' });
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

/* The example topologies the operator described — assert the exact keys
 * that would be persisted match the shipped deploy-env contract. */
describe('traced example topologies', () => {
  it('in-container embedder + external synth', () => {
    const written = {
      ...placementToConfig('embed', { backend: 'local', tier: '', host: '', gpu: '' }),
      ...placementToConfig('synth', { backend: 'external', endpoint: 'https://synth' }),
    };
    expect(written.llm_embed_backend).toBe('local');
    expect('llm_embed_tier' in written).toBe(false);
    expect(written.llm_synth_backend).toBe('external');
    expect(written.llm_synth_endpoint).toBe('https://synth');
  });

  it('in-container embedder + local synth', () => {
    const written = {
      ...placementToConfig('embed', { backend: 'local', tier: '', host: '', gpu: '' }),
      ...placementToConfig('synth', { backend: 'local', tier: 'small', host: 'box', gpu: '0' }),
    };
    expect(written.llm_synth_tier).toBe('small');
    expect(written.llm_synth_host).toBe('box');
    // only synth names a host; the embedder is wherever the kb is
    expect('llm_embed_host' in written).toBe(false);
  });
});

describe('buildDesiredConfig (the full save map)', () => {
  const localSel = (over: Partial<DeploySelection> = {}): DeploySelection => ({
    kbMode: 'local',
    kbUrl: '',
    kbBearer: '',
    placements: {
      embed: { backend: 'local', tier: '', host: '', gpu: '' },
      synth: { backend: 'external', endpoint: 'https://synth' },
    },
    embedModel: '',
    embedDim: '',
    ...over,
  });

  it('local KB writes kb_mode + every role placement', () => {
    const m = buildDesiredConfig(localSel());
    expect(m.kb_mode).toBe('local');
    expect(m.llm_embed_backend).toBe('local');
    expect('llm_embed_tier' in m).toBe(false);
    expect(m.llm_synth_backend).toBe('external');
    expect(m.llm_synth_endpoint).toBe('https://synth');
    // the synth model is fixed by tier, never written from the wizard
    expect('llm_synth_model' in m).toBe(false);
    // no external embedder → no embedding_model/dim written
    expect('embedding_model' in m).toBe(false);
  });

  it('external embedder writes embedding_endpoint + model + dim', () => {
    const m = buildDesiredConfig(
      localSel({
        placements: {
          embed: { backend: 'external', endpoint: 'https://emb' },
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

/* The embedder picker's whole job is to state the cost of the choice before it is made,
 * so these are the assertions that keep it honest. */
describe('embedderChangeImpact', () => {
  const catalog = [
    { id: 'nomic-embed-text-v2-moe', dim: 768, context: 2048, pooling: 'mean', local: true, prefixed: true },
    { id: 'bekko-a25m', dim: 384, context: 8192, pooling: 'mean', local: true, prefixed: false },
    { id: 'same-width-other', dim: 768, context: 512, pooling: 'last', local: true, prefixed: false },
  ];

  it('a first choice costs nothing (no corpus to invalidate)', () => {
    expect(embedderChangeImpact('', 'nomic-embed-text-v2-moe', catalog)).toBe('none');
  });

  it('re-selecting the same embedder costs nothing', () => {
    expect(embedderChangeImpact('bekko-a25m', 'bekko-a25m', catalog)).toBe('none');
  });

  it('a width change needs the schema rebuilt as well as a re-embed', () => {
    // The case the operator most needs warned about: 768 -> 384 rebuilds pgvector.
    expect(embedderChangeImpact('nomic-embed-text-v2-moe', 'bekko-a25m', catalog))
      .toBe('reembed+schema');
    expect(embedderChangeImpact('bekko-a25m', 'nomic-embed-text-v2-moe', catalog))
      .toBe('reembed+schema');
  });

  it('same width but a different model still needs a re-embed', () => {
    // Width is not identity: pooling and prefixes are part of the vector space, and the
    // kb refuses to start against a corpus embedded in another one.
    expect(embedderChangeImpact('nomic-embed-text-v2-moe', 'same-width-other', catalog))
      .toBe('reembed');
  });

  it('an unknown width does not promise a schema rebuild it cannot confirm', () => {
    expect(embedderChangeImpact('nomic-embed-text-v2-moe', 'byo-unlisted', catalog))
      .toBe('reembed');
  });
});

describe('buildDesiredConfig writes the embedder identity for a LOCAL choice', () => {
  it('local embed persists embedding_model and does NOT pin the dim', () => {
    const m = buildDesiredConfig({
      kbMode: 'local', kbUrl: '', kbBearer: '',
      placements: {
        embed: { backend: 'local', tier: 'cpu', host: 'box', gpu: '' },
        synth: { backend: 'off' },
      },
      embedModel: 'bekko-a25m',
      embedDim: '',
    });
    expect(m.embedding_model).toBe('bekko-a25m');
    // The registry declares the width and the kb derives it; a second copy here is a
    // second place to be wrong.
    expect('embedding_dim' in m).toBe(false);
  });

  it('external embed still persists model + dim', () => {
    const m = buildDesiredConfig({
      kbMode: 'local', kbUrl: '', kbBearer: '',
      placements: {
        embed: { backend: 'external', endpoint: 'https://emb' },
        synth: { backend: 'off' },
      },
      embedModel: 'my-qwen3-4b',
      embedDim: '2560',
    });
    expect(m.embedding_model).toBe('my-qwen3-4b');
    expect(m.embedding_dim).toBe('2560');
  });
});

/* Every option a role is OFFERED must be an option that role can actually be GIVEN.
 *
 * This is the bug class, stated once so it cannot come back in a new form. The embedder
 * shared the synth placement list, so the UI offered "GPU 0" for a model served inside the
 * kb: selectable, plausible, and it wrote nothing. The guard is deliberately NOT "the
 * embedder has no GPU option" — a GPU-served embedder is a reasonable thing to add later.
 * It is that whatever is offered round-trips into config and back, which stays true however
 * the runtimes change. Add a placement to any role and this test covers it for free; offer
 * one that writes nothing and it fails. */
describe('every offered placement is a real placement', () => {
  const hosts: (HostInfo | undefined)[] = [
    undefined,
    { name: 'box', kind: 'local', gpus: [] },
    { name: 'box', kind: 'local', gpus: [{ index: 0, name: 'RTX 5080', vram_mb: 16384 }] },
  ];

  for (const role of ROLES.map((r) => r.role)) {
    for (const [i, host] of hosts.entries()) {
      it(`${role} options are all writable (host ${i})`, () => {
        const opts = placementOptions(host, role);
        expect(opts.length).toBeGreaterThan(0);
        const allowed = new Set(ALL_ROLE_KEYS);
        for (const opt of opts) {
          const written = placementToConfig(role, opt.placement);
          // Nothing may write a key outside the page-2 allowlist...
          for (const key of Object.keys(written)) expect(allowed.has(key)).toBe(true);
          // ...and the selection must survive a save/load cycle, which is what catches an
          // option whose distinguishing fields are silently dropped.
          expect(configToPlacement(written, role)).toEqual(opt.placement);
        }
        // Option ids must be unique, or the <select> cannot represent them all.
        expect(new Set(opts.map((o) => o.id)).size).toBe(opts.length);
      });
    }
  }

  it('every role offers at least one option', () => {
    // Zero would render an empty <select> and resolve to undefined. The COUNT is not the
    // invariant — the embedder has no tier to choose but does choose between the bundled
    // model and an operator endpoint, and a GPU-served bundle would add more. What must
    // hold is that the list is never empty and every entry round-trips (above).
    for (const role of ROLES.map((r) => r.role)) {
      expect(placementOptions(undefined, role).length).toBeGreaterThan(0);
    }
  });

  it('the embedder offers the bundled model and an external endpoint', () => {
    // The supported route above the bundled model's width is someone else's GPU, so the
    // external option has to exist — and its dimension is operator-supplied, because the
    // kb cannot derive the width of an endpoint it does not serve.
    const ids = placementOptions(undefined, 'embed').map((o) => o.id);
    expect(ids).toContain('in-container');
    expect(ids).toContain('external');
  });

  it('ALL_ROLE_KEYS never names a key the config schema does not have', () => {
    // These three were removed with the aimee-llm container. If a GPU embedder brings
    // them back, roleKeys and the C config schema have to regain them together — this
    // fails loudly if only the UI side moves.
    for (const gone of ['llm_embed_host', 'llm_embed_gpu', 'llm_embed_tier']) {
      expect(ALL_ROLE_KEYS).not.toContain(gone);
    }
  });
});
