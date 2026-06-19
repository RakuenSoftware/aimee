/* memory_logic.c: kind-lifecycle promotion / demotion / expiry orchestration.
 * SQL surface lives in db2/memory_promotion.c — this file owns the
 * orchestration loop over kinds and the failure-driven demotion pass.
 *
 * Related concerns live in siblings:
 *   memory_health.c     — context snapshot, content safety, health metrics
 *   memory_conflict.c   — session folding + conflict detection (live + retro)
 *   memory_context.c    — window compaction + retrieval planner + derive-facts
 *   memory_retrieval.c  — memory_retrieval_confidence
 *   memory_assemble.c   — memory_assemble_context family + context cache
 */
#include "aimee.h"
#include "db1_optional.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/artifacts.h"
#include "db2/calibration.h"
#include "db2/kind_lifecycle.h"
#include "db2/memory_payload.h"
#include "db2/memory_promotion.h"
#endif
#include "config.h"
#include "kb.h"
#include "log.h"
#include "platform_process.h"
#include "workspace.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#if !defined(AIMEE_DB2_DISABLED)
static void calibration_trace_write(const char *kind, const char *path, double static_threshold,
                                    double calibrated_threshold, int changed)
{
   char art_id[64];
   char payload[384];
   db2_artifact_gen_id(art_id, sizeof(art_id));
   snprintf(payload, sizeof(payload),
            "{\"target_surface\":\"memory\",\"kind\":\"%s\",\"path\":\"%s\","
            "\"static_threshold\":%.4f,\"calibrated_threshold\":%.4f,"
            "\"promoted_count\":%d}",
            kind ? kind : "", path ? path : "", static_threshold, calibrated_threshold, changed);
   db2_artifact_write(art_id, "calibration_ab_trace", "committed", "memory", kind ? kind : "",
                      "system", 1.0, payload);
}
#endif

/* --- Kind Lifecycle Configuration ---
 *
 * The loader plus default thresholds moved to db2/kind_lifecycle.c
 * (db2_kind_lifecycle_load). */

/* --- Promotion / Demotion / Expiry --- */

/* Promote L1 -> L2 per kind-specific thresholds */
int memory_promote(void)
{
#if defined(AIMEE_DB2_DISABLED)
   return 0;
#else
   char ts[32];
   now_utc(ts, sizeof(ts));

   db2_memory_promotion_kind_t kinds[16];
   int nkinds = db2_memory_promotion_list_kinds_in_tier("L1", kinds,
                                                        (int)(sizeof(kinds) / sizeof(kinds[0])));

   config_t cfg;
   config_load(&cfg);

   int total = 0;
   for (int i = 0; i < nkinds; i++)
   {
      kind_lifecycle_t lc;
      db2_kind_lifecycle_load(kinds[i].kind, &lc);

      double promote_threshold = lc.promote_confidence;
      double calibrated_threshold = lc.promote_confidence;
      int has_calibration = 0;

      if (cfg.calibration_enabled >= 2)
      {
         char cal_buf[4096];
         if (db2_calibration_profile_read("memory", kinds[i].kind, "global", "", cal_buf,
                                          sizeof(cal_buf)) == 0)
         {
            double target = cfg.calibration_tau_memory_auto > 0.0 ? cfg.calibration_tau_memory_auto
                                                                  : lc.promote_confidence;
            has_calibration = db2_calibration_threshold_from_profile_json(
                                  cal_buf, target, &calibrated_threshold) == 0;
         }
      }

      if (has_calibration && cfg.calibration_enabled == 2)
      {
         int calibrated_changed = db2_memory_promotion_promote_kind_slot(
             ts, kinds[i].kind, lc.promote_use_count, calibrated_threshold, 5, 0, 1);
         int static_changed = db2_memory_promotion_promote_kind_slot(
             ts, kinds[i].kind, lc.promote_use_count, lc.promote_confidence, 5, 0, 0);
         calibration_trace_write(kinds[i].kind, "calibrated", lc.promote_confidence,
                                 calibrated_threshold, calibrated_changed);
         calibration_trace_write(kinds[i].kind, "static", lc.promote_confidence,
                                 calibrated_threshold, static_changed);
         total += calibrated_changed + static_changed;
         continue;
      }

      if (has_calibration && cfg.calibration_enabled >= 3)
      {
         promote_threshold = calibrated_threshold;
         aimee_log(LOG_INFO, "memory_promote",
                   "using calibrated threshold for kind=%s static=%.4f calibrated=%.4f",
                   kinds[i].kind, lc.promote_confidence, promote_threshold);
      }

      total += db2_memory_promotion_promote_kind(ts, kinds[i].kind, lc.promote_use_count,
                                                 promote_threshold);
   }
   return total;
#endif
}

