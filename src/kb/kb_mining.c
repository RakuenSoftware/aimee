/* kb_mining.c: aimee-kb continuous mining scheduler and initial jobs. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_mining.h"
#include "kb_background.h"
#include "kb_reasoning.h"
#include "log.h"
#include "cJSON.h"
#include "config.h"
#include "db2/artifacts.h"
#include "db2/db2_learning.h"
#include "learning.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"
#include "db2/feature_rows.h"
#include "db2/mining.h"
#include "kb_mdl.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef AIMEE_WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#endif

typedef int (*mining_run_fn)(int64_t hwm, int64_t *new_hwm_out);

typedef struct
{
   const char *id;
   mining_run_fn run;
} mining_job_t;

static pthread_t g_mining_thread;
static volatile int g_mining_stop;
static int g_mining_active;

static void mining_sleep_one_second(void)
{
#ifdef AIMEE_WINDOWS
   Sleep(1000);
#else
   sleep(1);
#endif
}

static int parse_job_timestamp(const char *value, time_t *out)
{
   int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
   if (!value || !value[0] || !out)
      return 0;
   if (sscanf(value, "%d-%d-%d %d:%d:%d", &year, &mon, &day, &hour, &min, &sec) != 6)
      return 0;

   struct tm tmv;
   memset(&tmv, 0, sizeof(tmv));
   tmv.tm_year = year - 1900;
   tmv.tm_mon = mon - 1;
   tmv.tm_mday = day;
   tmv.tm_hour = hour;
   tmv.tm_min = min;
   tmv.tm_sec = sec;
   tmv.tm_isdst = 0;
#ifdef AIMEE_WINDOWS
   time_t parsed = _mkgmtime(&tmv);
#else
   time_t parsed = timegm(&tmv);
#endif
   if (parsed == (time_t)-1)
      return 0;
   *out = parsed;
   return 1;
}

static int mining_job_due(const db2_mining_job_row_t *row)
{
   if (!row || !row->last_run_at[0] || row->interval_s <= 0)
      return 1;

   time_t last = 0;
   if (!parse_job_timestamp(row->last_run_at, &last))
      return 1;
   return difftime(time(NULL), last) >= (double)row->interval_s;
}

static uint64_t fnv1a64(const char *s)
{
   uint64_t h = 1469598103934665603ULL;
   if (!s)
      return h;
   while (*s)
   {
      h ^= (unsigned char)*s++;
      h *= 1099511628211ULL;
   }
   return h;
}

static void mining_uuid_for_key(const char *prefix, const char *key, char *out, size_t out_len)
{
   uint64_t h1 = fnv1a64(prefix);
   uint64_t h2 = fnv1a64(key);
   snprintf(out, out_len, "%08x-%04x-%04x-%04x-%012llx", (unsigned)(h1 & 0xffffffffU),
            (unsigned)((h1 >> 32) & 0xffffU), (unsigned)(h2 & 0xffffU),
            (unsigned)((h2 >> 16) & 0xffffU), (unsigned long long)(h2 & 0xffffffffffffULL));
}

static int query_max_event_id(int64_t hwm, const char *where_sql, int64_t *max_out)
{
   void *conn = db2_conn();
   if (!conn || !max_out)
      return -1;
   *max_out = hwm;

   char sql[512];
   snprintf(sql, sizeof(sql),
            "SELECT COALESCE(MAX(source_event_id), ?1)"
            " FROM interaction_event_embeddings WHERE source_event_id > ?1 AND %s",
            where_sql);
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", hwm);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      *max_out = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return 0;
}

static int propose_pattern_cluster(const char *cluster_key, int count, int64_t max_event_id)
{
   char id[37];
   mining_uuid_for_key("interaction_pattern", cluster_key, id, sizeof(id));

   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return -1;
   cJSON_AddStringToObject(payload, "title", "Recurring interaction pattern cluster");
   cJSON_AddStringToObject(payload, "cluster_key", cluster_key);
   cJSON_AddNumberToObject(payload, "evidence_count", count);
   cJSON_AddNumberToObject(payload, "max_event_id", (double)max_event_id);
   char *json = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   if (!json)
      return -1;

   double confidence = count >= 20 ? 0.85 : 0.70;
   int rc = db2_artifact_write(id, "interaction_pattern", "proposed", "workspace", "", "kb-mining",
                               confidence, json);
   if (rc == 0 && cluster_key && cluster_key[0])
   {
      kb_mdl_score_t mdl = {0};
      if (kb_mdl_score(json, cluster_key, &mdl) == 0)
      {
         char feat[256];
         snprintf(feat, sizeof(feat),
                  "{\"mdl.l_candidate\":%.2f,\"mdl.l_residual\":%.2f,"
                  "\"mdl.total\":%.2f,\"mdl.rank_in_cluster\":%d}",
                  mdl.l_candidate, mdl.l_residual, mdl.total, mdl.rank_in_cluster);
         db2_feature_row_upsert(id, "kb_artifact", "", "", "v1", feat, NULL);
      }
   }
   free(json);
   return rc;
}

static int run_pattern_cluster(int64_t hwm, int64_t *new_hwm_out)
{
   void *conn = db2_conn();
   if (!conn || !new_hwm_out)
      return -1;
   *new_hwm_out = hwm;
   if (query_max_event_id(hwm,
                          "event_type IN ('delegate_exit','guardrail_decision','user_correction')",
                          new_hwm_out) != 0)
      return -1;
   if (*new_hwm_out <= hwm)
      return 0;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT cluster_key, COUNT(*), MAX(source_event_id)"
       " FROM interaction_event_embeddings"
       " WHERE source_event_id > ?1"
       "   AND event_type IN ('delegate_exit','guardrail_decision','user_correction')"
       "   AND cluster_key <> ''"
       " GROUP BY cluster_key"
       " HAVING COUNT(*) >= 5"
       " ORDER BY COUNT(*) DESC"
       " LIMIT 16",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", hwm);
   int rc = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *cluster_key = aimee_pg_column_text(st, 0);
      int count = aimee_pg_column_int(st, 1);
      int64_t max_event_id = aimee_pg_column_int64(st, 2);
      if (cluster_key && cluster_key[0] &&
          propose_pattern_cluster(cluster_key, count, max_event_id) != 0)
         rc = -1;
   }
   aimee_pg_finalize(st);
   return rc;
}

/* Format a UTC "YYYY-MM-DD HH:MM:SS" timestamp `days` days from now, for a
 * learning proposal's expires_at (the §4 failure-learning path). */
