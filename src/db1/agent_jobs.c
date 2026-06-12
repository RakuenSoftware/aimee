/* db1/agent_jobs.c: durable per-machine agent-job queue. */

#include "agent_jobs.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Final-response turns are model waits, not external tool calls. Keep them at
 * least long enough for slow local/cloud completions even when an operator sets
 * an aggressive general idle threshold. */
#define FINAL_RESPONSE_MIN_STALE_THRESHOLD_SECS 300
#define REVIEW_IN_TOOL_STALE_THRESHOLD_SECS     240

static int agent_job_role_uses_short_tool_threshold(const char *role)
{
   return role && strcmp(role, "review") == 0;
}

static int agent_job_stale_threshold_secs(const char *role, const char *current_tool,
                                          int idle_threshold_secs, int in_tool_threshold_secs)
{
   if (!current_tool || !current_tool[0])
      return idle_threshold_secs;
   if (strcmp(current_tool, "final_response") == 0)
   {
      if (idle_threshold_secs > FINAL_RESPONSE_MIN_STALE_THRESHOLD_SECS)
         return idle_threshold_secs;
      return FINAL_RESPONSE_MIN_STALE_THRESHOLD_SECS;
   }
   if (strcmp(current_tool, "model") == 0)
      return idle_threshold_secs;
   if (agent_job_role_uses_short_tool_threshold(role) &&
       in_tool_threshold_secs > REVIEW_IN_TOOL_STALE_THRESHOLD_SECS)
      return REVIEW_IN_TOOL_STALE_THRESHOLD_SECS;
   return in_tool_threshold_secs;
}

int db1_agent_job_backfill_agent_names_from_log(void)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   static const char *sql =
       "UPDATE agent_jobs"
       " SET agent_name = ("
       "   SELECT candidate.agent_name FROM ("
       "      SELECT al.agent_name, al.id,"
       "             abs(strftime('%s', al.created_at) -"
       "                 strftime('%s', CASE WHEN agent_jobs.updated_at <> ''"
       "                                    THEN agent_jobs.updated_at ELSE agent_jobs.created_at"
       "                               END)) AS distance"
       "      FROM agent_log al"
       "      WHERE al.agent_name <> ''"
       "        AND al.role = agent_jobs.role"
       "   ) candidate"
       "   WHERE candidate.distance <= 900"
       "   ORDER BY candidate.distance, candidate.id DESC LIMIT 1"
       " )"
       " WHERE agent_name = ''"
       "   AND status IN ('done', 'failed', 'partial', 'cancelled')"
       "   AND EXISTS ("
       "      SELECT 1 FROM agent_log al"
       "      WHERE al.agent_name <> ''"
       "        AND al.role = agent_jobs.role"
       "        AND abs(strftime('%s', al.created_at) -"
       "                strftime('%s', CASE WHEN agent_jobs.updated_at <> ''"
       "                                   THEN agent_jobs.updated_at ELSE agent_jobs.created_at"
       "                              END)) <= 900"
       "   )";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(stmt);
   return changed;
}

int db1_agent_job_create(const char *role, const char *prompt, const char *agent_name,
                         const char *lease_owner)
{
   (void)lease_owner;
   if (!role || !prompt)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "INSERT INTO agent_jobs (role, prompt, agent_name, status,"
                            " heartbeat_at, created_at, updated_at)"
                            " VALUES (?, ?, ?, 'pending', '', datetime('now'), datetime('now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, role, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, prompt, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, agent_name ? agent_name : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   int id = (rc == SQLITE_DONE) ? (int)sqlite3_last_insert_rowid(db) : -1;
   sqlite3_finalize(stmt);
   return id;
}

