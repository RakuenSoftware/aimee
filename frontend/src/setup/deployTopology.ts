/* Deploy-topology (wizard page 2) — pure model + config mapping. No DOM, no
 * network: the whole key-translation contract is unit-tested (deployTopology.test.ts).
 *
 * The page places three LLM roles (embedder / reranker / synthesizer) and picks
 * the knowledge-base mode. Every value it persists goes through the SAME page-2
 * config record the backend shipped in p1–p2b (kb_mode + per-role llm_* keys),
 * which `aimee config deploy-env` already translates to compose env. All local
 * roles share ONE aimee-llm container; the tier tokens cpu/small/mid/large are
 * exactly the valid AIMEE_LLM_TIER values, and backend local|external|off maps
 * to AIMEE_LLM_<ROLE>_MODE. See deploy/compose + src/config_database.c. */

export type Role = 'embed' | 'rerank' | 'synth';

export const ROLES: { role: Role; label: string; blurb: string }[] = [
  { role: 'embed', label: 'Embedder', blurb: 'Turns text into vectors for search + memory.' },
  { role: 'rerank', label: 'Reranker', blurb: 'Re-scores retrieved candidates for relevance.' },
  { role: 'synth', label: 'Synthesizer', blurb: 'Writes the knowledge-base curation + summaries.' },
];

/** A local LLM tier. cpu = no GPU; small/mid/large size the co-hosted model to
 * the card. These are the literal AIMEE_LLM_TIER tokens. */
export type Tier = 'cpu' | 'small' | 'mid' | 'large';
export const GPU_TIERS: Tier[] = ['small', 'mid', 'large'];

/** How a role is served. `local` runs on the shared aimee-llm container at a
 * tier on a chosen host+GPU; `external` points at an existing endpoint; `off`
 * disables the role. */
export type Placement =
  | { backend: 'local'; tier: Tier; host: string; gpu: string }
  | { backend: 'external'; endpoint: string }
  | { backend: 'off' };

export type KbMode = 'local' | 'remote';

/** Map a card's VRAM to the largest tier it can host. Documented sizing:
 * small≈16 GB, mid≈24 GB, large≈32 GB (deploy/compose/aimee.gpu.yaml). A little
 * headroom is allowed under each floor; a GPU smaller than `small` still labels
 * `small` (best effort — the operator can drop to cpu). */
export function vramToTier(vram_mb: number): Tier {
  const gb = vram_mb / 1024;
  if (gb >= 30) return 'large';
  if (gb >= 22) return 'mid';
  return 'small';
}

export interface RoleKeys {
  backend: string;
  host: string;
  gpu: string;
  tier: string;
  /** External endpoint key. The embed role reuses `embedding_endpoint` (there is
   * no `llm_embed_endpoint`); rerank/synth have their own. */
  endpoint: string;
}

export function roleKeys(role: Role): RoleKeys {
  return {
    backend: `llm_${role}_backend`,
    host: `llm_${role}_host`,
    gpu: `llm_${role}_gpu`,
    tier: `llm_${role}_tier`,
    endpoint: role === 'embed' ? 'embedding_endpoint' : `llm_${role}_endpoint`,
  };
}

/** The synthesizer on CPU can only serve the Tier-A model (the GPU tiers add
 * Tier-B); the UI constrains the synth model + hides larger tiers accordingly.
 * (deploy/smoothnas/aimee-kb.plugin.yaml: "CPU tier = Tier-A synth".) */
export function synthIsTierAOnly(role: Role, p: Placement): boolean {
  return role === 'synth' && p.backend === 'local' && p.tier === 'cpu';
}

/** Every page-2 key this page may write, per role — used by a test to assert the
 * mapping never emits an off-allowlist key. */
export const ALL_ROLE_KEYS: string[] = ROLES.flatMap(({ role }) => {
  const k = roleKeys(role);
  return [k.backend, k.host, k.gpu, k.tier, k.endpoint];
});

/** Translate a role's placement selection into the {key: value} config map to
 * persist. Fields not relevant to the chosen backend are cleared ('') so a
 * switch (e.g. local→external) never leaves a stale host/tier/endpoint behind. */
export function placementToConfig(role: Role, p: Placement): Record<string, string> {
  const k = roleKeys(role);
  const base = { [k.backend]: '', [k.host]: '', [k.gpu]: '', [k.tier]: '', [k.endpoint]: '' };
  if (p.backend === 'local') {
    return { ...base, [k.backend]: 'local', [k.tier]: p.tier, [k.host]: p.host, [k.gpu]: p.gpu };
  }
  if (p.backend === 'external') {
    return { ...base, [k.backend]: 'external', [k.endpoint]: p.endpoint };
  }
  return { ...base, [k.backend]: 'off' };
}

