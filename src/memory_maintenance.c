/* memory_maintenance.c: scheduled memory curation cycle.
 *
 * Four additive modes — replay (re-score), compact (dedupe), prune
 * (sweep expired), summarize (optional LLM compaction) — driven off a
 * single `memory_maintenance_run` entry point so the on-demand CLI and
 * the periodic scheduler share one code path.  An idle guard keyed on
 * (last_run_at, memory_count) short-circuits cycles where nothing has
 * changed, so callers can invoke maybe_run on every hook without
 * burning cycles.
 *
 * Mutation is DB-side; summary lives in-memory + cached in the
 * maintenance_state table so the dashboard card can render the last
 * cycle without re-running it.  See
 * docs/proposals/done/scheduled-memory-maintenance-cycles.md. */
#include "aimee.h"
#include "util.h"
#include "cJSON.h"
#include "db1_optional.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/curiosity.h"
#include "db2/memory_payload.h"
#endif
#include "log.h"
#include "memory.h"
#include <pthread.h>
#include <string.h>
#include <time.h>

#define MAINTENANCE_STATE_KEY "cycle"

static pthread_mutex_t s_mm_metrics_mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t s_mm_metric_runs_total = 0;
static int64_t s_mm_metric_skips_total = 0;
static int64_t s_mm_metric_changes_total = 0;
static double s_mm_metric_ms_sum = 0.0;
static double s_mm_metric_ms_max = 0.0;

void memory_maintenance_metrics(int64_t *runs_total, int64_t *skips_total, int64_t *changes_total,
                                double *ms_avg, double *ms_max)
{
   pthread_mutex_lock(&s_mm_metrics_mu);
   if (runs_total)
      *runs_total = s_mm_metric_runs_total;
   if (skips_total)
      *skips_total = s_mm_metric_skips_total;
   if (changes_total)
      *changes_total = s_mm_metric_changes_total;
   if (ms_avg)
      *ms_avg =
          s_mm_metric_runs_total > 0 ? s_mm_metric_ms_sum / (double)s_mm_metric_runs_total : 0.0;
   if (ms_max)
      *ms_max = s_mm_metric_ms_max;
   pthread_mutex_unlock(&s_mm_metrics_mu);
}

#if !defined(AIMEE_DB2_DISABLED)
static void mm_metrics_record(double ms, int skipped, int changes)
{
   pthread_mutex_lock(&s_mm_metrics_mu);
   if (skipped)
      s_mm_metric_skips_total++;
   else
   {
      s_mm_metric_runs_total++;
      s_mm_metric_ms_sum += ms;
      if (ms > s_mm_metric_ms_max)
         s_mm_metric_ms_max = ms;
      if (changes > 0)
         s_mm_metric_changes_total += changes;
   }
   pthread_mutex_unlock(&s_mm_metrics_mu);
}

/* mm_memory_count moved to db2/memory_payload.c (db2_memory_count). */

static void mm_state_save(const memory_maintenance_summary_t *summary)
{
   if (!summary)
      return;
   int changes = summary->promoted + summary->demoted + summary->expired +
                 summary->lifecycle_archived + summary->reminders_expired +
                 summary->directives_expired + summary->rescored +
                 summary->profile_cards_refreshed + summary->merged + summary->summarized;
   db1_maintenance_state_t st = {0};
   st.present = 1;
   now_utc(st.last_run_at, sizeof(st.last_run_at));
   st.last_memory_count = summary->memory_count_after;
   st.last_changes = changes;
   st.last_elapsed_ms = summary->elapsed_ms;
   snprintf(st.last_summary_json, sizeof(st.last_summary_json), "%s", summary->summary_json);
   if (db1_maintenance_state_save)
      (void)db1_maintenance_state_save(MAINTENANCE_STATE_KEY, &st);
}

/* Resolve the configured cadence (seconds between cycles). Zero / unset
 * falls back to the header default. */
static int mm_interval_secs(const config_t *cfg)
{
   if (cfg && cfg->memory_maintenance_interval_seconds > 0)
      return cfg->memory_maintenance_interval_seconds;
   return MEMORY_MAINTENANCE_DEFAULT_INTERVAL_SECS;
}

