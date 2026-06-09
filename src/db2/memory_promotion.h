/* db2/memory_promotion.h: tier-promotion / demotion / expiry SQL primitives
 * for the memories table. DB2-owned.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB2_MEMORY_PROMOTION_H
#define DEC_DB2_MEMORY_PROMOTION_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* SELECT DISTINCT kind FROM memories WHERE tier = ?. Fills `out` with up
    * to `max` short kind strings (15 chars + NUL each); returns count. */
   typedef struct
   {
      char kind[16];
   } db2_memory_promotion_kind_t;

   int db2_memory_promotion_list_kinds_in_tier(const char *tier, db2_memory_promotion_kind_t *out,
                                               int max);

   /* L1 -> L2 promotion for a single kind, with the kind's own thresholds.
    * `ts` is the updated_at value to stamp. Returns rows changed. */
   int db2_memory_promotion_promote_kind(const char *ts, const char *kind, int promote_use_count,
                                         double promote_confidence);

   /* L1 -> L2 promotion for one deterministic A/B slot. When slot_match is
    * non-zero, only rows where id % slot_modulo == slot_remainder are eligible;
    * otherwise only rows outside that slot are eligible. Falls back to the
    * regular kind promotion when slot_modulo <= 0. */
   int db2_memory_promotion_promote_kind_slot(const char *ts, const char *kind,
                                              int promote_use_count, double promote_confidence,
                                              int slot_modulo, int slot_remainder, int slot_match);

   /* L2 -> L1 demotion for a single kind, gated by confidence and idle days
    * (`days_neg_str` is "-N", embedded into `datetime('now', ?||' days')`).
    * Returns rows changed. */
   int db2_memory_promotion_demote_kind(const char *ts, const char *kind, double demote_confidence,
                                        const char *days_neg_str);

   /* Cascade: drop confidence on memories that depend_on rows just demoted
    * (matched by tier='L1' AND updated_at = ts). Returns rows changed. */
   int db2_memory_promotion_demote_cascade(const char *ts);

   /* Wipe L0 provenance + all L0 memory rows. Returns L0 rows deleted. */
   int db2_memory_promotion_delete_l0_provenance(void);
   int db2_memory_promotion_delete_l0(void);

   /* Wipe stale-L1 provenance + the rows themselves, scoped to a kind and
    * an idle-window (`days_neg_str` is "-N"). Returns L1 rows deleted. */
   int db2_memory_promotion_delete_stale_l1_provenance(const char *kind, const char *days_neg_str);
   int db2_memory_promotion_delete_stale_l1(const char *kind, const char *days_neg_str);

   /* Match a lowered error string against memory keys via
    * `error LIKE '%'||LOWER(key)||'%'`. Fills up to `max` ids; returns
    * count. */
   int db2_memory_promotion_match_error_keys(const char *error_lowered, int64_t *ids_out, int max);

   /* `UPDATE memories SET confidence = confidence * 0.9 WHERE id = ? AND
    * confidence > 0.3`. Returns rows changed. */
   int db2_memory_promotion_demote_id(int64_t memory_id);

   /* List L2 memory ids that lack a versioned embedding row for `version`.
    * Up to `max` ids; returns count. */
   int db2_memory_promotion_list_unembedded_l2(const char *version, int64_t *ids_out, int max);

   /* Promote stable L2 facts/preferences to L3 (confidence >= 0.95,
    * use_count >= 5, untouched for 30+ days). `ts` is the updated_at
    * stamp. Returns rows changed. */
   int db2_memory_promotion_promote_stable_l2_to_l3(const char *ts);

   /* Promote L3 policy/workflow rows to L4. When require_approval is
    * non-zero, only policies with an entry in memory_promotion_approvals
    * are eligible (workflows are always eligible). Returns rows changed,
    * -1 on SQL error. */
   int db2_memory_promotion_reclassify_directives(int require_approval);

   /* INSERT OR REPLACE an approval row in memory_promotion_approvals
    * for (memory_id, 'L4'). Returns 0 on success, -1 on failure. */
   int db2_memory_promotion_record_l4_approval(int64_t memory_id, const char *approver,
                                               const char *note);

   /* L5 synthesis candidate row: a high-confidence L2 fact/pattern observed
    * across >= 3 distinct sessions and not yet synthesized into an L5
    * pattern. */
   typedef struct
   {
      int64_t source_id;
      char src_key[256];
      char src_content[1024];
      int session_count;
   } db2_memory_l5_candidate_t;

   /* Find up to `max` L5 synthesis candidates (LIMIT 20 in SQL). Returns
    * count written, or -1 on SQL error. */
   int db2_memory_promotion_l5_pattern_candidates(db2_memory_l5_candidate_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_MEMORY_PROMOTION_H */
