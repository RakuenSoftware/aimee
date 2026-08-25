/* pgvec_db3.h: the seam between DB2's pgvector searches and the DB3 route.
 *
 * DB3 lets an external vector provider serve the portable half of the vector
 * contract (see src/modules/db2/eventcontract/vector-portability.json). The
 * route itself — selection, fallback, and candidate re-authorization — lives in
 * db3_route.c and knows nothing about pgvector. This file is what connects the
 * two: it owns the process's routes, and it turns a pgvec-shaped search into a
 * DB3 request and the reply back into pgvec's id/score arrays.
 *
 * ONE ROUTE PER COLLECTION. A DB3 route selects a single provider, and a
 * provider serves a single collection, because the search request has no
 * collection field — only workspace, project and record_type, and record_type
 * is a label within a collection rather than a name for one. So a single
 * process-wide route could serve exactly one collection, and a kb search routed
 * through a memory provider's route would come back with memory candidates that
 * look like kb answers. Keying routes by collection is what makes the two
 * independent without changing the wire.
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

/* The collections this seam can route, matching db3_projection.collection.
 *
 * A collection appears here once every condition its search applies can be
 * expressed on the wire -- as the scope fields, or as exact-match filters. The
 * curator collections qualify because their searches are single-table filters
 * over labels the projection already captures. kb and kb_pdf do not: their
 * searches filter on kb_documents.generation, which is not a column on the
 * embedding row, so the capture trigger has nothing to label it with. */
#define PGVEC_DB3_COLLECTION_MEMORY             "memory"
#define PGVEC_DB3_COLLECTION_CODE               "code"
#define PGVEC_DB3_COLLECTION_CURATOR_ENTITY     "curator_entity"
#define PGVEC_DB3_COLLECTION_CURATOR_NARRATIVE  "curator_narrative"
#define PGVEC_DB3_COLLECTION_CURATOR_CLAIM_SUBJ "curator_claim_subject"
#define PGVEC_DB3_COLLECTION_CURATOR_CLAIM_VAL  "curator_claim_value"
#define PGVEC_DB3_COLLECTION_CURATOR_CODE_INT   "curator_code_intent"
#define PGVEC_DB3_COLLECTION_CURATOR_CODE_SIG   "curator_code_signature"
#define PGVEC_DB3_COLLECTION_CURATOR_CODE_BODY  "curator_code_body"

   /* Install the route for one collection. `internal_search` is the pgvector
    * implementation the route falls back to and uses when no provider is
    * selected; `authorize` re-checks each returned point id against DB2's
    * canonical rows. Both are required — a route that could not fall back or
    * could not authorize would be a route that must never be used.
    *
    * Idempotent per collection: a second call replaces that collection's
    * callbacks and clears its selected provider, because the provider was
    * selected against the old ones.
    *
    * Returns 0 on success, -1 on invalid arguments or an unknown collection. */
   int pgvec_db3_route_install(const char *collection, aimee_db3_search_fn internal_search,
                               void *internal_context,
                               aimee_db3_candidate_authorize_fn authorize, void *authorize_context);

   /* Select an external provider to serve one collection's portable reads.
    * `principal` must be nonzero and comes from the host-authenticated bus
    * frame, never from the provider's own payload. `fallback_enabled` decides
    * whether a provider failure silently falls back to pgvector or surfaces.
    *
    * Returns 0 on success, -1 if that collection has no route installed or the
    * arguments are invalid. */
   int pgvec_db3_route_select(const char *collection, uint32_t principal, int ready,
                              int fallback_enabled, aimee_db3_search_fn external_search,
                              void *external_context);

   /* Deselect one collection's external provider; its reads return to pgvector.
    * A NULL collection clears every route. */
   void pgvec_db3_route_clear(const char *collection);

   /* 1 when the collection has a provider selected AND ready, else 0. */
   int pgvec_db3_route_serving(const char *collection);

   /* A filter to send with a search, mirroring the wire's closed filter list.
    * Keys must be strictly ascending; the encoder refuses anything else, so one
    * filter set has exactly one encoding. */
   typedef struct
   {
      const char *key;
      const char *value;
   } pgvec_db3_filter_t;

   /* Run one candidate search for `collection` through its route.
    *
    * Fills `ids` and `scores` with at most `max` candidates and returns the
    * count, or -1 when the route did not produce an answer (including when no
    * route is installed for that collection) so the caller runs its own
    * pgvector query instead.
    *
    * `workspace` and `project` carry the scope. At least one must be non-empty:
    * an unscoped search cannot be expressed on the DB3 wire, and sending one
    * with both blank would ask a provider to search everything.
    *
    * `filters` carries any condition beyond the scope — a project generation,
    * a lifecycle state — as exact label matches the provider must honour. A
    * caller whose query depends on something it cannot express as a filter must
    * not use this path: the search would come back answering a wider question
    * while looking correct.
    *
    * A kind filter and a project exclusion are still inexpressible. Both are
    * set membership and negation respectively, and the wire's grammar is exact
    * match only. */
   int pgvec_db3_candidates(const char *collection, const float *vec, int dim,
                            const char *record_type, const char *workspace, const char *project,
                            const pgvec_db3_filter_t *filters, int filter_count, int limit,
                            int64_t *ids, double *scores, int max);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_PGVEC_DB3_H */
