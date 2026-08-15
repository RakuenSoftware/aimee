/* db2/curiosity.h: curiosity backlog, DB2 subsystem.
 *
 * Pure domain API. No backend types. SQL is encapsulated in
 * src/modules/db2/c/curiosity.c, which goes through the libpq shim via
 * aimee_pg_* helpers. */
#ifndef DEC_DB2_CURIOSITY_H
#define DEC_DB2_CURIOSITY_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define CURIOSITY_TARGET_LEN   128
#define CURIOSITY_EVIDENCE_LEN 512
#define CURIOSITY_GAP_TYPE_LEN 32
#define CURIOSITY_STATE_LEN    16

/* Canonical gap-type names. The schema accepts any string so future
 * proposals can extend without a migration, but helpers enforce the
 * canonical five at entry points. */
#define CURIOSITY_GAP_MISSING_FACT          "missing_fact"
#define CURIOSITY_GAP_CONTRADICTION         "contradiction"
#define CURIOSITY_GAP_STALE_FACT            "stale_fact"
#define CURIOSITY_GAP_WEAK_COVERAGE         "weak_coverage"
#define CURIOSITY_GAP_UNVERIFIED_ASSUMPTION "unverified_assumption"

/* State transitions: open -> in_progress -> resolved | suppressed. */
#define CURIOSITY_STATE_OPEN        "open"
#define CURIOSITY_STATE_IN_PROGRESS "in_progress"
#define CURIOSITY_STATE_RESOLVED    "resolved"
#define CURIOSITY_STATE_SUPPRESSED  "suppressed"

   typedef struct
   {
      int64_t id;
      char gap_type[CURIOSITY_GAP_TYPE_LEN];
      char target_entity[CURIOSITY_TARGET_LEN];
      char target_topic[CURIOSITY_TARGET_LEN];
      char evidence[CURIOSITY_EVIDENCE_LEN];
      double importance;
      double novelty;
      double progress;
      double routing_score;
      char state[CURIOSITY_STATE_LEN];
      char source_session[64];
      char created_at[32];
      char updated_at[32];
   } curiosity_item_t;

   /* Predicates: cheap, no DB. */
   int db2_curiosity_gap_type_is_canonical(const char *gap_type);
   int db2_curiosity_state_is_valid(const char *state);

   /* Create a new curiosity item with state = open. source_session is
    * stored verbatim; callers usually pass session_id(). Returns 0 on
    * success and populates *out (if non-NULL). */
   int db2_curiosity_create(const char *gap_type, const char *target_entity,
                            const char *target_topic, const char *evidence, double importance,
                            double novelty, const char *source_session, curiosity_item_t *out);

   /* List items, optionally filtered by state (NULL/empty = all).
    * Newest-first by created_at. Returns count written (<= max). */
   int db2_curiosity_list(const char *state, curiosity_item_t *out, int max);

   /* List the top-N open items by routing_score (DESC), tiebreaking on
    * created_at (ASC). Used by the directive-routing path so the
    * highest-priority gap goes first. Returns count written (<= max). */
   int db2_curiosity_list_top_open_by_score(curiosity_item_t *out, int max);

   /* Fetch a single item by id. Returns 1 if found, 0 if not, -1 on error. */
   int db2_curiosity_get(int64_t id, curiosity_item_t *out);

   /* Transition state. Rejects unknown states. Returns 0 on success. */
   int db2_curiosity_update_state(int64_t id, const char *new_state);

   /* Sweep the local failed_queries table for retrieval-failure entries
    * that don't yet have an open `missing_fact` curiosity item, and
    * create one each. Returns the number of new items created. */
   int db2_curiosity_sweep_failed_queries(void);

   /* Recompute importance / novelty / progress / routing_score for
    * every open or in-progress item. Returns the number of rows
    * rescored. */
   int db2_curiosity_rescore_all(void);

   /* Remove every curiosity item. Test-only. */
   int db2_curiosity_reset(void);

   /* Promote a corpus gap artifact to a curiosity_items row.
    * gap_kind maps: undefined_entity→missing_fact, dangling_reference→weak_coverage.
    * Skips if an open item for the same subject already exists.
    * Returns 0 on success or skip, -1 on error. */
   int db2_curiosity_promote_corpus_gap(const char *artifact_id, const char *gap_kind,
                                        const char *subject, const char *evidence_ref);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_CURIOSITY_H */
