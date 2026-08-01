/* Deploy topology (wizard page 2) — pure model + config mapping. No DOM, no
 * network: the whole key-translation contract is unit-tested (deployTopology.test.ts).
 *
 * The page makes two choices — which EMBEDDER turns text into vectors, and which
 * SYNTHESIS model writes curation and summaries — and picks the knowledge-base
 * mode. Everything it persists goes through /api/config/set, and
 * `aimee config deploy-env` translates it to container env.
 *
 * This used to place LLM "roles" onto a shared aimee-llm container: each role had
 * a backend (local/external/off), a tier (cpu/small/mid/large), a host and a GPU.
 * That container is retired. The aimee-kb IMAGE VARIANT now encodes what used to
 * be placement — which embedder is baked in, and whether llama.cpp ships with it —
 * so there is no tier to size and no host to choose at wizard time. What remains
 * is a model choice per role, plus an endpoint when the model is someone else's. */

export type KbMode = 'local' | 'remote';

/** One selectable embedder, as GET /api/embedders reports it. Every field here changes
 * the vectors, which is why the picker shows them rather than just a name. */
export interface EmbedderChoice {
  id: string;
  dim: number;
  context: number;
  pooling: string;
  /** Whether this image can serve the model itself — i.e. whether it is the baked one. */
  local: boolean;
  prefixed: boolean;
}

/** What changing the embedder costs, so the UI can say it plainly instead of letting the
 * kb refuse at next boot.
 *
 *   none            — nothing to do (first choice, or the same model again).
 *   reembed         — same width, different vector space. Pooling and prefixes are part
 *                     of the space, so the stored vectors are not comparable to new
 *                     queries even though the columns fit. The kb's serving_id guard
 *                     refuses to start until the corpus is rebuilt.
 *   reembed+schema  — different width. The pgvector columns themselves must be rebuilt,
 *                     on top of the re-embed. This is the 384 (bekko) vs 768 (nomic)
 *                     case, and it is why they are separate images rather than a
 *                     runtime switch.
 *
 * `current` empty means a fresh install: there is no corpus to invalidate, so the choice
 * is free. That is the proxy this UI uses for "populated" — it cannot see row counts, and
 * an embedder that was never recorded cannot have embedded anything. */
export type EmbedderChangeImpact = 'none' | 'reembed' | 'reembed+schema';

export function embedderChangeImpact(
  current: string,
  next: string,
  catalog: EmbedderChoice[],
): EmbedderChangeImpact {
  const from = (current ?? '').trim();
  const to = (next ?? '').trim();
  if (!from || !to || from === to) return 'none';
  const dimOf = (id: string) => catalog.find((e) => e.id === id)?.dim ?? 0;
  const a = dimOf(from);
  const b = dimOf(to);
  // Unknown width on either side: assume the cheaper-to-state of the two costs rather
  // than promising a schema rebuild we cannot confirm is needed.
  if (a > 0 && b > 0 && a !== b) return 'reembed+schema';
  return 'reembed';
}

// --- the embedder choice -------------------------------------------------

/** How the embedder is served.
 *
 *   bundled  — the model baked into this image variant (bekko-a25m at 384 on
 *              aimee-kb / aimee-kb-llm, nomic-v2 at 768 on the -nomic variants).
 *   external — an operator-run endpoint. Its width cannot be derived, so `dims`
 *              is required; anything up to EMBED_MAX_DIM (4000, the DB2 column
 *              ceiling) is valid. */
export type EmbedderSelection =
  | { kind: 'bundled'; model: string }
  | { kind: 'external'; endpoint: string; apiKey: string; dims: string };

// --- the synthesis choice ------------------------------------------------

/** A locally-servable synthesis model. Numbers are extraction F1 on the 69-note
 * gold set at Q8_0 (docs/SYNTHESIS_MODELS.md); the shipped default is Q4_K_M,
 * which was not measured for quality. Only the E2B/E4B gap is statistically
 * resolved by that set — see the paired bootstrap in the same doc — so these must
 * not be presented as a fine-grained ranking. */
export interface SynthesisModelChoice {
  id: string;
  label: string;
  blurb: string;
}

export const SYNTHESIS_MODELS: SynthesisModelChoice[] = [
  {
    id: 'gemma-4-E4B-it',
    label: 'gemma-4-E4B-it (recommended)',
    blurb: 'The better model. ~7.5 GB of weights, ~3.3 tok/s on 8 CPU threads.',
  },
  {
    id: 'gemma-4-E2B-it',
    label: 'gemma-4-E2B-it (small box)',
    blurb: 'Roughly half the memory and about twice the CPU speed, measurably weaker.',
  },
];

/** How synthesis is served.
 *
 *   off      — no synthesis. A SUPPORTED state, not an error: embedding, search,
 *              recall and indexing never call this endpoint.
 *   bundled  — gemma-4 running beside the kb, which needs an image variant that
 *              ships llama.cpp. The entrypoint fetches the weights onto the
 *              persistent volume on first start and serves them at loopback.
 *   external — any OpenAI-compatible endpoint. Best quality, no local GPU or RAM
 *              cost, and your notes leave the machine. */
export type SynthesisSelection =
  | { kind: 'off' }
  | { kind: 'bundled'; model: string }
  | { kind: 'external'; endpoint: string; apiKey: string };

/** Every config key this page may write — used by a test to assert the mapping
 * never emits an off-allowlist key. */