void db1_agent_job_update(int job_id, const char *status, int cursor_turn, const char *result)
{
   if (job_id <= 0 || !status)
      return;
   sqlite3 *db = db1_conn();
   if (!db)
      return;

   sqlite3_stmt *stmt = NULL;
   /* Don't let progress updates overwrite a 'cancelled' status — cmd_cancel
    * sets it out-of-band and the running loop would otherwise stomp it back
    * to 'running' on its next per-turn update. Allow final success to win a
    * cancel/complete race so a delegate that finished while a cancellation was
    * being issued still records its result instead of leaving contradictory
    * job status and agent-log rows. */
   static const char *sql =
       "UPDATE agent_jobs SET status = ?, cursor = ?, result = ?, updated_at = datetime('now')"
       " WHERE id = ? AND (status != 'cancelled' OR ? = 'done')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return;

   char cursor[32];
   snprintf(cursor, sizeof(cursor), "%d", cursor_turn);

   /* Stored-result ceiling: prompt/result are now unbounded heap, so bound the
    * stored result to keep a single runaway delegate from persisting an
    * arbitrarily large blob (a poll/list memory-pressure surface). Over the
    * ceiling we store a truncated copy with an explicit marker rather than
    * silently dropping the tail. */
   const char *result_text = result ? result : "";
   char *capped = NULL;
   size_t rlen = strlen(result_text);
   if (rlen > DB1_AJ_RESULT_STORE_MAX)
   {
      static const char marker[] = "\n\n[truncated: result exceeded storage ceiling]";
      size_t keep = DB1_AJ_RESULT_STORE_MAX - (sizeof(marker) - 1);
      capped = malloc(keep + sizeof(marker));
      if (capped)
      {
         memcpy(capped, result_text, keep);
         memcpy(capped + keep, marker, sizeof(marker)); /* includes NUL */
         result_text = capped;
      }
   }

   sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, cursor, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, result_text, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 4, job_id);
   sqlite3_bind_text(stmt, 5, status, -1, SQLITE_TRANSIENT);
   (void)sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   free(capped);
}

void db1_agent_job_set_agent(int job_id, const char *agent_name)
{
   if (job_id <= 0 || !agent_name || !agent_name[0])
      return;
   sqlite3 *db = db1_conn();
   if (!db)
      return;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "UPDATE agent_jobs SET agent_name = ?, updated_at = datetime('now')"
                            " WHERE id = ? AND status != 'cancelled'";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return;

   sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, job_id);
   (void)sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