/* Demote L2 -> L1 per kind-specific thresholds (with demotion resistance) */
int memory_demote(void)
{
#if defined(AIMEE_DB2_DISABLED)
   return 0;
#else
   char ts[32];
   now_utc(ts, sizeof(ts));

   db2_memory_promotion_kind_t kinds[16];
   int nkinds = db2_memory_promotion_list_kinds_in_tier("L2", kinds,
                                                        (int)(sizeof(kinds) / sizeof(kinds[0])));

   int total = 0;
   for (int i = 0; i < nkinds; i++)
   {
      kind_lifecycle_t lc;
      db2_kind_lifecycle_load(kinds[i].kind, &lc);

      int effective_days = (int)(lc.demote_days * lc.demotion_resistance);
      char days_str[16];
      snprintf(days_str, sizeof(days_str), "-%d", effective_days);

      total += db2_memory_promotion_demote_kind(ts, kinds[i].kind, lc.demote_confidence, days_str);
   }

   /* Cascade: reduce confidence of memories that depend_on recently demoted ones.
    * A demoted memory is now L1 with updated_at = ts (just set above). */
   if (total > 0)
      db2_memory_promotion_demote_cascade(ts);

   return total;
#endif
}

/* Expire L0 (all) and stale L1 per kind-specific thresholds */
int memory_expire(void)
{
#if defined(AIMEE_DB2_DISABLED)
   return 0;
#else
   /* Wipe all L0 (provenance + rows). */
   db2_memory_promotion_delete_l0_provenance();
   int total = db2_memory_promotion_delete_l0();

   db2_memory_promotion_kind_t kinds[16];
   int nkinds = db2_memory_promotion_list_kinds_in_tier("L1", kinds,
                                                        (int)(sizeof(kinds) / sizeof(kinds[0])));

   for (int i = 0; i < nkinds; i++)
   {
      kind_lifecycle_t lc;
      db2_kind_lifecycle_load(kinds[i].kind, &lc);

      char days_str[16];
      snprintf(days_str, sizeof(days_str), "-%d", lc.expire_days);

      db2_memory_promotion_delete_stale_l1_provenance(kinds[i].kind, days_str);
      total += db2_memory_promotion_delete_stale_l1(kinds[i].kind, days_str);
   }

   return total;
#endif
}

/* Demote memories associated with failed agent executions.
 * Reduces confidence of memories that were used in contexts where
 * the agent failed, creating a negative feedback loop. */
#if !defined(AIMEE_DB2_DISABLED)
static void lowercase_copy(char *dst, size_t cap, const char *src)
{
   if (!dst || cap == 0)
      return;
   size_t i = 0;
   if (src)
   {
      for (; i + 1 < cap && src[i]; i++)
         dst[i] = (char)tolower((unsigned char)src[i]);
   }
   dst[i] = '\0';
}

static int append_unique_int64(int64_t **ids, int *count, int *cap, int64_t id)
{
   if (!ids || !count || !cap)
      return -1;
   for (int i = 0; i < *count; i++)
   {
      if ((*ids)[i] == id)
         return 0;
   }
   if (*count >= *cap)
   {
      int new_cap = (*cap > 0) ? (*cap * 2) : 16;
      int64_t *grown = realloc(*ids, (size_t)new_cap * sizeof(*grown));
      if (!grown)
         return -1;
      *ids = grown;
      *cap = new_cap;
   }
   (*ids)[(*count)++] = id;
   return 0;
}
#endif

int memory_demote_from_failures(void)
{
#if defined(AIMEE_DB2_DISABLED)
   return 0;
#else
   if (!db1_agent_log_list_recent_errors)
      return 0;
   db1_agent_log_recent_error_t errors[128];
   int error_count = db1_agent_log_list_recent_errors(7, errors, 128);
   if (error_count <= 0)
      return 0;

   int64_t *matched_ids = NULL;
   int matched_count = 0;
   int matched_cap = 0;

   for (int i = 0; i < error_count; i++)
   {
      char lowered[DB1_AL_LEARN_ERROR_LEN];
      lowercase_copy(lowered, sizeof(lowered), errors[i].error);
      if (!lowered[0])
         continue;

      int64_t batch[256];
      int n = db2_memory_promotion_match_error_keys(lowered, batch,
                                                    (int)(sizeof(batch) / sizeof(batch[0])));
      for (int j = 0; j < n; j++)
      {
         if (append_unique_int64(&matched_ids, &matched_count, &matched_cap, batch[j]) != 0)
         {
            free(matched_ids);
            return 0;
         }
      }
   }

   if (matched_count == 0)
   {
      free(matched_ids);
      return 0;
   }

   int changes = 0;
   for (int i = 0; i < matched_count; i++)
      changes += db2_memory_promotion_demote_id(matched_ids[i]);
   free(matched_ids);
   return changes;
#endif
}

/* Synthesize L2 facts from delegation patterns in agent_log.
 * When a role+agent combo has 3+ attempts in 30 days, create a fact
 * summarizing success rate, avg turns, and common errors. */