/* Idle guard: skip a cycle when the DB hasn't changed AND the cadence
 * hasn't lapsed.  Both predicates must hold for a skip — a burst of
 * inserts always gets the next cycle, regardless of timer. */
static int mm_should_skip(const config_t *cfg, const db1_maintenance_state_t *state,
                          int64_t current_count)
{
   if (!state->present)
      return 0; /* first cycle always runs */
   if (current_count != state->last_memory_count)
      return 0; /* new inserts — don't skip */

   int interval = mm_interval_secs(cfg);
   if (interval <= 0)
      return 0;

   time_t last_run = str_parse_iso8601(state->last_run_at);
   if (last_run == (time_t)0)
      return 0;
   int elapsed = (int)difftime(time(NULL), last_run);
   if (elapsed < 0)
      return 0;
   return elapsed < interval;
}
#endif

cJSON *memory_maintenance_summary_to_json(const memory_maintenance_summary_t *summary)
{
   cJSON *j = cJSON_CreateObject();
   if (!j || !summary)
      return j;
   cJSON_AddBoolToObject(j, "skipped", summary->skipped ? 1 : 0);
   cJSON_AddBoolToObject(j, "dry_run", summary->dry_run ? 1 : 0);
   cJSON_AddNumberToObject(j, "modes_run", summary->modes_run);
   cJSON_AddNumberToObject(j, "promoted", summary->promoted);
   cJSON_AddNumberToObject(j, "demoted", summary->demoted);
   cJSON_AddNumberToObject(j, "expired", summary->expired);
   cJSON_AddNumberToObject(j, "lifecycle_archived", summary->lifecycle_archived);
   cJSON_AddNumberToObject(j, "reminders_expired", summary->reminders_expired);
   cJSON_AddNumberToObject(j, "directives_expired", summary->directives_expired);
   cJSON_AddNumberToObject(j, "rescored", summary->rescored);
   cJSON_AddNumberToObject(j, "profile_cards_refreshed", summary->profile_cards_refreshed);
   cJSON_AddNumberToObject(j, "merged", summary->merged);
   cJSON_AddNumberToObject(j, "summarized", summary->summarized);
   cJSON_AddNumberToObject(j, "elapsed_ms", summary->elapsed_ms);
   cJSON_AddNumberToObject(j, "memory_count_before", (double)summary->memory_count_before);
   cJSON_AddNumberToObject(j, "memory_count_after", (double)summary->memory_count_after);
   return j;
}

#if defined(AIMEE_DB2_DISABLED)
/* The server profile keeps DB1-backed summary/metrics helpers from this file.
 * The maintenance runner itself is DB2-owned and must run inside aimee-kb. */
int memory_maintenance_run(const config_t *cfg, unsigned int modes, int force, int dry_run,
                           memory_maintenance_summary_t *summary)
{
   (void)cfg;
   (void)modes;
   (void)force;
   (void)dry_run;
   if (summary)
      memset(summary, 0, sizeof(*summary));
   return -1;
}