void db1_agent_job_heartbeat(int job_id)
{
   if (job_id <= 0)
      return;
   sqlite3 *db = db1_conn();
   if (!db)
      return;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "UPDATE agent_jobs SET heartbeat_at = datetime('now'),"
                            " updated_at = datetime('now') WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return;
   sqlite3_bind_int(stmt, 1, job_id);
   (void)sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

void db1_agent_job_heartbeat_ext(int job_id, const char *current_tool, int api_call_count)
{
   if (job_id <= 0)
      return;
   sqlite3 *db = db1_conn();
   if (!db)
      return;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "UPDATE agent_jobs SET heartbeat_at = datetime('now'),"
                            " current_tool = ?, api_call_count = ?,"
                            " updated_at = datetime('now') WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return;
   sqlite3_bind_text(stmt, 1, current_tool ? current_tool : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, api_call_count);
   sqlite3_bind_int(stmt, 3, job_id);
   (void)sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

int db1_agent_job_is_cancelled(int job_id)
{
   if (job_id <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT status FROM agent_jobs WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, job_id);
   int cancelled = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *s = sqlite3_column_text(stmt, 0);
      if (s && strcmp((const char *)s, "cancelled") == 0)
         cancelled = 1;
   }
   sqlite3_finalize(stmt);
   return cancelled;
}

int db1_agent_job_classify_stale(int job_id, int idle_threshold_secs, int in_tool_threshold_secs,
                                 char *out_state, size_t out_state_cap)
{
   if (out_state && out_state_cap > 0)
      out_state[0] = '\0';
   if (job_id <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT role, current_tool,"
                            " (julianday('now') - julianday(heartbeat_at)) * 86400 AS age_secs"
                            " FROM agent_jobs WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, job_id);

   int stale = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *role = sqlite3_column_text(stmt, 0);
      const char *role_text = role ? (const char *)role : "";
      const unsigned char *tool = sqlite3_column_text(stmt, 1);
      const char *tool_text = tool ? (const char *)tool : "";
      double age_secs = sqlite3_column_double(stmt, 2);
      int threshold_secs = agent_job_stale_threshold_secs(role_text, tool_text, idle_threshold_secs,
                                                          in_tool_threshold_secs);
      int is_stale = age_secs > (double)threshold_secs;
      const char *label;
      if (!is_stale)
         label = "fresh";
      else if (strcmp(tool_text, "final_response") == 0)
         label = "final_response";
      else if (strcmp(tool_text, "model") == 0)
         label = "model";
      else if (tool_text[0])
         label = "in_tool";
      else
         label = "idle";
      if (out_state && out_state_cap > 0)
         snprintf(out_state, out_state_cap, "%s", label);
      stale = is_stale ? 1 : 0;
   }
   sqlite3_finalize(stmt);
   return stale;
}

int db1_agent_job_get(int job_id, db1_agent_job_t *out)
{
   if (!out || job_id <= 0)
      return -1;
   memset(out, 0, sizeof(*out));
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   (void)db1_agent_job_backfill_agent_names_from_log();

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT id, role, prompt, agent_name, status, result, COALESCE(cursor, ''),"
       " COALESCE(lease_owner, ''), COALESCE(heartbeat_at, ''),"
       " COALESCE(current_tool, ''), COALESCE(api_call_count, 0),"
       " created_at, updated_at"
       " FROM agent_jobs WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, job_id);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      out->id = sqlite3_column_int(stmt, 0);
      db1_copy_col_text(out->role, sizeof(out->role), stmt, 1);
      out->prompt = db1_dup_col_text(stmt, 2);
      db1_copy_col_text(out->agent_name, sizeof(out->agent_name), stmt, 3);
      db1_copy_col_text(out->status, sizeof(out->status), stmt, 4);
      out->result = db1_dup_col_text(stmt, 5);
      const unsigned char *cursor_txt = sqlite3_column_text(stmt, 6);
      out->cursor_turn = cursor_txt ? atoi((const char *)cursor_txt) : 0;
      db1_copy_col_text(out->lease_owner, sizeof(out->lease_owner), stmt, 7);
      db1_copy_col_text(out->heartbeat_at, sizeof(out->heartbeat_at), stmt, 8);
      db1_copy_col_text(out->current_tool, sizeof(out->current_tool), stmt, 9);
      out->api_call_count = sqlite3_column_int(stmt, 10);
      db1_copy_col_text(out->created_at, sizeof(out->created_at), stmt, 11);
      db1_copy_col_text(out->updated_at, sizeof(out->updated_at), stmt, 12);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_agent_job_heartbeat_is_stale(const char *heartbeat_at, int stale_minutes)
{
   if (!heartbeat_at || !heartbeat_at[0])
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   char sql[256];
   snprintf(sql, sizeof(sql),
            "SELECT CASE WHEN datetime(?, '+%d minutes') < datetime('now') THEN 1 ELSE 0 END",
            stale_minutes);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(stmt, 1, heartbeat_at, -1, SQLITE_TRANSIENT);
   int stale = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      stale = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return stale;
}

int db1_agent_job_take_lease(int job_id, const char *owner)
{
   if (job_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "UPDATE agent_jobs SET status = 'running', lease_owner = ?,"
                            " heartbeat_at = datetime('now'), updated_at = datetime('now')"
                            " WHERE id = ? AND status = 'pending'";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, owner ? owner : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, job_id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
}

void db1_agent_job_free(db1_agent_job_t *job)
{
   if (!job)
      return;
   free(job->prompt);
   free(job->result);
   job->prompt = NULL;
   job->result = NULL;
}

int db1_agent_job_list_recent(db1_agent_job_t *out, int max, int include_heavy)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   (void)db1_agent_job_backfill_agent_names_from_log();

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT id, role, prompt, agent_name, status, result, COALESCE(cursor, ''),"
       " COALESCE(lease_owner, ''), COALESCE(heartbeat_at, ''),"
       " COALESCE(current_tool, ''), COALESCE(api_call_count, 0),"
       " created_at, updated_at"
       " FROM agent_jobs ORDER BY id DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_agent_job_t *o = &out[n];
      memset(o, 0, sizeof(*o));
      o->id = sqlite3_column_int(stmt, 0);
      db1_copy_col_text(o->role, sizeof(o->role), stmt, 1);
      o->prompt = include_heavy ? db1_dup_col_text(stmt, 2) : strdup("");
      db1_copy_col_text(o->agent_name, sizeof(o->agent_name), stmt, 3);
      db1_copy_col_text(o->status, sizeof(o->status), stmt, 4);
      o->result = include_heavy ? db1_dup_col_text(stmt, 5) : strdup("");
      const unsigned char *cursor_txt = sqlite3_column_text(stmt, 6);
      o->cursor_turn = cursor_txt ? atoi((const char *)cursor_txt) : 0;
      db1_copy_col_text(o->lease_owner, sizeof(o->lease_owner), stmt, 7);
      db1_copy_col_text(o->heartbeat_at, sizeof(o->heartbeat_at), stmt, 8);
      db1_copy_col_text(o->current_tool, sizeof(o->current_tool), stmt, 9);
      o->api_call_count = sqlite3_column_int(stmt, 10);
      db1_copy_col_text(o->created_at, sizeof(o->created_at), stmt, 11);
      db1_copy_col_text(o->updated_at, sizeof(o->updated_at), stmt, 12);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_agent_job_list_running_ids(int *out_ids, int max)
{
   if (!out_ids || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT id FROM agent_jobs WHERE status = 'running' LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
      out_ids[n++] = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

int db1_agent_job_cancel_by_id(int job_id, const char *reason)
{
   if (job_id <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE agent_jobs SET status = 'cancelled', cancelled_at = datetime('now'),"
       " cancel_reason = ?, result = CASE WHEN result = '' THEN ? ELSE result END"
       " WHERE id = ? AND status IN ('pending', 'running')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   char result[512];
   snprintf(result, sizeof(result), "cancelled: %s", reason && reason[0] ? reason : "cancelled");
   sqlite3_bind_text(stmt, 1, reason ? reason : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, result, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, job_id);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed;
}

int db1_agent_job_cancel_stale(int threshold_seconds, const char *reason)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   char sql[384];
   snprintf(sql, sizeof(sql),
            "UPDATE agent_jobs SET status = 'cancelled', cancelled_at = datetime('now'),"
            " cancel_reason = ?, result = CASE WHEN result = '' THEN ? ELSE result END"
            " WHERE status = 'running'"
            "   AND created_at < datetime('now', '-%d seconds')",
            threshold_seconds);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   const char *cancel_reason = reason ? reason : "orphan cleanup";
   char result[512];
   snprintf(result, sizeof(result), "cancelled: %s", cancel_reason);
   sqlite3_bind_text(stmt, 1, cancel_reason, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, result, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed;
}

int db1_agent_log_list(const char *agent_filter, db1_agent_log_entry_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   const char *sql_all = "SELECT agent_name, role, turns, tool_calls, success, confidence,"
                         " prompt_tokens, completion_tokens, latency_ms, created_at"
                         " FROM agent_log ORDER BY id DESC LIMIT ?";
   const char *sql_filtered = "SELECT agent_name, role, turns, tool_calls, success, confidence,"
                              " prompt_tokens, completion_tokens, latency_ms, created_at"
                              " FROM agent_log WHERE agent_name=? ORDER BY id DESC LIMIT ?";
   const char *sql = (agent_filter && agent_filter[0]) ? sql_filtered : sql_all;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   if (agent_filter && agent_filter[0])
   {
      sqlite3_bind_text(stmt, 1, agent_filter, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 2, max);
   }
   else
   {
      sqlite3_bind_int(stmt, 1, max);
   }
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_agent_log_entry_t *o = &out[n];
      memset(o, 0, sizeof(*o));
      db1_copy_col_text(o->agent_name, sizeof(o->agent_name), stmt, 0);
      db1_copy_col_text(o->role, sizeof(o->role), stmt, 1);
      o->turns = sqlite3_column_int(stmt, 2);
      o->tool_calls = sqlite3_column_int(stmt, 3);
      o->success = sqlite3_column_int(stmt, 4);
      o->confidence = sqlite3_column_int(stmt, 5);
      o->prompt_tokens = sqlite3_column_int(stmt, 6);
      o->completion_tokens = sqlite3_column_int(stmt, 7);
      o->latency_ms = sqlite3_column_int(stmt, 8);
      db1_copy_col_text(o->created_at, sizeof(o->created_at), stmt, 9);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}
