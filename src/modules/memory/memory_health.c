/* memory_health.c: context snapshot effectiveness tracking, content-safety
 * retention, and health metrics. Extracted from memory_logic.c (the old
 * monolith). */
#include "aimee.h"
#include "util.h"
#include "cJSON.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db1_optional.h"
#include "db2/memory_health.h"
#include "db2/memory_query.h"
#include "kb.h"
#include "log.h"
#endif
#include "memory.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "memory_context_internal.h"
#include "memory_ontology.h"
#include "platform_process.h"
#include "workspace.h"
#endif
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#if defined(AIMEE_DB2_DISABLED)
/* Memory health, effectiveness, and maintenance writes are DB2-owned. Server
 * code should route these operations through aimee-kb RPCs. */
int memory_compute_effectiveness(void)
{
   return 0;
}

int memory_demote_low_effectiveness(void)
{
   return 0;
}

int memory_effectiveness_stats(effectiveness_stats_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int memory_enforce_retention(void)
{
   return 0;
}

int memory_run_maintenance(int *promoted, int *demoted, int *expired)
{
   if (promoted)
      *promoted = 0;
   if (demoted)
      *demoted = 0;
   if (expired)
      *expired = 0;
   return 0;
}

void memory_record_health(int promotions, int demotions, int expirations)
{
   (void)promotions;
   (void)demotions;
   (void)expirations;
}

void memory_prune_health(void)
{
}

int memory_query_health(memory_health_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}
#else

static int mh_is_within_days(const char *s, int days)
{
   time_t ts = str_parse_iso8601(s);
   if (ts == (time_t)0)
      return 0;
   return difftime(time(NULL), ts) <= (double)days * 86400.0;
}

typedef struct
{
   char session_id[DB1_SS_ID_LEN];
   signed char outcome; /* -1 = unknown/empty, 0 = non-success, 1 = success */
} mh_session_outcome_t;

static int mh_cached_session_outcome(const char *session_id, mh_session_outcome_t **cache,
                                     int *count, int *cap)
{
   if (!session_id || !session_id[0] || !cache || !count || !cap)
      return -1;

   for (int i = 0; i < *count; i++)
   {
      if (strcmp((*cache)[i].session_id, session_id) == 0)
         return (*cache)[i].outcome;
   }

   if (*count >= *cap)
   {
      int next_cap = (*cap == 0) ? 16 : (*cap * 2);
      mh_session_outcome_t *tmp = realloc(*cache, (size_t)next_cap * sizeof(**cache));
      if (!tmp)
         return -1;
      *cache = tmp;
      *cap = next_cap;
   }

   mh_session_outcome_t *slot = &(*cache)[(*count)++];
   memset(slot, 0, sizeof(*slot));
   snprintf(slot->session_id, sizeof(slot->session_id), "%s", session_id);
   slot->outcome = -1;

   db1_server_session_t session;
   if (db1_server_session_get && db1_server_session_get(session_id, &session) == 0 &&
       session.outcome[0])
      slot->outcome = (strcmp(session.outcome, "success") == 0) ? 1 : 0;

   return slot->outcome;
}

/* --- Context Snapshot and Effectiveness --- */

int memory_compute_effectiveness(void)
{
   if (!db1_context_snapshot_count_memories_with_min_samples ||
       !db1_context_snapshot_list_memory_ids_with_min_samples ||
       !db1_context_snapshot_count_for_memory || !db1_context_snapshot_list_sessions_for_memory)
      return 0;

   int updated = db1_context_snapshot_count_memories_with_min_samples(EFFECTIVENESS_MIN_SAMPLES);
   if (updated <= 0)
      return 0;

   int64_t *memory_ids = calloc((size_t)updated, sizeof(*memory_ids));
   if (!memory_ids)
      return 0;
   if (db1_context_snapshot_list_memory_ids_with_min_samples(EFFECTIVENESS_MIN_SAMPLES, memory_ids,
                                                             updated) != updated)
   {
      free(memory_ids);
      return 0;
   }

   mh_session_outcome_t *cache = NULL;
   int cache_count = 0;
   int cache_cap = 0;

   for (int i = 0; i < updated; i++)
   {
      int64_t memory_id = memory_ids[i];
      int rated_total = 0;
      int successes = 0;
      int snapshot_count = db1_context_snapshot_count_for_memory(memory_id);
      char(*session_ids)[DB1_CONTEXT_SNAPSHOT_SESSION_LEN] = NULL;

      if (snapshot_count > 0)
         session_ids = calloc((size_t)snapshot_count, sizeof(*session_ids));
      if (!session_ids)
         continue;

      int loaded =
          db1_context_snapshot_list_sessions_for_memory(memory_id, session_ids, snapshot_count);
      for (int s = 0; s < loaded; s++)
      {
         int outcome = mh_cached_session_outcome(session_ids[s], &cache, &cache_count, &cache_cap);
         if (outcome >= 0)
         {
            rated_total++;
            if (outcome > 0)
               successes++;
         }
      }
      free(session_ids);

      if (rated_total > 0)
      {
         double effectiveness = (double)(successes + 1) / (double)(rated_total + 2);
         db2_memory_health_set_effectiveness(memory_id, effectiveness);
      }
      else
      {
         db2_memory_health_clear_effectiveness(memory_id);
      }
   }

   free(cache);
   free(memory_ids);
   return updated;
}

int memory_demote_low_effectiveness(void)
{
   return db2_memory_health_demote_low_effectiveness(EFFECTIVENESS_DEMOTE_THRESHOLD);
}

int memory_effectiveness_stats(effectiveness_stats_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));

   if (db2_memory_health_effectiveness_stats(EFFECTIVENESS_DEMOTE_THRESHOLD,
                                             &out->avg_effectiveness, &out->low_effectiveness_count,
                                             &out->high_impact_count) != 0)
      return -1;

   /* Walk L2 memory ids and count those that the dashboard has never
    * shown. The has_memory check lives in db1, so the orchestrator
    * stays in this file. */
   int64_t l2_ids[2048];
   int n = db2_memory_health_list_l2_memory_ids(l2_ids, (int)(sizeof(l2_ids) / sizeof(l2_ids[0])));
   for (int i = 0; i < n; i++)
   {
      if (!db1_context_snapshot_has_memory || !db1_context_snapshot_has_memory(l2_ids[i]))
         out->never_surfaced_l2++;
   }
   return 0;
}

