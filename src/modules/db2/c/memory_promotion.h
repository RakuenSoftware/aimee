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

   /* SELECT DISTINCT epistemic_kind FROM memories WHERE tier = ?. The field is
    * retained as `kind` in this compatibility type, but lifecycle selection is
    * exclusively epistemic. */
   typedef struct
   {
      char kind[16];
   } db2_memory_promotion_kind_t;

   int db2_memory_promotion_list_kinds_in_tier(const char *tier, db2_memory_promotion_kind_t *out,
                                               int max);

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
    * across >= 3 distinct sessions of one canonical scope and not yet synthesized
    * into an L5 pattern.
    *
    * `scope_type`/`scope_value` identify the most-specific canonical scope the
    * recurrence was counted within, and the scope
    * the synthesized row must be written into. Recurrence across unrelated
    * workspaces is not recurrence: counting it that way turned three sessions
    * in three unrelated projects into one globally-reachable record, which is
    * both a scope leak in a background job and a derived row with no owning
    * scope. `session_count` is a reachability/salience signal only -- it must
    * never be converted into confidence. */
   typedef struct
   {
      int64_t source_id;
      char src_key[256];
      char src_content[1024];
      char scope_type[16];
      char scope_value[512];
      int session_count;
   } db2_memory_l5_candidate_t;

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_MEMORY_PROMOTION_H */
