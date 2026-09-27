/* db2/memory_health.h: SQL primitives for the memory_health table and the
 * supporting counters that feed it. DB2-owned. */
#ifndef DEC_DB2_MEMORY_HEALTH_H
#define DEC_DB2_MEMORY_HEALTH_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* INSERT a memory_health row stamped to now with the given counters. */
   void db2_memory_health_record(int total_memories, int contradictions_detected, int promotions,
                                 int demotions, int expirations);

   /* SELECT COUNT(*) FROM memories. Returns 0 on error. */
   int db2_memory_health_count_memories(void);

   /* Count memory_conflicts rows whose detected_at falls within the last
    * `days` days (relative to wall clock). */
   int db2_memory_health_count_recent_conflicts(int days);

   /* DELETE FROM memory_health WHERE cycle_at < now - days. Returns rows
    * deleted. Idempotent. */
   int db2_memory_health_prune_old(int days);

   /* DELETE FROM contradiction_log WHERE detected_at < now - days.
    * Returns rows deleted. Idempotent. */
   int db2_memory_health_prune_old_contradictions(int days);

   /* Move L2 memories whose effectiveness is below `threshold` back to
    * L1 with updated_at stamped to now. Returns rows changed (>= 0). */
   int db2_memory_health_demote_low_effectiveness(double threshold);

   /* Aggregate `effectiveness` over the memories table.
    *  - avg_effectiveness:    AVG over rows where effectiveness IS NOT NULL.
    *  - low_effectiveness:    COUNT of rows below `low_threshold`.
    *  - high_impact:          COUNT of rows with effectiveness > 0.8 AND use_count >= 10.
    * Returns 0 on success, -1 on error. Output pointers may be NULL. */
   int db2_memory_health_effectiveness_stats(double low_threshold, double *avg_effectiveness,
                                             int *low_effectiveness, int *high_impact);

   /* List L2 memory ids (up to `max`). Returns count written. */
   int db2_memory_health_list_l2_memory_ids(int64_t *out, int max);

   /* Aggregated counters over the rolling 7-day health window plus the
    * current memory snapshot, used to compute the rates returned by
    * memory_query_health. The orchestrator turns these into ratios. */
   typedef struct
   {
      int cycles;
      int total_contradictions;
      int total_promotions;
      int total_demotions;
      int total_expirations;
      int new_memories;     /* memories.created_at within last 7 days */
      int l1_eligible;      /* L1 rows with use_count >= ? OR confidence >= ? */
      int l2_total;         /* L2 row count */
      int l2_stale_30_days; /* L2 rows with last_used_at NULL or > 30 days old */
   } db2_memory_health_query_counters_t;

   int db2_memory_health_query_counters(int promote_use_count, double promote_confidence,
                                        db2_memory_health_query_counters_t *out);

   /* DELETE FROM memories WHERE sensitivity = ? AND created_at < now-days.
    * Returns rows deleted. Idempotent. */
   int db2_memory_health_delete_by_sensitivity(const char *sensitivity, int days);

   /* Stamp a memory's effectiveness column. The _clear variant writes
    * NULL; _set writes the supplied value. Returns 0 on success. */
   int db2_memory_health_set_effectiveness(int64_t memory_id, double value);
   int db2_memory_health_clear_effectiveness(int64_t memory_id);

   /* Fill tier_counts / kind_counts / total / conflicts from the
    * memories + memory_conflicts tables. Caller layers PageRank timing
    * stats on top. memory_stats_t lives in headers/memory.h; callers
    * must include "aimee.h" before this header for the type to
    * resolve. */
   int db2_memory_stats_counts(memory_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_MEMORY_HEALTH_H */
