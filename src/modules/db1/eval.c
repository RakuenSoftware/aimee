/* db1/eval.c: per-machine evaluation run results. */

#include "eval.h"
#include "db1_internal.h"

#include "cJSON.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int db1_eval_result_insert(const db1_eval_result_row_t *row)
{
   if (!row || !row->suite || !row->task_name)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO eval_results (suite, task_name, agent_name, ablation, success, turns,"
       " tool_calls, tool_call_failures, rescue_recoveries, prompt_tokens, completion_tokens,"
       " latency_ms, response, error, dataset_hash, target_hash, harness_version,"
       " hardware_profile, seed)"
       " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, row->suite, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, row->task_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, row->agent_name ? row->agent_name : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, row->ablation ? row->ablation : "full", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 5, row->success);
   sqlite3_bind_int(stmt, 6, row->turns);
   sqlite3_bind_int(stmt, 7, row->tool_calls);
   sqlite3_bind_int(stmt, 8, row->tool_call_failures);
   sqlite3_bind_int(stmt, 9, row->rescue_recoveries);
   sqlite3_bind_int(stmt, 10, row->prompt_tokens);
   sqlite3_bind_int(stmt, 11, row->completion_tokens);
   sqlite3_bind_int(stmt, 12, row->latency_ms);
   sqlite3_bind_text(stmt, 13, row->response ? row->response : "", -1, SQLITE_TRANSIENT);
   if (row->error)
      sqlite3_bind_text(stmt, 14, row->error, -1, SQLITE_TRANSIENT);
   else
      sqlite3_bind_null(stmt, 14);
   sqlite3_bind_text(stmt, 15, row->dataset_hash ? row->dataset_hash : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 16, row->target_hash ? row->target_hash : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 17, row->harness_version ? row->harness_version : "1", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 18, row->hardware_profile ? row->hardware_profile : "", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 19, row->seed);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_eval_failed_tasks_recent(db1_eval_failed_task_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT DISTINCT task_name, error FROM eval_results"
                            " WHERE success = 0"
                            "   AND created_at > datetime('now', '-7 days')"
                            " ORDER BY created_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *t = sqlite3_column_text(stmt, 0);
      const unsigned char *e = sqlite3_column_text(stmt, 1);
      snprintf(out[n].task_name, sizeof(out[n].task_name), "%s", t ? (const char *)t : "");
      snprintf(out[n].error, sizeof(out[n].error), "%s", e ? (const char *)e : "");
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_eval_passed_tasks_recent(db1_eval_passed_task_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT DISTINCT task_name FROM eval_results"
                            " WHERE success = 1"
                            "   AND created_at > datetime('now', '-7 days')"
                            " ORDER BY created_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *t = sqlite3_column_text(stmt, 0);
      snprintf(out[n].task_name, sizeof(out[n].task_name), "%s", t ? (const char *)t : "");
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_eval_results_list(const char *suite_or_null, db1_eval_display_row_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   const char *sql = suite_or_null
                         ? "SELECT suite, task_name, agent_name, ablation, success, turns,"
                           " tool_calls, tool_call_failures, rescue_recoveries, latency_ms,"
                           " created_at"
                           " FROM eval_results WHERE suite = ? ORDER BY id DESC LIMIT ?"
                         : "SELECT suite, task_name, agent_name, ablation, success, turns,"
                           " tool_calls, tool_call_failures, rescue_recoveries, latency_ms,"
                           " created_at"
                           " FROM eval_results ORDER BY id DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   if (suite_or_null)
   {
      sqlite3_bind_text(stmt, 1, suite_or_null, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 2, max);
   }
   else
   {
      sqlite3_bind_int(stmt, 1, max);
   }

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *s = sqlite3_column_text(stmt, 0);
      const unsigned char *t = sqlite3_column_text(stmt, 1);
      const unsigned char *a = sqlite3_column_text(stmt, 2);
      const unsigned char *ab = sqlite3_column_text(stmt, 3);
      const unsigned char *c = sqlite3_column_text(stmt, 10);
      snprintf(out[n].suite, sizeof(out[n].suite), "%s", s ? (const char *)s : "");
      snprintf(out[n].task_name, sizeof(out[n].task_name), "%s", t ? (const char *)t : "");
      snprintf(out[n].agent_name, sizeof(out[n].agent_name), "%s", a ? (const char *)a : "");
      snprintf(out[n].ablation, sizeof(out[n].ablation), "%s", ab ? (const char *)ab : "");
      out[n].success = sqlite3_column_int(stmt, 4);
      out[n].turns = sqlite3_column_int(stmt, 5);
      out[n].tool_calls = sqlite3_column_int(stmt, 6);
      out[n].tool_call_failures = sqlite3_column_int(stmt, 7);
      out[n].rescue_recoveries = sqlite3_column_int(stmt, 8);
      out[n].latency_ms = sqlite3_column_int(stmt, 9);
      snprintf(out[n].created_at, sizeof(out[n].created_at), "%s", c ? (const char *)c : "");
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

/* --- Synthesised regression candidates (recursive-self-improvement S1) --- */

/* Column order shared by every candidate SELECT below. */
#define EVAL_CAND_COLUMNS                                                                          \
   "id, signature, state, suite, task_name, task_json, origin, origin_ref, occurrences,"           \
   " sessions_json, admitted_by, admitted_path, reject_reason, passing_windows, created_at,"       \
   " updated_at"

static void eval_cand_copy(char *dst, size_t dstsz, const unsigned char *src)
{
   snprintf(dst, dstsz, "%s", src ? (const char *)src : "");
}

/* Count the retained session ids, so admission can require distinct sessions
 * without a child table. A malformed set counts as zero rather than failing
 * the read — the row is still useful, it just cannot be admitted yet. */
static int eval_cand_session_count(const char *sessions_json)
{
   if (!sessions_json || !sessions_json[0])
      return 0;
   cJSON *arr = cJSON_Parse(sessions_json);
   if (!cJSON_IsArray(arr))
   {
      cJSON_Delete(arr);
      return 0;
   }
   int n = cJSON_GetArraySize(arr);
   cJSON_Delete(arr);
   return n;
}

static void eval_cand_load_row(sqlite3_stmt *stmt, db1_eval_candidate_t *out)
{
   memset(out, 0, sizeof(*out));
   out->id = sqlite3_column_int64(stmt, 0);
   eval_cand_copy(out->signature, sizeof(out->signature), sqlite3_column_text(stmt, 1));
   eval_cand_copy(out->state, sizeof(out->state), sqlite3_column_text(stmt, 2));
   eval_cand_copy(out->suite, sizeof(out->suite), sqlite3_column_text(stmt, 3));
   eval_cand_copy(out->task_name, sizeof(out->task_name), sqlite3_column_text(stmt, 4));
   eval_cand_copy(out->task_json, sizeof(out->task_json), sqlite3_column_text(stmt, 5));
   eval_cand_copy(out->origin, sizeof(out->origin), sqlite3_column_text(stmt, 6));
   eval_cand_copy(out->origin_ref, sizeof(out->origin_ref), sqlite3_column_text(stmt, 7));
   out->occurrences = sqlite3_column_int(stmt, 8);
   {
      const unsigned char *sessions = sqlite3_column_text(stmt, 9);
      out->distinct_sessions = eval_cand_session_count(sessions ? (const char *)sessions : "");
   }
   eval_cand_copy(out->admitted_by, sizeof(out->admitted_by), sqlite3_column_text(stmt, 10));
   eval_cand_copy(out->admitted_path, sizeof(out->admitted_path), sqlite3_column_text(stmt, 11));
   eval_cand_copy(out->reject_reason, sizeof(out->reject_reason), sqlite3_column_text(stmt, 12));
   out->passing_windows = sqlite3_column_int(stmt, 13);
   eval_cand_copy(out->created_at, sizeof(out->created_at), sqlite3_column_text(stmt, 14));
   eval_cand_copy(out->updated_at, sizeof(out->updated_at), sqlite3_column_text(stmt, 15));
}

/* Merge one session id into the retained set, preserving insertion order and
 * capping the size. Returns a freshly allocated JSON array string the caller
 * must free, or NULL on allocation failure. Sets *was_new to 0 when the id was
 * already present — the caller uses that to keep observation idempotent per
 * session, so re-running a scan over the same ledger does not inflate the
 * count. A caller that supplies no session id has given us no identity to
 * deduplicate on, so that always counts as new. */
static char *eval_cand_merge_session(const char *sessions_json, const char *session_id,
                                     int *was_new)
{
   if (was_new)
      *was_new = 1;
   cJSON *arr = (sessions_json && sessions_json[0]) ? cJSON_Parse(sessions_json) : NULL;
   if (!cJSON_IsArray(arr))
   {
      cJSON_Delete(arr);
      arr = cJSON_CreateArray();
   }
   if (!arr)
      return NULL;

   if (session_id && session_id[0])
   {
      int seen = 0;
      cJSON *item = NULL;
      cJSON_ArrayForEach(item, arr)
      {
         if (cJSON_IsString(item) && strcmp(item->valuestring, session_id) == 0)
         {
            seen = 1;
            break;
         }
      }
      if (was_new)
         *was_new = !seen;
      /* Past the cap the set stops growing, but a session it cannot remember
       * still counts as new — by then the candidate is far past any
       * admission bar, so the only cost is a counter that keeps rising. */
      if (!seen && cJSON_GetArraySize(arr) < DB1_EVAL_CAND_MAX_SESSIONS)
         cJSON_AddItemToArray(arr, cJSON_CreateString(session_id));
   }

   char *rendered = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return rendered;
}

int db1_eval_candidate_get_by_signature(const char *signature, db1_eval_candidate_t *out)
{
   if (!signature || !signature[0] || !out)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT " EVAL_CAND_COLUMNS " FROM eval_candidates WHERE signature = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, signature, -1, SQLITE_TRANSIENT);

   int found = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      eval_cand_load_row(stmt, out);
      found = 1;
   }
   sqlite3_finalize(stmt);
   return found;
}

int db1_eval_candidate_observe(const char *signature, const char *suite, const char *task_name,
                               const char *task_json, const char *origin, const char *origin_ref,
                               const char *session_id)
{
   if (!signature || !signature[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   db1_eval_candidate_t existing;
   int found = db1_eval_candidate_get_by_signature(signature, &existing);
   if (found < 0)
      return -1;

   if (!found)
   {
      char *sessions = eval_cand_merge_session("[]", session_id, NULL);
      if (!sessions)
         return -1;
      sqlite3_stmt *stmt = NULL;
      static const char *sql =
          "INSERT INTO eval_candidates (signature, state, suite, task_name, task_json, origin,"
          " origin_ref, occurrences, sessions_json)"
          " VALUES (?, 'candidate', ?, ?, ?, ?, ?, 1, ?)";
      if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      {
         free(sessions);
         return -1;
      }
      sqlite3_bind_text(stmt, 1, signature, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, (suite && suite[0]) ? suite : "regressions", -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, task_name ? task_name : "", -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, task_json ? task_json : "{}", -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 5, origin ? origin : "", -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 6, origin_ref ? origin_ref : "", -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 7, sessions, -1, SQLITE_TRANSIENT);
      int rc = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      free(sessions);
      if (rc != SQLITE_DONE)
         return -1;
   }
   else
   {
      /* Re-observation: bump the counter and widen the session set. The state
       * is deliberately untouched, so a rejected signature stays rejected. */
      sqlite3_stmt *sel = NULL;
      static const char *sel_sql = "SELECT sessions_json FROM eval_candidates WHERE signature = ?";
      if (sqlite3_prepare_v2(db, sel_sql, -1, &sel, NULL) != SQLITE_OK)
         return -1;
      sqlite3_bind_text(sel, 1, signature, -1, SQLITE_TRANSIENT);
      char current[DB1_EVAL_CAND_SESSIONS_LEN] = "[]";
      if (sqlite3_step(sel) == SQLITE_ROW)
         eval_cand_copy(current, sizeof(current), sqlite3_column_text(sel, 0));
      sqlite3_finalize(sel);

      int was_new = 1;
      char *sessions = eval_cand_merge_session(current, session_id, &was_new);
      if (!sessions)
         return -1;
      sqlite3_stmt *stmt = NULL;
      /* A session already on the row is the same observation reported twice
       * (a re-run scan), not a second failure. */
      static const char *sql_new =
          "UPDATE eval_candidates SET occurrences = occurrences + 1, sessions_json = ?,"
          " updated_at = datetime('now') WHERE signature = ?";
      static const char *sql_repeat =
          "UPDATE eval_candidates SET sessions_json = ? WHERE signature = ?";
      if (sqlite3_prepare_v2(db, was_new ? sql_new : sql_repeat, -1, &stmt, NULL) != SQLITE_OK)
      {
         free(sessions);
         return -1;
      }
      sqlite3_bind_text(stmt, 1, sessions, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, signature, -1, SQLITE_TRANSIENT);
      int rc = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      free(sessions);
      if (rc != SQLITE_DONE)
         return -1;
   }

   return 0;
}

int db1_eval_candidate_list(const char *state_or_null, db1_eval_candidate_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   const char *state = (state_or_null && state_or_null[0]) ? state_or_null : "";
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT " EVAL_CAND_COLUMNS
                            " FROM eval_candidates WHERE (? = '' OR state = ?)"
                            " ORDER BY updated_at DESC, id DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, state, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, state, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      eval_cand_load_row(stmt, &out[n]);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

/* Shared state transition. `require_state` is the state the row must currently
 * be in ("" = any state); 'rejected' is terminal and never transitions out. */
static int eval_cand_transition(int64_t id, const char *require_state, const char *next_state,
                                const char *admitted_by, const char *path, const char *reason)
{
   if (id <= 0 || !next_state || !next_state[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE eval_candidates SET state = ?,"
       " admitted_by = CASE WHEN ? = '' THEN admitted_by ELSE ? END,"
       " admitted_path = CASE WHEN ? = '' THEN admitted_path ELSE ? END,"
       " reject_reason = CASE WHEN ? = '' THEN reject_reason ELSE ? END,"
       " updated_at = datetime('now')"
       " WHERE id = ? AND state <> 'rejected' AND (? = '' OR state = ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   const char *by = admitted_by ? admitted_by : "";
   const char *p = path ? path : "";
   const char *r = reason ? reason : "";
   const char *req = require_state ? require_state : "";
   sqlite3_bind_text(stmt, 1, next_state, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, by, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, by, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, p, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, p, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, r, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 7, r, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 8, id);
   sqlite3_bind_text(stmt, 9, req, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 10, req, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE)
      return -1;
   return sqlite3_changes(db) > 0 ? 0 : -1;
}

int db1_eval_candidate_mark_admitted(int64_t id, const char *admitted_by, const char *path)
{
   return eval_cand_transition(id, "candidate", "admitted", admitted_by, path, NULL);
}

int db1_eval_candidate_mark_rejected(int64_t id, const char *reason)
{
   return eval_cand_transition(id, "", "rejected", NULL, NULL,
                               (reason && reason[0]) ? reason : "operator");
}

int db1_eval_candidate_mark_archived(int64_t id)
{
   return eval_cand_transition(id, "admitted", "archived", NULL, NULL, NULL);
}

int db1_eval_candidate_set_passing_windows(int64_t id, int windows)
{
   if (id <= 0 || windows < 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "UPDATE eval_candidates SET passing_windows = ?,"
                            " updated_at = datetime('now') WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, windows);
   sqlite3_bind_int64(stmt, 2, id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}
