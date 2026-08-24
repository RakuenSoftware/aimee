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

static char *recurrence_evidence_refs(const char *observation_id)
{
   cJSON *refs = cJSON_CreateArray();
   if (!refs)
      return NULL;
   cJSON *oref = cJSON_CreateObject();
   if (oref)
   {
      cJSON_AddStringToObject(oref, "kind", "learning_observation");
      cJSON_AddStringToObject(oref, "id", observation_id);
      cJSON_AddItemToArray(refs, oref);
   }
   int64_t event_ids[64];
   int event_n = db2_learning_observation_evidence_ids(observation_id, event_ids, 64);
   for (int i = 0; i < event_n; i++)
   {
      cJSON *eref = cJSON_CreateObject();
      if (!eref)
         continue;
      cJSON_AddStringToObject(eref, "kind", "interaction_event");
      cJSON_AddNumberToObject(eref, "stable_id", (double)event_ids[i]);
      cJSON_AddItemToArray(refs, eref);
   }
   char *json = cJSON_PrintUnformatted(refs);
   cJSON_Delete(refs);
   return json;
}

static const char *recurrence_observation_type(const char *failure_mode,
                                               const char *recovery_action, const char *outcome)
{
   if (recovery_action && recovery_action[0] && outcome && strcmp(outcome, "success") == 0)
      return "successful_recovery";
   if (failure_mode && (strstr(failure_mode, "precondition") || strstr(failure_mode, "missing")))
      return "missing_precondition";
   if (failure_mode && strstr(failure_mode, "tool"))
      return "tool_misuse";
   if (failure_mode && (strstr(failure_mode, "environment") || strstr(failure_mode, "service")))
      return "environment_mismatch";
   return "recurring_failure";
}