int memory_promote_delegation_patterns(void)
{
#if defined(AIMEE_DB2_DISABLED)
   return 0;
#else
   if (!db1_agent_log_list_delegation_patterns)
      return 0;
   db1_agent_log_delegation_pattern_t patterns[128];
   int pattern_count = db1_agent_log_list_delegation_patterns(30, 3, patterns, 128);
   if (pattern_count <= 0)
      return 0;

   int count = 0;
   for (int i = 0; i < pattern_count; i++)
   {
      const char *role = patterns[i].role;
      const char *agent = patterns[i].agent_name;
      int wins = patterns[i].wins;
      int fails = patterns[i].fails;
      int total = patterns[i].total;
      int avg_turns = patterns[i].avg_turns;
      int avg_tools = patterns[i].avg_tools;

      if (!role || !agent)
         continue;

      double success_rate = total > 0 ? (double)wins / total : 0;

      char key[256];
      char content[1024];

      if (success_rate >= 0.7)
      {
         snprintf(key, sizeof(key), "delegate_pattern_%s_%s", role, agent);
         snprintf(content, sizeof(content),
                  "Delegation [%s] via %s reliably succeeds (%d/%d attempts, "
                  "avg %d turns, %d tool calls).",
                  role, agent, wins, total, avg_turns, avg_tools);
         memory_t mem;
         memory_insert(TIER_L2, KIND_FACT, key, content, success_rate, "", &mem);
         count++;
      }

      if (fails >= 3 && (double)fails / total >= 0.5)
      {
         const char *last_err = patterns[i].recent_error[0] ? patterns[i].recent_error : "unknown";

         snprintf(key, sizeof(key), "delegate_warning_%s_%s", role, agent);
         snprintf(content, sizeof(content),
                  "Delegation [%s] via %s often fails (%d/%d attempts). "
                  "Recent error: %s",
                  role, agent, fails, total, last_err);
         memory_t mem;
         memory_insert(TIER_L2, KIND_FACT, key, content, 0.8, "", &mem);
         count++;
      }
   }
   return count;
#endif
}

/* Synthesize L3 episode memories from recurring delegation failures.
 * When a role+agent combo has FAILURE_EPISODE_MIN+ failures in
 * FAILURE_EPISODE_WINDOW days, create a structured episode. */
int memory_synthesize_failure_episodes(void)
{
#if defined(AIMEE_DB2_DISABLED)
   return 0;
#else
   if (!db1_agent_log_list_failure_episode_seeds)
      return 0;
   db1_agent_log_failure_episode_seed_t seeds[128];
   int seed_count = db1_agent_log_list_failure_episode_seeds(FAILURE_EPISODE_WINDOW,
                                                             FAILURE_EPISODE_MIN, seeds, 128);
   if (seed_count <= 0)
      return 0;

   int count = 0;
   for (int i = 0; i < seed_count; i++)
   {
      const char *role = seeds[i].role;
      const char *agent = seeds[i].agent_name;
      int fails = seeds[i].fails;
      const char *errors = seeds[i].errors;

      if (!role || !agent)
         continue;

      /* Build episode key with date to avoid duplicates per period */
      char date_buf[16];
      {
         char ts[32];
         now_utc(ts, sizeof(ts));
         /* Extract YYYY-MM-DD */
         snprintf(date_buf, sizeof(date_buf), "%.10s", ts);
      }

      char key[256];
      snprintf(key, sizeof(key), "failure_episode_%s_%s_%s", role, agent, date_buf);

      /* Skip episodes already on file for this period. */
      if (db2_memory_key_exists(key) == 1)
         continue;

      /* Truncate errors to fit in content */
      char err_summary[512];
      if (errors && errors[0])
         snprintf(err_summary, sizeof(err_summary), "%.500s", errors);
      else
         snprintf(err_summary, sizeof(err_summary), "unknown");

      char content[1024];
      snprintf(content, sizeof(content),
               "Delegation [%s] via %s failed %d times in %d days. "
               "Errors: %s. "
               "Avoid repeating this pattern without addressing the root cause.",
               role, agent, fails, FAILURE_EPISODE_WINDOW, err_summary);

      memory_t mem;
      if (memory_insert(TIER_L3, KIND_EPISODE, key, content, 0.7, "", &mem) == 0)
      {
         aimee_log(LOG_INFO, "memory_promote", "L3 episode: %s (%d failures)", key, fails);
         count++;
      }
   }
   return count;
#endif
}

/* Embed any L2 memories that lack embeddings (called after promotion). */
void embed_unembedded_l2(void)
{
#if defined(AIMEE_DB2_DISABLED)
   return;
#else
   config_t cfg;
   config_load(&cfg);
   const char *embed_command = config_embedding_command(&cfg, NULL);
   const char *embed_ver = cfg.embedding_model[0] ? cfg.embedding_model : embed_command;

   int64_t ids[50];
   int count =
       db2_memory_promotion_list_unembedded_l2(embed_ver, ids, (int)(sizeof(ids) / sizeof(ids[0])));

   for (int i = 0; i < count; i++)
      memory_embed(ids[i], embed_command);
#endif
}
