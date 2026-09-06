/* models_dev.h: models.dev registry ingestion (offline-first snapshot + cache refresh) */
#ifndef DEC_MODELS_DEV_H
#define DEC_MODELS_DEV_H 1

#include "model_registry.h"

/*
 * Fetch and cache the models.dev registry snapshot.
 * Downloads https://models.dev/api.json to ~/.cache/aimee/models_dev.json.
 * Returns 0 on success, -1 on error (non-fatal; callers should degrade gracefully).
 * Logs progress via aimee_log(LOG_INFO, "model_meta", ...).
 */
int models_dev_refresh(void);

/*
 * Look up per-model capability metadata from the models.dev registry.
 * Served by the Go providers module.
 *
 * Returns 1 if metadata was found, 0 otherwise.
 */
int models_dev_capability_get(const char *provider, const char *model_id, model_capability_t *out);

/*
 * Look up capability metadata from the operator overrides file only.
 * The overrides file uses the same JSON format as models_dev.json.
 * Returns 1 if found and out is populated, 0 otherwise.
 */
int models_dev_override_lookup(const char *provider, const char *model_id, model_capability_t *out);

/*
 * Look up capability metadata directly from the on-disk models.dev JSON cache
 * (~/.cache/aimee/models_dev.json), falling back to the bundled snapshot.
 * Returns 1 if found and out is populated, 0 otherwise.
 */
int models_dev_cache_lookup(const char *provider, const char *model_id, model_capability_t *out);

/*
 * Iterate all entries in the on-disk models.dev JSON cache.
 * Fills up to max entries into out[], filtered by required_flags and open_weights_only.
 * Returns the number of matching entries (may exceed max if the buffer is full).
 */
int models_dev_cache_list(model_capability_t *out, int max, unsigned required_flags,
                          int open_weights_only);

#endif /* DEC_MODELS_DEV_H */
