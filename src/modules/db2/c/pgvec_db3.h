/* pgvec_db3.h: the seam between DB2's pgvector searches and the DB3 route.
 *
 * DB3 lets an external vector provider serve the portable half of the vector
 * contract (see src/modules/db2/eventcontract/vector-portability.json). The
 * route itself — selection, fallback, and candidate re-authorization — lives in
 * db3_route.c and knows nothing about pgvector. This file is what connects the
 * two: it owns the process's single route, and it turns a pgvec-shaped search
 * into a DB3 request and the reply back into pgvec's id/score arrays.
 *
 * Nothing here decides authority. DB3 returns opaque point ids, and DB2
 * rehydrates and re-authorizes every one of them; a provider can narrow what
 * DB2 considers, never widen it.
 */
#ifndef AIMEE_DB2_PGVEC_DB3_H
#define AIMEE_DB2_PGVEC_DB3_H 1

#include <stdint.h>

#include <aimee/db2/db3_route.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Install the process route. `internal_search` is the pgvector
    * implementation the route falls back to and uses when no provider is
    * selected; `authorize` re-checks each returned point id against DB2's
    * canonical rows. Both are required — a route that could not fall back or
    * could not authorize would be a route that must never be used.
    *
    * Idempotent: a second call replaces the callbacks and clears any selected
    * provider, because the provider was selected against the old ones.
    *
    * Returns 0 on success, -1 on invalid arguments. */
   int pgvec_db3_route_install(aimee_db3_search_fn internal_search, void *internal_context,
                               aimee_db3_candidate_authorize_fn authorize, void *authorize_context);

   /* Select an external provider to serve portable reads. `principal` must be
    * nonzero and comes from the host-authenticated bus frame, never from the
    * provider's own payload. `fallback_enabled` decides whether a provider
    * failure silently falls back to pgvector or surfaces.
    *
    * Returns 0 on success, -1 if no route is installed or the arguments are
    * invalid. */
   int pgvec_db3_route_select(uint32_t principal, int ready, int fallback_enabled,
                              aimee_db3_search_fn external_search, void *external_context);

   /* Deselect the external provider; portable reads return to pgvector. */
   void pgvec_db3_route_clear(void);

   /* 1 when an external provider is currently selected AND ready, else 0. */
   int pgvec_db3_route_serving(void);

   /* Run one memory candidate search through the route.
    *
    * Fills `ids` and `scores` with at most `max` candidates and returns the
    * count, or -1 when the route did not produce an answer (including when no
    * route is installed) so the caller runs its own pgvector query instead.
    *
    * `workspace` and `project` carry the scope. At least one must be non-empty:
    * an unscoped search cannot be expressed on the DB3 wire, and sending one
    * with both blank would ask a provider to search everything.
    *
    * This is deliberately incapable of expressing a kind filter. The DB3 search
    * request has no field for one, so a filtered search routed here would come
    * back unfiltered and look like a correct answer — callers with kind filters
    * must not use this path. */
   int pgvec_db3_memory_candidates(const float *vec, int dim, const char *record_type,
                                   const char *workspace, const char *project, int limit,
                                   int64_t *ids, double *scores, int max);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_PGVEC_DB3_H */