static void recurrence_expiry(int days, char *buf, size_t len)
{
   time_t now = time(NULL);
   time_t future = now + (time_t)days * 24 * 60 * 60;
   struct tm tmv;
   gmtime_r(&future, &tmv);
   strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tmv);
}

static int propose_recurrence(const char *role, const char *failure_mode, int evidence_count,
                              int session_count, int64_t max_event_id)
{
   char key[256];
   snprintf(key, sizeof(key), "%s:%s", role ? role : "", failure_mode ? failure_mode : "");
   char id[37];
   mining_uuid_for_key("workflow_pattern", key, id, sizeof(id));

   double confidence = 0.5 + 0.1 * (double)evidence_count;
   if (confidence > 0.95)
      confidence = 0.95;

   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return -1;
   char title[256];
   snprintf(title, sizeof(title), "Recurring: %s in role %s",
            failure_mode && failure_mode[0] ? failure_mode : "delegate failure",
            role && role[0] ? role : "unknown");
   cJSON_AddStringToObject(payload, "title", title);
   cJSON_AddStringToObject(payload, "role", role ? role : "");
   cJSON_AddStringToObject(payload, "failure_mode", failure_mode ? failure_mode : "");
   cJSON_AddNumberToObject(payload, "evidence_count", evidence_count);
   cJSON_AddNumberToObject(payload, "session_count", session_count);
   cJSON_AddNumberToObject(payload, "max_event_id", (double)max_event_id);
   cJSON_AddStringToObject(payload, "body",
                           "Repeated delegate failures should be reviewed as a workflow pattern.");

   /* Learning surface: check for existing case precedent before proposing. */
   {
      config_t mining_cfg;
      config_load(&mining_cfg);
      char *tmp_json = cJSON_PrintUnformatted(payload);
      if (tmp_json)
      {
         kb_reasoning_case_result_t cases[1];
         int nc = kb_reasoning_case_recall(&mining_cfg, tmp_json, "workspace", "", cases, 1);
         if (nc > 0 && cases[0].artifact_id[0])
            cJSON_AddStringToObject(payload, "prior_case_id", cases[0].artifact_id);
         free(tmp_json);
      }
   }

   char *json = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   if (!json)
      return -1;

   /* ingress-compression §4: when failure-learning is enabled, route the cluster
    * through the learning pipeline (signal -> reviewed/Gate-Promoted proposal ->
    * artifact) instead of writing the workflow_pattern artifact directly. Dedup on
    * the event_type:role:failure_mode cluster so the same recurrence corroborates
    * an existing pending proposal rather than spamming new ones (and never produces
    * both a direct artifact and a proposal for one cluster). */
   {
      config_t lcfg;
      config_load(&lcfg);
      if (lcfg.kb_mining_failure_learning_enabled)
      {
         char target_key[256];
         snprintf(target_key, sizeof(target_key), "delegate_exit:%s:%s", role ? role : "",
                  failure_mode ? failure_mode : "");
         int existing = db2_learning_proposal_find_pending("artifact", target_key, 0);
         if (existing > 0)
         {
            db2_learning_proposal_bump_corroboration(existing);
            free(json);
            return 0;
         }
         cJSON *action = cJSON_CreateObject();
         if (action)
         {
            cJSON_AddStringToObject(action, "artifact_id", id);
            cJSON_AddStringToObject(action, "artifact_kind", "workflow_pattern");
            cJSON_AddStringToObject(action, "scope_kind", "workspace");
            cJSON_AddNumberToObject(action, "confidence", confidence);
            cJSON_AddStringToObject(action, "payload_json", json);
            char *action_json = cJSON_PrintUnformatted(action);
            cJSON_Delete(action);
            if (action_json)
            {
               learning_signal_input_t sig;
               memset(&sig, 0, sizeof(sig));
               snprintf(sig.signal_type, sizeof(sig.signal_type), "recurrence_failure");
               snprintf(sig.source, sizeof(sig.source), "kb-mining");
               snprintf(sig.title, sizeof(sig.title), "%s", title);
               snprintf(sig.description, sizeof(sig.description),
                        "Recurring %s in role %s: %d events across %d sessions",
                        failure_mode && failure_mode[0] ? failure_mode : "delegate failure",
                        role && role[0] ? role : "unknown", evidence_count, session_count);
               snprintf(sig.target_key, sizeof(sig.target_key), "%s", target_key);
               int sigid = db2_learning_signal_insert(&sig, "");
               if (sigid > 0)
               {
                  char expires[32];
                  recurrence_expiry(30, expires, sizeof(expires));
                  (void)db2_learning_proposal_insert(sigid, "artifact", target_key, 0, action_json,
                                                     NULL, expires);
               }
               free(action_json);
            }
         }
         free(json);
         return 0;
      }
   }

   int rc = db2_artifact_write(id, "workflow_pattern", "proposed", "workspace", "", "kb-mining",
                               confidence, json);
   if (rc == 0 && key[0])
   {
      kb_mdl_score_t mdl = {0};
      if (kb_mdl_score(json, key, &mdl) == 0)
      {
         char feat[256];
         snprintf(feat, sizeof(feat),
                  "{\"mdl.l_candidate\":%.2f,\"mdl.l_residual\":%.2f,"
                  "\"mdl.total\":%.2f,\"mdl.rank_in_cluster\":%d}",
                  mdl.l_candidate, mdl.l_residual, mdl.total, mdl.rank_in_cluster);
         db2_feature_row_upsert(id, "kb_artifact", "", "", "v1", feat, NULL);
      }
   }
   free(json);
   return rc;
}

