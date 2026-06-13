/* model_registry.h: model alias resolution and provider autodetection */
#ifndef DEC_MODEL_REGISTRY_H
#define DEC_MODEL_REGISTRY_H 1

#include <stddef.h>

/*
 * Model registry provides:
 *  1. Alias resolution: "opus" → provider="anthropic", model="claude-opus-4-6"
 *  2. Provider autodetection: "gpt-4o" → provider="openai"
 *  3. Capability metadata lookup: context window, tool support
 */

#define MODEL_PROVIDER_MAX 32
#define MODEL_ID_MAX       128

typedef struct
{
   char provider[MODEL_PROVIDER_MAX]; /* e.g. "anthropic", "openai", "gemini" */
   char model_id[MODEL_ID_MAX];       /* canonical model identifier */
} model_info_t;

enum
{
   MODEL_CAP_REASONING = 1 << 0,
   MODEL_CAP_TOOLS = 1 << 1,
   MODEL_CAP_VISION = 1 << 2,
   MODEL_CAP_PDF = 1 << 3,
   MODEL_CAP_AUDIO = 1 << 4,
   MODEL_CAP_STREAMING = 1 << 5,
};

typedef struct
{
   char provider[MODEL_PROVIDER_MAX];
   char model_id[MODEL_ID_MAX];
   int context_window;
   int max_output;
   double cost_in_per_mtok;
   double cost_out_per_mtok;
   unsigned flags;
   char modalities[64];
   char knowledge_cutoff[16];
   int open_weights;
   int deprecated;
} model_capability_t;

/*
 * Resolve a model alias or partial name to provider + canonical model ID.
 * Returns 1 if resolved, 0 if not found (out is unchanged).
 *
 * Examples:
 *   "opus"    → { "anthropic", "claude-opus-4-6" }
 *   "sonnet"  → { "anthropic", "claude-sonnet-4-6" }
 *   "haiku"   → { "anthropic", "claude-haiku-4-5-20251001" }
 *   "gpt4o"   → { "openai",    "gpt-4o" }
 *   "gpt4"    → { "openai",    "gpt-4-turbo" }
 *   "gemini"  → { "gemini",    "gemini-1.5-pro" }
 */
int model_alias_resolve(const char *alias, model_info_t *out);

/*
 * Autodetect provider from a full model ID string.
 * Returns provider name (static string) or NULL if unknown.
 *
 * Examples:
 *   "claude-opus-4-6"          → "anthropic"
 *   "gpt-4o"                   → "openai"
 *   "gemini-1.5-pro"           → "gemini"
 *   "mistral-7b-instruct"      → "openai"  (treat as openai-compatible)
 */
const char *model_detect_provider(const char *model_id);

/*
 * List all known aliases. Writes at most max entries.
 * Returns the total number of aliases available.
 * Useful for tab completion / help text.
 */
int model_alias_list(model_info_t *out, int max);

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

unsigned model_capability_flag_from_name(const char *name);
void model_capability_format_flags(unsigned flags, char *buf, size_t buf_len);

/*
 * List known capability entries. required_flags may be 0 or a MODEL_CAP_*
 * bitmask; open_weights_only filters to open-weights models when non-zero.
 * Returns the total matching count, even when out is NULL or max is smaller.
 */
int model_capability_list(model_capability_t *out, int max, unsigned required_flags,
                          int open_weights_only);

void model_capability_flags_string(unsigned flags, char *out, size_t out_len);

/* Reload model metadata from cache/snapshot sources. Returns the number of
 * loaded entries (or 0 on fallback/no external entries). */
int model_capability_refresh(char *msg, size_t msg_len);

#endif /* DEC_MODEL_REGISTRY_H */