int memory_maintenance_maybe_run(const config_t *cfg, memory_maintenance_summary_t *summary_out)
{
   (void)cfg;
   if (summary_out)
      memset(summary_out, 0, sizeof(*summary_out));
   return 0;
}
#else
int memory_maintenance_run(const config_t *cfg, unsigned int modes, int force, int dry_run,
                           memory_maintenance_summary_t *summary)
{
   if (modes == 0)
      modes = MEMORY_MAINTENANCE_MODES_DEFAULT;

   memory_maintenance_summary_t local;
   memory_maintenance_summary_t *out = summary ? summary : &local;
   memset(out, 0, sizeof(*out));
   out->modes_run = (int)modes;
   out->dry_run = dry_run ? 1 : 0;

   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   int64_t count_before = db2_memory_count();
   out->memory_count_before = count_before;

   db1_maintenance_state_t state = {0};
   if (db1_maintenance_state_load)
      (void)db1_maintenance_state_load(MAINTENANCE_STATE_KEY, &state);

   if (!force && mm_should_skip(cfg, &state, count_before))
   {
      out->skipped = 1;
      out->memory_count_after = count_before;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      out->elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;
      cJSON *j = memory_maintenance_summary_to_json(out);
      char *js = j ? cJSON_PrintUnformatted(j) : NULL;
      if (js)
         snprintf(out->summary_json, sizeof(out->summary_json), "%s", js);
      free(js);
      cJSON_Delete(j);
      mm_metrics_record(out->elapsed_ms, 1, 0);
      return 0;
   }

   /* Replay: re-score effectiveness and refresh profile cards. */
   if (modes & MEMORY_MAINTENANCE_MODE_REPLAY)
   {
      if (!dry_run)
      {
         int rescored = memory_compute_effectiveness();
         if (rescored > 0)
            out->rescored = rescored;
         if (cfg && cfg->memory_profile_cards_enabled)
         {
            int min_obs =
                cfg->memory_profile_cards_min_obs > 0 ? cfg->memory_profile_cards_min_obs : 10;
            int stale = cfg->memory_profile_cards_stale_secs > 0
                            ? cfg->memory_profile_cards_stale_secs
                            : 86400;
            int refreshed = memory_profile_card_refresh(min_obs, stale);
            if (refreshed > 0)
               out->profile_cards_refreshed = refreshed;
         }
         /* Curiosity scoring rides on replay — novelty and progress
          * need to stay fresh for routing to pick the right top-N. */
         int curiosity_rescored = db2_curiosity_rescore_all();
         if (curiosity_rescored > 0)
         {
            out->rescored += curiosity_rescored;
            aimee_log(LOG_INFO, "curiosity", "maintenance rescored %d item(s)", curiosity_rescored);
         }
      }
   }

   /* Prune: lifecycle + reminders + directives + L0/L1 expiry + tier
    * promote/demote cycle.  Each helper is already idempotent. */
   if (modes & MEMORY_MAINTENANCE_MODE_PRUNE)
   {
      if (!dry_run)
      {
         int archived = memory_lifecycle_sweep_expired();
         if (archived > 0)
            out->lifecycle_archived = archived;
         int rem = memory_prospective_sweep_expired();
         if (rem > 0)
            out->reminders_expired = rem;
         int dirs = memory_directive_sweep_expired();
         if (dirs > 0)
            out->directives_expired = dirs;

         int promoted = 0, demoted = 0, expired = 0;
         memory_run_maintenance(&promoted, &demoted, &expired);
         out->promoted = promoted;
         out->demoted = demoted;
         out->expired = expired;
      }
   }

   /* Compact: dedupe near-duplicate keys via the improve module. */
   if (modes & MEMORY_MAINTENANCE_MODE_COMPACT)
   {
      if (cfg && cfg->memory_improve_dedupe_enabled)
      {
         int merged = memory_improve_dedupe(dry_run);
         if (merged > 0)
            out->merged = merged;
      }
   }

   /* Summarize: gated behind config flag; may call an LLM. */
   if (modes & MEMORY_MAINTENANCE_MODE_SUMMARIZE)
   {
      /* Either the maintenance-summarize gate or the (formerly inert) improve
       * summarise toggle enables the LLM-backed cluster summarisation. */
      int enabled = cfg && (cfg->memory_maintenance_summarize_enabled ||
                            cfg->memory_improve_summarise_enabled);
      if (enabled)
      {
         int min_cluster =
             cfg->memory_improve_min_cluster > 0 ? cfg->memory_improve_min_cluster : 3;
         double max_conf =
             cfg->memory_improve_max_confidence > 0.0 ? cfg->memory_improve_max_confidence : 0.6;
         int summarized = memory_improve_summarise(dry_run, min_cluster, max_conf);
         if (summarized > 0)
            out->summarized = summarized;
      }
   }

   out->memory_count_after = db2_memory_count();
   clock_gettime(CLOCK_MONOTONIC, &t1);
   out->elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;

   cJSON *j = memory_maintenance_summary_to_json(out);
   char *js = j ? cJSON_PrintUnformatted(j) : NULL;
   if (js)
      snprintf(out->summary_json, sizeof(out->summary_json), "%s", js);
   free(js);
   cJSON_Delete(j);

   int changes = out->promoted + out->demoted + out->expired + out->lifecycle_archived +
                 out->reminders_expired + out->directives_expired + out->rescored +
                 out->profile_cards_refreshed + out->merged + out->summarized;
   mm_metrics_record(out->elapsed_ms, 0, changes);

   if (!dry_run)
      mm_state_save(out);

   aimee_log(LOG_INFO, "memory.maintenance", "cycle modes=%u changes=%d elapsed_ms=%.2f dry_run=%d",
             modes, changes, out->elapsed_ms, dry_run ? 1 : 0);
   return 0;
}