/** The operator's full page-2 selection, independent of the DOM. */
export interface DeploySelection {
  kbMode: KbMode;
  kbUrl: string;
  kbBearer: string;
  placements: Record<Role, Placement>;
  embedModel: string;
  embedDim: string;
}

/** Build the complete {key: value} config map a selection would persist. A
 * remote KB deploys nothing locally, so ONLY the kb_* keys are written (the LLM
 * placement is skipped, mirroring `deploy-env`'s early return). Otherwise every
 * role's placement is emitted, plus the external-embedder model/dim. The synth
 * model is not configurable — it is fixed by the placement tier (cpu/small/mid/
 * large). Pure — the component saves only the keys that changed vs config. */
export function buildDesiredConfig(sel: DeploySelection): Record<string, string> {
  const out: Record<string, string> = { kb_mode: sel.kbMode };
  if (sel.kbMode === 'remote') {
    out.kb_client_url = sel.kbUrl.trim();
    out.kb_client_bearer_token = sel.kbBearer.trim();
    return out;
  }
  for (const { role } of ROLES) {
    const p = sel.placements[role];
    Object.assign(out, placementToConfig(role, p));
    if (role === 'embed' && p.backend === 'external') {
      out.embedding_model = sel.embedModel.trim();
      // embedding_dim is a CFG_INT key — only emit it when set, never a blank
      // string (which would reach the int allowlist as '').
      const dim = sel.embedDim.trim();
      if (dim !== '') out.embedding_dim = dim;
    }
  }
  return out;
}

function str(cfg: Record<string, unknown>, key: string): string {
  const v = cfg[key];
  return v == null ? '' : String(v);
}

/** Recover the current placement for a role from a loaded config map. An unset
 * or `local` backend reads as local (default cpu tier) so a fresh instance lands
 * on the simplest working choice. */
export function configToPlacement(cfg: Record<string, unknown>, role: Role): Placement {
  const k = roleKeys(role);
  const backend = str(cfg, k.backend);
  if (backend === 'external') return { backend: 'external', endpoint: str(cfg, k.endpoint) };
  if (backend === 'off') return { backend: 'off' };
  const tier = str(cfg, k.tier) as Tier;
  return {
    backend: 'local',
    tier: tier === 'small' || tier === 'mid' || tier === 'large' ? tier : 'cpu',
    host: str(cfg, k.host),
    gpu: str(cfg, k.gpu),
  };
}

// --- Host inventory (GET /api/hosts) -------------------------------------
export interface HostGpu {
  index: number;
  name: string;
  vendor: string;
  vram_mb: number;
}
export interface HostInfo {
  name: string;
  kind: 'local' | 'remote';
  ip?: string;
  gpus: HostGpu[];
  error?: string;
}

/** One selectable placement option for a role, given the chosen host. */
export interface PlacementOption {
  id: string; // 'cpu' | 'gpu:<index>' | 'external'
  label: string;
  placement: Placement;
}

/** Build the per-role option list for the selected host: CPU, one per GPU (tier
 * derived from VRAM), and External. `gpu:<index>` placements carry the host name
 * so all local roles pin to the single shared container's host. */
export function placementOptions(host: HostInfo | undefined): PlacementOption[] {
  const opts: PlacementOption[] = [
    { id: 'cpu', label: 'CPU', placement: { backend: 'local', tier: 'cpu', host: host?.name ?? '', gpu: '' } },
  ];
  for (const g of host?.gpus ?? []) {
    const tier = vramToTier(g.vram_mb);
    const gb = Math.round(g.vram_mb / 1024);
    opts.push({
      id: `gpu:${g.index}`,
      label: `${g.name} · ${gb} GB (${tier})`,
      placement: { backend: 'local', tier, host: host?.name ?? '', gpu: String(g.index) },
    });
  }
  opts.push({ id: 'external', label: 'External host', placement: { backend: 'external', endpoint: '' } });
  // `off` must be a first-class option: without it a role already stored as
  // backend='off' has no matching <select> entry, renders as the first option
  // (cpu), and the next save silently re-enables it as local cpu.
  opts.push({ id: 'off', label: 'Off (disabled)', placement: { backend: 'off' } });
  return opts;
}

/** Which option id a placement currently corresponds to (for selecting the right
 * control). A local GPU placement matches `gpu:<gpu>`; local cpu → 'cpu'. */
export function placementOptionId(p: Placement): string {
  if (p.backend === 'external') return 'external';
  if (p.backend === 'off') return 'off';
  return p.tier === 'cpu' && !p.gpu ? 'cpu' : `gpu:${p.gpu}`;
}