/* --- Content Safety Retention --- */

int memory_enforce_retention(void)
{
   int total = 0;
   total += db2_memory_health_delete_by_sensitivity("restricted", RETENTION_RESTRICTED_DAYS);
   total += db2_memory_health_delete_by_sensitivity("sensitive", RETENTION_SENSITIVE_DAYS);
   return total;
}

/* Distill L1 memory chains (multiple entries for same session/task) into L2 facts.
 * This is the "Sleep Cycle" that creates high-density milestones. */
static int memory_consolidate_l1_chains(void)
{
   db2_memory_l1_cluster_row_t clusters[64];
   int cluster_count = db2_memory_l1_session_clusters("workflow_learning", 5, clusters, 64);

   int consolidated = 0;
   for (int i = 0; i < cluster_count; i++)
   {
      const char *session_id = clusters[i].session_id;
      if (!session_id[0])
         continue;

      db2_memory_created_at_row_t ats[256];
      int at_n = db2_memory_l1_session_created_at(session_id, ats, 256);
      int recent_count = 0;
      for (int j = 0; j < at_n; j++)
         if (mh_is_within_days(ats[j].created_at, 1))
            recent_count++;
      if (recent_count < 5)
         continue;

      db2_memory_content_row_t cs[256];
      int cn = db2_memory_l1_session_content(session_id, cs, 256);
      char summary[2048] = "";
      int pos = 0;
      for (int j = 0; j < cn; j++)
      {
         const char *c = cs[j].content;
         if (!c[0])
            continue;
         int len = (int)strlen(c);
         if (pos + len + 4 >= (int)sizeof(summary))
            continue;
         if (pos > 0)
            pos += snprintf(summary + pos, sizeof(summary) - (size_t)pos, " | ");
         pos += snprintf(summary + pos, sizeof(summary) - (size_t)pos, "%s", c);
      }

      if (pos > 0)
      {
         char key[256];
         snprintf(key, sizeof(key), "milestone:%s", session_id);
         char content[2048];
         snprintf(content, sizeof(content), "Consolidated milestone for session %s: %s", session_id,
                  summary);

         memory_t mem;
         if (memory_insert(TIER_L2, KIND_FACT, key, content, 0.8, session_id, &mem) == 0)
         {
            consolidated++;
            add_provenance(mem.id, session_id, "consolidate", "L1 chain distillation");
         }
      }
   }
   return consolidated;
}