int memory_maintenance_maybe_run(const config_t *cfg, memory_maintenance_summary_t *summary_out)
{
   if (!cfg || !cfg->memory_maintenance_enabled)
      return 0;
   memory_maintenance_summary_t summary;
   memset(&summary, 0, sizeof(summary));
   if (memory_maintenance_run(cfg, 0 /* default modes */, 0 /* honour idle guard */, 0 /* commit */,
                              &summary) != 0)
      return 0;
   if (summary_out)
      *summary_out = summary;
   return summary.skipped ? 0 : 1;
}
#endif

int memory_maintenance_last_summary(memory_maintenance_summary_t *out)
{
   if (!out)
      return -1;
   db1_maintenance_state_t state = {0};
   if (db1_maintenance_state_load)
      (void)db1_maintenance_state_load(MAINTENANCE_STATE_KEY, &state);
   if (!state.present)
      return -1;
   memset(out, 0, sizeof(*out));
   snprintf(out->summary_json, sizeof(out->summary_json), "%s", state.last_summary_json);
   out->elapsed_ms = state.last_elapsed_ms;
   out->memory_count_after = state.last_memory_count;
   cJSON *parsed = cJSON_Parse(state.last_summary_json);
   if (parsed)
   {
      cJSON *v;
      if ((v = cJSON_GetObjectItem(parsed, "skipped")) && cJSON_IsBool(v))
         out->skipped = cJSON_IsTrue(v) ? 1 : 0;
      if ((v = cJSON_GetObjectItem(parsed, "dry_run")) && cJSON_IsBool(v))
         out->dry_run = cJSON_IsTrue(v) ? 1 : 0;
      if ((v = cJSON_GetObjectItem(parsed, "modes_run")) && cJSON_IsNumber(v))
         out->modes_run = (int)v->valuedouble;
      if ((v = cJSON_GetObjectItem(parsed, "promoted")) && cJSON_IsNumber(v))
         out->promoted = (int)v->valuedouble;
      if ((v = cJSON_GetObjectItem(parsed, "demoted")) && cJSON_IsNumber(v))
         out->demoted = (int)v->valuedouble;
      if ((v = cJSON_GetObjectItem(parsed, "expired")) && cJSON_IsNumber(v))
         out->expired = (int)v->valuedouble;
      if ((v = cJSON_GetObjectItem(parsed, "lifecycle_archived")) && cJSON_IsNumber(v))
         out->lifecycle_archived = (int)v->valuedouble;
      if ((v = cJSON_GetObjectItem(parsed, "reminders_expired")) && cJSON_IsNumber(v))
         out->reminders_expired = (int)v->valuedouble;
      if ((v = cJSON_GetObjectItem(parsed, "directives_expired")) && cJSON_IsNumber(v))
         out->directives_expired = (int)v->valuedouble;
      if ((v = cJSON_GetObjectItem(parsed, "rescored")) && cJSON_IsNumber(v))
         out->rescored = (int)v->valuedouble;
      if ((v = cJSON_GetObjectItem(parsed, "profile_cards_refreshed")) && cJSON_IsNumber(v))
         out->profile_cards_refreshed = (int)v->valuedouble;
      if ((v = cJSON_GetObjectItem(parsed, "merged")) && cJSON_IsNumber(v))
         out->merged = (int)v->valuedouble;
      if ((v = cJSON_GetObjectItem(parsed, "summarized")) && cJSON_IsNumber(v))
         out->summarized = (int)v->valuedouble;
      if ((v = cJSON_GetObjectItem(parsed, "memory_count_before")) && cJSON_IsNumber(v))
         out->memory_count_before = (int64_t)v->valuedouble;
      if ((v = cJSON_GetObjectItem(parsed, "memory_count_after")) && cJSON_IsNumber(v))
         out->memory_count_after = (int64_t)v->valuedouble;
      cJSON_Delete(parsed);
   }
   return 0;
}