static int recurrence_collect_evidence(const char *scope_kind, const char *scope_id,
                                       const char *task_family, const char *role,
                                       const char *failure_mode, const char *error_signature,
                                       const char *recovery_action, int64_t max_event_id,
                                       learning_observation_evidence_input_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out || max <= 0)
      return -1;
   static const char *sql =
       "SELECT source_event_id FROM interaction_event_embeddings"
       " WHERE source_event_id<=?1 AND event_type IN ('delegate_exit','task_attempt')"
       " AND scope_kind=?2 AND scope_id=?3 AND task_family=?4 AND role=?5"
       " AND failure_mode=?6 AND error_signature=?7 AND recovery_action=?8"
       " ORDER BY source_event_id LIMIT ?9";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", max_event_id);
   aimee_pg_bind_text(st, "?2", scope_kind);
   aimee_pg_bind_text(st, "?3", scope_id);
   aimee_pg_bind_text(st, "?4", task_family);
   aimee_pg_bind_text(st, "?5", role);
   aimee_pg_bind_text(st, "?6", failure_mode);
   aimee_pg_bind_text(st, "?7", error_signature);
   aimee_pg_bind_text(st, "?8", recovery_action);
   aimee_pg_bind_int(st, "?9", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out[n].source_event_id = aimee_pg_column_int64(st, 0);
      out[n].source_span = "";
      out[n].stance = "supports";
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

static int propose_recurrence(const char *scope_kind, const char *scope_id, const char *task_family,
                              const char *role, const char *failure_mode,
                              const char *action_sequence, const char *error_signature,
                              const char *environment, const char *preconditions,
                              const char *outcome, const char *recovery_action, int evidence_count,
                              int session_count, int64_t max_event_id)
{
   char key[1024];
   snprintf(key, sizeof(key), "%s:%s:%s:%s:%s:%s:%s", scope_kind, scope_id, task_family, role,
            failure_mode, error_signature, recovery_action);
   char id[37];
   mining_uuid_for_key("workflow_pattern", key, id, sizeof(id));
   char observation_id[37];
   mining_uuid_for_key("learning_observation", key, observation_id, sizeof(observation_id));
   const char *observation_type =
       recurrence_observation_type(failure_mode, recovery_action, outcome);

   double confidence = 0.5 + 0.1 * (double)evidence_count;
   if (confidence > 0.95)
      confidence = 0.95;

   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return -1;
   char title[256];
   snprintf(title, sizeof(title), "%s: %s in role %s",
            strcmp(observation_type, "successful_recovery") == 0 ? "Recovery" : "Recurring",
            failure_mode && failure_mode[0] ? failure_mode : "delegate failure",
            role && role[0] ? role : "unknown");
   char summary[512];
   snprintf(summary, sizeof(summary),
            "The %s role produced failure mode '%s' in %d events across %d independent sessions.",
            role && role[0] ? role : "unknown",
            failure_mode && failure_mode[0] ? failure_mode : "delegate failure", evidence_count,
            session_count);
   learning_observation_evidence_input_t evidence[64];
   int collected =
       recurrence_collect_evidence(scope_kind, scope_id, task_family, role, failure_mode,
                                   error_signature, recovery_action, max_event_id, evidence, 64);
   if (collected < 0 || db2_learning_observation_refresh(
                            observation_id, scope_kind, scope_id, observation_type, title, summary,
                            "attempt-pattern-v2", evidence, collected, "") != 0)
   {
      cJSON_Delete(payload);
      return -1;
   }
   cJSON_AddStringToObject(payload, "title", title);
   cJSON_AddStringToObject(payload, "observation_id", observation_id);
   cJSON_AddStringToObject(payload, "observation_type", observation_type);
   cJSON_AddStringToObject(payload, "scope_kind", scope_kind);
   cJSON_AddStringToObject(payload, "scope_id", scope_id);
   cJSON_AddStringToObject(payload, "task_family", task_family);
   cJSON_AddStringToObject(payload, "role", role ? role : "");
   cJSON_AddStringToObject(payload, "failure_mode", failure_mode ? failure_mode : "");
   cJSON_AddStringToObject(payload, "action_sequence", action_sequence);
   cJSON_AddStringToObject(payload, "error_signature", error_signature);
   cJSON_AddStringToObject(payload, "environment", environment);
   cJSON_AddStringToObject(payload, "preconditions", preconditions);
   cJSON_AddStringToObject(payload, "outcome", outcome);
   cJSON_AddStringToObject(payload, "recovery_action", recovery_action);
   cJSON_AddNumberToObject(payload, "evidence_count", evidence_count);
   cJSON_AddNumberToObject(payload, "session_count", session_count);
   cJSON_AddNumberToObject(payload, "max_event_id", (double)max_event_id);
   cJSON_AddStringToObject(payload, "body", summary);

   /* Learning surface: check for existing case precedent before proposing. */
   {
      char *tmp_json = cJSON_PrintUnformatted(payload);
      if (tmp_json)
      {
         kb_reasoning_case_result_t cases[1];
         int nc = kb_reasoning_case_recall(tmp_json, scope_kind, scope_id, cases, 1);
         if (nc > 0 && cases[0].artifact_id[0])
            cJSON_AddStringToObject(payload, "prior_case_id", cases[0].artifact_id);
         free(tmp_json);
      }
   }

   char *json = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   if (!json)
      return -1;
   char *refs_json = recurrence_evidence_refs(observation_id);

   /* An observation is interpretation, never self-authorized action. In shadow
    * mode the derived row is the terminal output. When reviewed failure learning
    * is enabled it may additionally emit signal -> proposal -> review; there is
    * deliberately no direct artifact write on either branch. */
   {
      if (config_kb_mining_failure_learning_enabled())
      {
         char target_key[1024];
         if (!scope_id[0] && !task_family[0] && !error_signature[0] && !recovery_action[0])
            snprintf(target_key, sizeof(target_key), "delegate_exit:%s:%s", role, failure_mode);
         else
            snprintf(target_key, sizeof(target_key), "attempt:%s", key);
         int existing = db2_learning_proposal_find_pending("artifact", target_key, 0);
         if (existing > 0)
         {
            db2_learning_proposal_bump_corroboration(existing);
            if (refs_json)
               (void)db2_learning_proposal_refresh_evidence(existing, refs_json);
            free(refs_json);
            free(json);
            return 0;
         }
         cJSON *action = cJSON_CreateObject();
         if (action)
         {
            cJSON_AddStringToObject(action, "artifact_id", id);
            cJSON_AddStringToObject(action, "artifact_kind", "workflow_pattern");
            cJSON_AddStringToObject(action, "scope_kind", scope_kind);
            cJSON_AddStringToObject(action, "scope_id", scope_id);
            cJSON_AddNumberToObject(action, "confidence", confidence);
            cJSON_AddStringToObject(action, "payload_json", json);
            cJSON_AddStringToObject(action, "observation_id", observation_id);
            cJSON_AddStringToObject(action, "triggering_preconditions",
                                    preconditions[0] ? preconditions : title);
            cJSON_AddStringToObject(
                action, "proposed_action",
                recovery_action[0]
                    ? recovery_action
                    : "Review the cited attempts and approve a bounded recovery procedure.");
            cJSON_AddStringToObject(action, "expected_outcome",
                                    "Reduce recurrence of the cited failure mode.");
            cJSON_AddStringToObject(action, "do_not_apply_when",
                                    "Do not apply outside the cited scope, task family, role, "
                                    "failure mode, and preconditions without separate evidence.");
            cJSON_AddStringToObject(
                action, "rollback",
                "Retire the promoted procedure and preserve this observation and its outcomes.");
            char *action_json = cJSON_PrintUnformatted(action);
            cJSON_Delete(action);
            if (action_json)
            {
               learning_signal_input_t sig;
               memset(&sig, 0, sizeof(sig));
               snprintf(sig.signal_type, sizeof(sig.signal_type), "%s",
                        strcmp(observation_type, "successful_recovery") == 0
                            ? "successful_recovery"
                            : "recurrence_failure");
               snprintf(sig.source, sizeof(sig.source), "kb-mining");
               snprintf(sig.title, sizeof(sig.title), "%s", title);
               snprintf(sig.description, sizeof(sig.description),
                        "Recurring %s in role %s: %d events across %d sessions",
                        failure_mode && failure_mode[0] ? failure_mode : "delegate failure",
                        role && role[0] ? role : "unknown", evidence_count, session_count);
               snprintf(sig.target_key, sizeof(sig.target_key), "%s", target_key);
               sig.evidence_refs_json = refs_json;
               int sigid = db2_learning_signal_insert(&sig, "");
               if (sigid > 0)
               {
                  char expires[32];
                  recurrence_expiry(30, expires, sizeof(expires));
                  (void)db2_learning_proposal_insert(sigid, "artifact", target_key, 0, action_json,
                                                     refs_json, expires);
               }
               free(action_json);
            }
         }
         free(refs_json);
         free(json);
         return 0;
      }
   }
   free(refs_json);
   free(json);
   return 0;
}

static int run_recurrence(int64_t hwm, int64_t *new_hwm_out)
{
   void *conn = db2_conn();
   if (!conn || !new_hwm_out)
      return -1;
   if (db2_learning_observations_reconcile() != 0)
      return -1;
   *new_hwm_out = hwm;
   if (query_max_event_id(hwm, "event_type IN ('delegate_exit','task_attempt')", new_hwm_out) != 0)
      return -1;
   if (*new_hwm_out <= hwm)
      return 0;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT scope_kind,scope_id,task_family,role,failure_mode,action_sequence,error_signature,"
       " environment,preconditions,outcome,recovery_action,COUNT(*),COUNT(DISTINCT session_id),"
       " MAX(source_event_id)"
       " FROM interaction_event_embeddings"
       " WHERE source_event_id <= ?2"
       "   AND event_type IN ('delegate_exit','task_attempt')"
       "   AND (failure_mode <> '' OR recovery_action <> '')"
       " GROUP BY scope_kind,scope_id,task_family,role,failure_mode,action_sequence,"
       " error_signature,environment,preconditions,outcome,recovery_action"
       " HAVING MAX(source_event_id)>?1 AND COUNT(DISTINCT session_id)>=2"
       " AND ((recovery_action<>'' AND outcome='success' AND COUNT(*)>=2) OR COUNT(*)>=3)"
       " ORDER BY COUNT(*) DESC"
       " LIMIT 16",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", hwm);
   aimee_pg_bind_int64(st, "?2", *new_hwm_out);
   int rc = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *scope_kind = aimee_pg_column_text(st, 0);
      const char *scope_id = aimee_pg_column_text(st, 1);
      const char *task_family = aimee_pg_column_text(st, 2);
      const char *role = aimee_pg_column_text(st, 3);
      const char *failure_mode = aimee_pg_column_text(st, 4);
      const char *action_sequence = aimee_pg_column_text(st, 5);
      const char *error_signature = aimee_pg_column_text(st, 6);
      const char *environment = aimee_pg_column_text(st, 7);
      const char *preconditions = aimee_pg_column_text(st, 8);
      const char *outcome = aimee_pg_column_text(st, 9);
      const char *recovery_action = aimee_pg_column_text(st, 10);
      int evidence_count = aimee_pg_column_int(st, 11);
      int session_count = aimee_pg_column_int(st, 12);
      int64_t max_event_id = aimee_pg_column_int64(st, 13);
      if (propose_recurrence(scope_kind, scope_id, task_family, role, failure_mode, action_sequence,
                             error_signature, environment, preconditions, outcome, recovery_action,
                             evidence_count, session_count, max_event_id) != 0)
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
   {
      LOG_WARN("kb.mining", "could not seed mining job definitions");
      return -1;
   }

   int ran = 0;
   for (int i = 0; JOBS[i].id; i++)
   {
      db2_mining_job_row_t row;
      if (db2_mining_job_get(JOBS[i].id, &row) != 0)
      {
         LOG_WARN("kb.mining", "could not load job %s", JOBS[i].id);
         continue;
      }
      if (!row.enabled)
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
      if (rc != 0)
         LOG_WARN("kb.mining", "job %s failed at hwm=%lld", JOBS[i].id, (long long)row.hwm);
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
   LOG_INFO("kb.mining", "scheduler started (minimum poll=%ds)", min_poll_s);

   while (!g_mining_stop)
   {
      db2_lease_begin(); /* WP-C: hold a pool lease only during the cycle */
      int ran = kb_mining_run_once();
      db2_lease_end();
      LOG_DEBUG("kb.mining", "scheduler tick completed (jobs=%d)", ran);
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