export const ALL_TOPOLOGY_KEYS: string[] = [
  'embedder_model',
  'embedder_url',
  'embedder_api_key',
  'embedder_dims',
  'synthesis_endpoint',
  'synthesis_model',
  'synthesis_api_key',
];

/** Translate the embedder selection into the {key: value} map to persist. Keys
 * not relevant to the chosen kind are cleared ('') so switching never leaves a
 * stale endpoint or pinned width behind — a stale embedder_dims disagreeing with
 * a bundled model is exactly the pin that fails startup. */
export function embedderToConfig(sel: EmbedderSelection): Record<string, string> {
  const base: Record<string, string> = {
    embedder_model: '',
    embedder_url: '',
    embedder_api_key: '',
  };
  if (sel.kind === 'bundled') {
    // The identity is written even though the model is baked in: it is the registry
    // key the kb resolves pooling and prefixes from, and the value it records against
    // the corpus. Leaving it blank is what made the vector-space guard a no-op.
    // embedder_dims is deliberately NOT emitted — a bundled model declares its own
    // width and the kb derives it (pinned > recorded > probed). Pinning here would
    // only create a second place to be wrong.
    return { ...base, embedder_model: sel.model.trim() };
  }
  const out: Record<string, string> = {
    ...base,
    embedder_url: sel.endpoint.trim(),
    embedder_api_key: sel.apiKey.trim(),
  };
  // embedder_dims is a CFG_INT key — only emit it when set, never a blank string
  // (which would reach the int allowlist as '').
  const dims = sel.dims.trim();
  if (dims !== '') out.embedder_dims = dims;
  return out;
}

/** Translate the synthesis selection into the {key: value} map to persist.
 *
 * A bundled model writes synthesis_model and leaves synthesis_endpoint EMPTY: the
 * container entrypoint starts llama-server and sets the loopback endpoint itself.
 * Writing a 127.0.0.1 URL here would hardcode a port the entrypoint owns. */
export function synthesisToConfig(sel: SynthesisSelection): Record<string, string> {
  const base: Record<string, string> = {
    synthesis_endpoint: '',
    synthesis_model: '',
    synthesis_api_key: '',
  };
  if (sel.kind === 'off') return base;
  if (sel.kind === 'bundled') return { ...base, synthesis_model: sel.model.trim() };
  return {
    ...base,
    synthesis_endpoint: sel.endpoint.trim(),
    synthesis_api_key: sel.apiKey.trim(),
  };
}

/** The operator's full page-2 selection, independent of the DOM. */
export interface DeploySelection {
  kbMode: KbMode;
  kbUrl: string;
  kbBearer: string;
  embedder: EmbedderSelection;
  synthesis: SynthesisSelection;
}

/** Build the complete {key: value} config map a selection would persist. A remote
 * KB deploys nothing locally, so ONLY the kb_* keys are written, mirroring
 * `deploy-env`'s early return. Pure — the component saves only what changed. */
export function buildDesiredConfig(sel: DeploySelection): Record<string, string> {
  const out: Record<string, string> = { kb_mode: sel.kbMode };
  if (sel.kbMode === 'remote') {
    out.kb_client_url = sel.kbUrl.trim();
    out.kb_client_bearer_token = sel.kbBearer.trim();
    return out;
  }
  Object.assign(out, embedderToConfig(sel.embedder));
  Object.assign(out, synthesisToConfig(sel.synthesis));
  return out;
}

function str(cfg: Record<string, unknown>, key: string): string {
  const v = cfg[key];
  return v == null ? '' : String(v);
}

/** Recover the embedder selection from a loaded config map. A non-empty URL IS
 * the external embedder — there is no separate backend selector that could
 * disagree with it, which is how "external with an empty URL" used to become a
 * silently dead configuration. */
export function configToEmbedder(cfg: Record<string, unknown>): EmbedderSelection {
  const url = str(cfg, 'embedder_url').trim();
  if (url) {
    return {
      kind: 'external',
      endpoint: url,
      apiKey: str(cfg, 'embedder_api_key'),
      dims: str(cfg, 'embedder_dims'),
    };
  }
  return { kind: 'bundled', model: str(cfg, 'embedder_model') };
}

/** Recover the synthesis selection. Empty endpoint AND empty model is 'off',
 * which is a real configured state rather than a missing one. */
export function configToSynthesis(cfg: Record<string, unknown>): SynthesisSelection {
  const endpoint = str(cfg, 'synthesis_endpoint').trim();
  if (endpoint) {
    return { kind: 'external', endpoint, apiKey: str(cfg, 'synthesis_api_key') };
  }
  const model = str(cfg, 'synthesis_model').trim();
  if (model) return { kind: 'bundled', model };
  return { kind: 'off' };
}

/** Whether this running image ships llama.cpp, i.e. whether the bundled synthesis
 * options can work at all. The image records AIMEE_WITH_LLAMACPP as ENV in every
 * variant precisely so this is observable rather than guessed: offering "run
 * gemma-4 locally" on an image without llama.cpp is an option that cannot work,
 * and the failure would appear later as synthesis silently never starting.
 *
 * Unknown (key absent) is treated as NOT available: better to point an operator at
 * an external endpoint that works than at a local model that never loads. */
export function imageHasLlamaCpp(cfg: Record<string, unknown>): boolean {
  const v = str(cfg, 'aimee_with_llamacpp').trim();
  return v === '1' || v.toLowerCase() === 'true';
}