static int run_recurrence(int64_t hwm, int64_t *new_hwm_out)
{
   void *conn = db2_conn();
   if (!conn || !new_hwm_out)
      return -1;
   *new_hwm_out = hwm;
   if (query_max_event_id(hwm, "event_type = 'delegate_exit'", new_hwm_out) != 0)
      return -1;
   if (*new_hwm_out <= hwm)
      return 0;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT role, failure_mode, COUNT(*), COUNT(DISTINCT session_id), MAX(source_event_id)"
       " FROM interaction_event_embeddings"
       " WHERE source_event_id > ?1"
       "   AND event_type = 'delegate_exit'"
       "   AND failure_mode <> ''"
       " GROUP BY role, failure_mode"
       " HAVING COUNT(*) >= 3 AND COUNT(DISTINCT session_id) >= 2"
       " ORDER BY COUNT(*) DESC"
       " LIMIT 16",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", hwm);
   int rc = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *role = aimee_pg_column_text(st, 0);
      const char *failure_mode = aimee_pg_column_text(st, 1);
      int evidence_count = aimee_pg_column_int(st, 2);
      int session_count = aimee_pg_column_int(st, 3);
      int64_t max_event_id = aimee_pg_column_int64(st, 4);
      if (propose_recurrence(role, failure_mode, evidence_count, session_count, max_event_id) != 0)
         rc = -1;
   }
   aimee_pg_finalize(st);
   return rc;
}

