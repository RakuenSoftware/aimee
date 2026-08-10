/* provider_model.h: one model as a provider describes it.
 *
 * The record a provider's model-list fetch returns, and the row the db1
 * catalog caches. Previously both sides carried a bare `char *` model id and
 * discarded everything else the provider published, which is why per-model
 * limits had to come from a bundled third-party snapshot that went stale.
 *
 * EVERY numeric field distinguishes "the provider published this" from "the
 * provider said nothing". 0 means UNKNOWN, never zero-capacity: a provider that
 * lists model ids and no limits (most OpenAI-compatible endpoints do exactly
 * that) must leave the operator's declared value standing rather than overwrite
 * it with a confident-looking zero. Consumers that need a number and find 0 are
 * expected to fall back to operator config, or to treat the value as unknown --
 * both of which are already modelled states elsewhere in the tree.
 *
 * Deliberately a flat struct of scalars: db1's domain API takes no backend
 * types, and this crosses that boundary. */
#ifndef DEC_PROVIDER_MODEL_H
#define DEC_PROVIDER_MODEL_H 1

#define PROVIDER_MODEL_ID_MAX   128
#define PROVIDER_MODEL_NAME_MAX 64

typedef struct
{
   char id[PROVIDER_MODEL_ID_MAX];
   /* Human label when the provider publishes one ("Claude Sonnet 5"). Empty
    * means none; callers fall back to `id`. */
   char display_name[PROVIDER_MODEL_NAME_MAX];
   int context_window; /* input token limit; 0 = provider did not publish */
   int max_output;     /* output token ceiling; 0 = provider did not publish */
   /* MODEL_CAP_* bitmask (model_registry.h). 0 = the provider published no
    * capability information -- NOT "supports nothing". */
   unsigned caps;
   /* 1 when the provider explicitly marks the model deprecated/retired. A
    * provider that simply lists a model says nothing either way, so absent
    * stays 0 and the operator's own judgement is what retires a model. */
   int deprecated;
} provider_model_t;

#endif /* DEC_PROVIDER_MODEL_H */
