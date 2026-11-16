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

   void db1_model_catalog_free(provider_model_t *models, int n);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_MODEL_CATALOG_H */
