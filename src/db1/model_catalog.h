/* db1/model_catalog.h: provider model catalog cache.
 *
 * Caches what a provider reports about its own models, per provider, with a
 * freshness stamp. The rows carry limits and capabilities, not just ids: a
 * provider that publishes them is the authoritative source for its models, and
 * caching the whole record is what removes the need for a bundled third-party
 * snapshot that ages out of date.
 *
 * Fields a provider does not publish stay 0/empty and MUST be read as "unknown",
 * not as zero-capacity — see provider_model.h. Most OpenAI-compatible endpoints
 * publish ids only, so partially-populated rows are the normal case, not an
 * error.
 *
 * Pure domain API. No backend types in any signature. */
#ifndef DEC_DB1_MODEL_CATALOG_H
#define DEC_DB1_MODEL_CATALOG_H 1

#include "provider_model.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* 1 when this provider has at least one row fetched within ttl_seconds. */
   int db1_model_catalog_is_fresh(const char *provider, int ttl_seconds);

   /* Read every cached model for a provider, ordered by id. Returns 0 and sets
    * *models_out / *n_out on success; -1 on error or when nothing is cached
    * (callers treat both as "no catalog"). Free with db1_model_catalog_free. */
   int db1_model_catalog_get(const char *provider, provider_model_t **models_out, int *n_out);

   /* Replace this provider's cached rows wholesale, in one transaction. A fetch
    * reports the provider's CURRENT model set, so replacing rather than merging
    * is what lets a withdrawn model disappear instead of lingering forever. */
   int db1_model_catalog_replace(const char *provider, const provider_model_t *models, int n);

   void db1_model_catalog_free(provider_model_t *models, int n);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_MODEL_CATALOG_H */