static const mining_job_t JOBS[] = {
    {"pattern_cluster", run_pattern_cluster},
    {"recurrence", run_recurrence},
    {NULL, NULL},
};

int kb_mining_run_once(void)
{
   if (db2_mining_seed_job_defaults() != 0)
      return -1;

   int ran = 0;
   for (int i = 0; JOBS[i].id; i++)
   {
      db2_mining_job_row_t row;
      if (db2_mining_job_get(JOBS[i].id, &row) != 0 || !row.enabled)
         continue;
      if (!mining_job_due(&row))
         continue;
      if (!db2_mining_job_try_lock(JOBS[i].id))
         continue;

      int64_t new_hwm = row.hwm;
      kb_background_set("mining", "job=%s hwm=%lld", JOBS[i].id, (long long)row.hwm);
      int rc = JOBS[i].run(row.hwm, &new_hwm);
      kb_background_clear("mining");
      (void)db2_mining_job_complete(JOBS[i].id, new_hwm, rc == 0 ? "" : "job failed");
      db2_mining_job_unlock(JOBS[i].id);
      ran++;
   }
   return ran;
}

static void *mining_thread_main(void *arg)
{
   int min_poll_s = *(int *)arg;
   free(arg);
   if (min_poll_s <= 0)
      min_poll_s = 300;

   while (!g_mining_stop)
   {
      db2_lease_begin(); /* WP-C: hold a pool lease only during the cycle */
      (void)kb_mining_run_once();
      db2_lease_end();
      for (int i = 0; i < min_poll_s && !g_mining_stop; i++)
         mining_sleep_one_second();
   }
   return NULL;
}

int kb_mining_start(int min_poll_s)
{
   if (g_mining_active)
      return 0;
   int *arg = malloc(sizeof(int));
   if (!arg)
      return -1;
   *arg = min_poll_s;
   g_mining_stop = 0;
   if (pthread_create(&g_mining_thread, NULL, mining_thread_main, arg) != 0)
   {
      free(arg);
      return -1;
   }
   g_mining_active = 1;
   return 0;
}

void kb_mining_stop(void)
{
   if (!g_mining_active)
      return;
   g_mining_stop = 1;
   pthread_join(g_mining_thread, NULL);
   g_mining_active = 0;
}

int kb_mining_active(void)
{
   return g_mining_active;
}