int memory_run_maintenance(int *promoted, int *demoted, int *expired)
{
   int p = memory_promote();
   p += memory_promote_delegation_patterns();
   p += memory_synthesize_failure_episodes();
   p += memory_consolidate_l1_chains();
   /* L2 → L3 for stable facts. */
   {
      int n = memory_promote_stable_l2_to_l3();
      if (n > 0)
         p += n;
   }
   /* L0..Ln → L5 pattern synthesis. */
   {
      int n = memory_synthesize_l5_patterns();
      if (n > 0)
         p += n;
   }
   /* KB → L3 convention candidates. */
   {
      int n = kb_extract_convention_candidates();
      if (n > 0)
         p += n;
   }
   int d = memory_demote();
   d += memory_demote_from_failures();
   d += memory_demote_low_effectiveness();
   int e = memory_expire();
   e += memory_enforce_retention();

   if (promoted)
      *promoted = p;
   if (demoted)
      *demoted = d;
   if (expired)
      *expired = e;

   /* Embed newly promoted L2 memories that lack embeddings */
   if (p > 0)
      embed_unembedded_l2();

   /* Compute memory effectiveness scores */
   memory_compute_effectiveness();

   /* Keep derived retrieval structures current for older rows as the schema evolves. */
   memory_rebuild_derived_indexes(250);

   /* Retroactive conflict detection (daily, rate-limited) */
   memory_scan_retroactive_conflicts();

   /* Record health metrics for this maintenance cycle */
   memory_record_health(p, d, e);

   /* Prune old health and contradiction_log rows (90-day retention) */
   memory_prune_health();

   /* Prune graph edges where both endpoints have no L1+ memory */
   memory_graph_prune();

   /* Rescale edge weights per relation so the maximum is 100. Runs after the
    * prune so removed edges cannot hold the per-relation maximum and squash the
    * survivors. Idempotent once converged: the max is already 100, so the
    * rescale is identity until a new edge exceeds it. */
   memory_graph_normalize();

   return 0;
}

/* --- Health Metrics --- */

void memory_record_health(int promotions, int demotions, int expirations)
{
   int total = db2_memory_health_count_memories();
   int contradictions = db2_memory_health_count_recent_conflicts(1);
   db2_memory_health_record(total, contradictions, promotions, demotions, expirations);
}

void memory_prune_health(void)
{
   db2_memory_health_prune_old(90);
   db2_memory_health_prune_old_contradictions(90);
}

int memory_query_health(memory_health_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));

   db2_memory_health_query_counters_t c;
   if (db2_memory_health_query_counters(PROMOTE_L1_USE_COUNT, PROMOTE_L1_CONFIDENCE, &c) != 0)
      return -1;

   out->cycles = c.cycles;
   out->total_contradictions = c.total_contradictions;
   out->total_promotions = c.total_promotions;
   out->total_demotions = c.total_demotions;
   out->total_expirations = c.total_expirations;

   if (c.new_memories > 0)
      out->contradiction_rate = (double)c.total_contradictions / c.new_memories;

   int pre_eligible = c.l1_eligible + c.total_promotions;
   if (pre_eligible > 0)
      out->promotion_rate = (double)c.total_promotions / pre_eligible;

   int pre_l2_total = c.l2_total + c.total_demotions;
   if (pre_l2_total > 0)
      out->demotion_rate = (double)c.total_demotions / pre_l2_total;

   if (c.l2_total > 0)
      out->staleness = (double)c.l2_stale_30_days / c.l2_total;

   return 0;
}

#endif
