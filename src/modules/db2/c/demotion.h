/* db2/demotion.h: retrieval attribution evidence and demotion profiles.
 *
 * Evidence storage uses the charter artifacts table (no new tables).
 * Evidence kinds:
 *   retrieval_event       — one row per recall invocation; carries surfaced ids.
 *   retrieval_attribution — one row per (retrieval_event, surfaced_row); carries
 *                           verdict and contribution weight.
 *   demotion_profile      — fitted per (memory_class, scope) at maintenance time.
 *
 * Lookup for demotion_profile uses narrowest-scope fallback:
 *   exact (memory_class, scope_kind, scope_id) first,
 *   then (memory_class, scope_kind, ""),
 *   then (memory_class, "global", "").
 *
 * See docs/proposals/done/outcome-driven-demotion-and-poison-resilience.md */
#ifndef DEC_DB2_DEMOTION_H
#define DEC_DB2_DEMOTION_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Verdict tokens for retrieval_attribution evidence. */
#define DEMOTION_VERDICT_ACCEPTED     "accepted"
#define DEMOTION_VERDICT_CORRECTED    "corrected"
#define DEMOTION_VERDICT_CONTRADICTED "contradicted"
#define DEMOTION_VERDICT_ROLLED_BACK  "rolled_back"
#define DEMOTION_VERDICT_IRRELEVANT   "irrelevant"

   /* Write a retrieval_event artifact capturing one recall invocation.
    * query_fingerprint: short opaque hash of the query (may be "").
    * role: charter recall role (e.g. "Recall").
    * surfaced_ids: array of n_surfaced memory row ids.
    * id_out: receives the new UUID (>= 37 bytes); may be NULL.
    * Returns 0 on success, -1 on error. */
   int db2_demotion_retrieval_event_write(const char *query_fingerprint, const char *role,
                                          const int64_t *surfaced_ids, int n_surfaced, char *id_out,
                                          int id_out_len);

   /* Like db2_demotion_retrieval_event_write but stamps the caller-visible
    * `turn_id` (auditable-correctness §Layer 2 / P1) so an audit trace can find
    * the event that grounded a turn. The partial unique index makes one turn map
    * to one event; a duplicate turn_id leaves the event written but un-stamped
    * (the first turn-stamped event stays authoritative — idempotent two-writer
    * merge is P1.5). turn_id NULL/"" behaves exactly like the base writer.
    * Returns 0 on success, -1 on error. */
   int db2_demotion_retrieval_event_write_turn(const char *turn_id, const char *query_fingerprint,
                                               const char *role, const int64_t *surfaced_ids,
                                               int n_surfaced, char *id_out, int id_out_len);

   /* Look up a turn-keyed retrieval_event by its caller-visible `turn_id` (the
    * /v1/audit/trace read). Writes the internal event id into id_out and the JSON
    * payload into payload_out (either may be NULL). Returns 1 on hit, 0 if no
    * event for that turn, -1 on error. */
   int db2_demotion_retrieval_event_by_turn(const char *turn_id, char *id_out, int id_out_len,
                                            char *payload_out, int payload_out_len);

   /* auditable-correctness P1.5 (D14): the idempotent two-writer merge. If no event
    * exists for `turn_id` yet, behaves exactly like ..._write_turn (first writer
    * creates it). If one exists, MERGES the new `surfaced_ids` into that same event
    * (deduped by id, with each ref's point-in-time version captured into
    * surfaced_items) instead of dropping them — so a second surface (e.g. code
    * search) contributes to the same turn event. Idempotent: re-merging refs already
    * present is a no-op. Concurrency-safe via a compare-and-swap retry (budget of 5
    * attempts, then -1; no FOR UPDATE needed; portable to the sqlite shim). The
    * create path also retries so refs land even if a concurrent writer wins the
    * initial create. Non-positive ids in `surfaced_ids` are
    * ignored. `query_fingerprint`/`role` are used ONLY on the first-writer create
    * path (a later writer contributes refs, not a new fingerprint). Writes the
    * canonical event id into id_out (may be NULL). Returns 0 / -1. */
   int db2_demotion_retrieval_event_merge_turn(const char *turn_id, const char *query_fingerprint,
                                               const char *role, const int64_t *surfaced_ids,
                                               int n_surfaced, char *id_out, int id_out_len);

   /* auditable-correctness P1.5 (D3/D14): merge TYPED refs into the turn's unified
    * surfaced_refs. The three parallel arrays give each ref's `type` (e.g. "code"),
    * `ref` (the stable identity, e.g. "code:<project>:<file_path>") and `versions`
    * (e.g. content_hash; `versions` may be NULL, or an entry "" to omit v). Entries
    * with an empty type or ref are skipped. Deduped by (type, ref) — idempotent, and
    * a typed ref never collides with a memory entry. Entries with type "memory" are
    * skipped — memory refs are id-keyed and must use ..._merge_turn. If no event
    * exists yet a bare turn event is created first (reusing write_turn's dup-race
    * handling). Same CAS retry/concurrency contract as the int64 merge. `versions`
    * may be NULL; `n_refs`==0 is a valid no-op. Returns 0 / -1. */
   int db2_demotion_retrieval_event_merge_refs_turn(const char *turn_id,
                                                    const char *query_fingerprint, const char *role,
                                                    const char *const *types,
                                                    const char *const *refs,
                                                    const char *const *versions, int n_refs,
                                                    char *id_out, int id_out_len);

   /* Write a retrieval_attribution artifact linking one surfaced row to a verdict.
    * retrieval_event_id: UUID of the originating retrieval_event.
    * surfaced_row_id: memory row id that contributed to the response.
    * verdict: one of the DEMOTION_VERDICT_* constants.
    * weight: contribution fraction in [0, 1].
    * Returns 0 on success, -1 on error. */
   int db2_demotion_retrieval_attribution_write(const char *retrieval_event_id,
                                                int64_t surfaced_row_id, const char *verdict,
                                                double weight);

   /* Compute a time-decayed demotion score for a memory row from its recent
    * attribution window.  Only DEMOTION_VERDICT_* verdicts affect the score;
    * accepted contributes positively, negative verdicts negatively, irrelevant
    * is zero-weight.  The scorer reads only attributed outcome evidence — not
    * source tags, declared confidence, author id, or retrieval frequency.
    * window_size: max attribution rows (most recent first).
    * half_life_days: exponential decay half-life.
    * n_min: minimum rows before scoring; returns NAN if fewer are found.
    * Returns the score on success, NAN on insufficient data or error. */
   double db2_demotion_score(int64_t row_id, int window_size, double half_life_days, int n_min);

   /* Write a demotion_profile artifact for (memory_class, scope_kind, scope_id).
    * memory_class: memory kind this profile covers (e.g. "preference").
    * payload_json: the full JSON profile blob.
    * id_out: receives the new UUID (>= 37 bytes); may be NULL.
    * Returns 0 on success, -1 on error. */
   int db2_demotion_profile_write(const char *memory_class, const char *scope_kind,
                                  const char *scope_id, const char *payload_json, char *id_out,
                                  int id_out_len);

   /* Read the most recently committed demotion_profile for (memory_class,
    * scope_kind, scope_id) with narrowest-scope fallback.
    * Returns 0 on success, -1 if not found or error. */
   int db2_demotion_profile_read(const char *memory_class, const char *scope_kind,
                                 const char *scope_id, char *buf, size_t len);

   typedef struct
   {
      int64_t row_id;
      int attribution_n;
   } db2_demotion_candidate_t;

   /* Enumerate memory rows that have at least n_min retrieval_attribution rows.
    * Used to select candidates for demotion scoring at maintenance time.
    * Returns count filled (>= 0) or -1 on error. */
   int db2_demotion_candidates(int n_min, db2_demotion_candidate_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_DEMOTION_H */
