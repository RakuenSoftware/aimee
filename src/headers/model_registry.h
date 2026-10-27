/* model_registry.h: model alias resolution and provider autodetection */
#ifndef DEC_MODEL_REGISTRY_H
#define DEC_MODEL_REGISTRY_H 1

#include <stddef.h>

/* Model metadata views over the provider registry owner. */

#define MODEL_PROVIDER_MAX 32
#define MODEL_ID_MAX       128

enum
{
   MODEL_CAP_REASONING = 1 << 0,
   MODEL_CAP_TOOLS = 1 << 1,
   MODEL_CAP_VISION = 1 << 2,
   MODEL_CAP_PDF = 1 << 3,
   MODEL_CAP_AUDIO = 1 << 4,
   MODEL_CAP_STREAMING = 1 << 5,
   /* The model accepts Anthropic's ADAPTIVE thinking config. Set only from a
    * provider that positively says so -- Anthropic publishes it under
    * capabilities.thinking.types.adaptive. models.dev carries a bare
    * `reasoning` boolean and cannot distinguish adaptive from the older
    * budget_tokens form, so a catalogued model leaves this CLEAR.
    *
    * Absent therefore means "not known to accept it", never "cannot reason".
    * Callers must fail closed on that: emitting the wrong thinking shape is a
    * 400 the operator sees as an agent failure, so sending nothing is strictly
    * better than guessing. */
   MODEL_CAP_THINKING_ADAPTIVE = 1 << 6,
};

/* Modality capabilities (vision/pdf/audio) are INFERRED from prompt TEXT
 * (delegate_infer_capability_requirements) — a heuristic that over-triggers on a
 * text task merely mentioning an image/pdf/audio filename. They are therefore
 * BEST-EFFORT routing preferences: when requiring them would disable every
 * candidate, the router relaxes them and routes on the hard caps (tools) +
 * min_context instead, rather than hard-failing the whole task fleet-wide. */
#define MODEL_CAP_MODALITY_SOFT (MODEL_CAP_VISION | MODEL_CAP_PDF | MODEL_CAP_AUDIO)

/* Context-band pricing. Several providers charge more once a request's context
 * exceeds a threshold — gpt-5.6-sol doubles to $10/$45 above 272k, MiniMax-M3
 * above 512k — so price is a function of (model, context size), not one number
 * per model. Treating the base rate as authoritative would display and compare a
 * large-context agent at HALF its applicable rate.
 *
 * NOTE the registry also publishes a legacy `context_over_200k` key whose NAME
 * does not encode the real threshold (sol's is 272000, MiniMax-M3's is 512000).
 * Only the structured `tiers[].tier.size` is authoritative; the legacy alias is
 * ignored. Of 5728 catalogued models, 252 publish bands: 245 with one and 7 with
 * two, all of type "context". */
#define MODEL_PRICE_BANDS_MAX 8

typedef struct
{
   /* The band applies when the request context EXCEEDS this many tokens. */
   int above_tokens;
   double in_per_mtok;
   double out_per_mtok;
   double cache_read_per_mtok;
} model_price_band_t;

typedef struct
{
   char provider[MODEL_PROVIDER_MAX];
   char model_id[MODEL_ID_MAX];
   int context_window;
   int max_output;
   double cost_in_per_mtok;
   double cost_out_per_mtok;
   /* Cache-read price ($/Mtok). Typically an order of magnitude below input, so
    * it dominates real spend on any prompt-caching workload and cannot be
    * approximated by the input rate. 0 = the source published none. */
   double cost_cache_read_per_mtok;
   /* Bands above the base rate, ascending by `above_tokens`. Empty (count 0)
    * means a single flat price at every context size. */
   model_price_band_t price_bands[MODEL_PRICE_BANDS_MAX];
   int price_band_count;
   /* 1 when the source published more bands than fit, so the schedule is
    * INCOMPLETE. Consumers must not assert a definitive price ordering from a
    * truncated schedule — the dropped bands could reverse it. */
   int price_bands_truncated;
   unsigned flags;
   char modalities[64];
   char knowledge_cutoff[16];
   /* Human-facing label from the upstream registry, e.g. "GPT-5.6 Sol" for
    * openai/gpt-5.6-sol. Empty when the source has no name (the flat snapshot
    * schema carries none); callers fall back to model_id. Lets operator-facing
    * surfaces that must name a specific model — roundtable seats, routing
    * attribution — show provider+model without hand-maintained strings. */
   char display_name[64];
   int open_weights;
   int deprecated;
} model_capability_t;

/*
 * Look up the context window size (in tokens) for a model ID.
 * Performs case-insensitive prefix matching against known models.
 * Returns context window size, or 0 if the model is unknown.
 *
 * Examples:
 *   "claude-opus-4-6"      → 200000
 *   "gpt-4o"               → 128000
 *   "gemini-1.5-pro"       → 1000000
 */
int model_context_window(const char *model_id);

/*
 * The model's output-token ceiling (max_output), used as a request's max_tokens
 * when no explicit cap was pinned by the caller or agent config. Resolves the
 * registry's per-model max_output (static table or inferred from family +
 * context window); returns a conservative fallback for an unknown model, never
 * 0. provider may be NULL/empty (inferred from the model id).
 */
int model_max_output(const char *provider, const char *model_id);

/*
 * Heuristic offline capability lookup. This is intentionally local and
 * conservative: operator/model.dev overrides can replace these values later.
 * Provider may be NULL or empty, in which case it is inferred from the model
 * id. Returns 1 when metadata was found or inferred, 0 otherwise.
 */
int model_capability_get(const char *provider, const char *model_id, model_capability_t *out);

/*
 * Resolve a user-facing model reference into provider + model + capabilities.
 * Accepts aliases ("opus"), provider-qualified refs ("openrouter:anthropic/claude-opus-4"),
 * or bare model ids ("gpt-4o").
 */
int model_capability_resolve_ref(const char *ref, char *provider, size_t provider_cap,
                                 char *model_id, size_t model_id_cap, model_capability_t *out);

void model_capability_format_flags(unsigned flags, char *buf, size_t buf_len);

/*
 * List known capability entries. required_flags may be 0 or a MODEL_CAP_*
 * bitmask; open_weights_only filters to open-weights models when non-zero.
 * Returns the total matching count, even when out is NULL or max is smaller.
 */
int model_capability_list(model_capability_t *out, int max, unsigned required_flags,
                          int open_weights_only);

#endif /* DEC_MODEL_REGISTRY_H */
