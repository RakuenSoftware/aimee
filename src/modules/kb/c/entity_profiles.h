/* kb_store/entity_profiles.h: storage primitives for entity_profiles — KB_STORE.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_KB_STORE_ENTITY_PROFILES_H
#define DEC_KB_STORE_ENTITY_PROFILES_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Insert a new entity_profiles row or replace its observation_count /
    * card_json / last_refreshed in place. Returns 0 on success. */
   int kb_store_entity_profile_upsert(const char *entity_id, const char *canonical_name,
                                      int observation_count, const char *card_json);

   /* Returns 1 if there is a row whose last_refreshed is later than the
    * supplied SQLite datetime modifier (e.g. "-3600 seconds" for the
    * last hour). Returns 0 on miss/stale, -1 on error. */
   int kb_store_entity_profile_is_fresh(const char *entity_id, const char *cutoff_modifier);

   /* Look up a stored card by entity_id (case-insensitive). Returns 0
    * on hit, -1 on miss. */
   int kb_store_entity_profile_get_card(const char *entity_id, char *out_json, size_t out_len);

   /* Count distinct memory_id rows in memory_entities for `entity_id`
    * (case-insensitive). Returns 0 on no rows / error. */
   int kb_store_entity_count_observations(const char *entity_id);

   /* List up to `max` entities with at least `min_obs` distinct memory
    * mentions. Each output slot receives the lowercased entity name in
    * `names_out[i]` (capacity 128) and the observation count in
    * `obs_out[i]`. Returns count written. */
   int kb_store_entity_list_active(int min_obs, char (*names_out)[128], int *obs_out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_ENTITY_PROFILES_H */
