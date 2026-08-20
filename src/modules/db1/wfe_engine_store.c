/* wfe_engine_store.c: see wfe_engine_store.h. The engine's own queries, on the
 * side of the boundary that owns the rows.
 *
 * Each function here is a transcription of a statement the Go engine ran
 * against this file directly. They are transcriptions on purpose: the point of
 * the move is that the same rows are produced by the same predicates, and any
 * improvement to a query belongs in a change that is about the query rather
 * than in the one that relocates it.
 */
#include "wfe_engine_store.h"

#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* One integer answer from a statement the caller has already bound. Every count
 * below has the same shape, and the shape is where the mistakes are: a
 * forgotten finalize leaks the statement, and treating SQLITE_DONE as failure
 * turns "no rows matched" into "the store is broken". */
static int count_step(sqlite3_stmt *st)
{
   int rc = sqlite3_step(st);
   int value = (rc == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
   sqlite3_finalize(st);
   if (rc != SQLITE_ROW && rc != SQLITE_DONE)
      return -1;
   return value;
}

int db1_wfe_children_list(const char *parent_id, char (*out_ids)[DB1_WFE_ID_LEN], int max)
{
   if (!out_ids || max <= 0 || !parent_id)
      return -1;
   if (max > DB1_WFE_ID_LIST_MAX)
      max = DB1_WFE_ID_LIST_MAX;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   static const char *sql =
       "SELECT work_item_id FROM lifecycle_work_item WHERE parent_id=? ORDER BY id";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, parent_id, -1, SQLITE_TRANSIENT);
   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW)
   {
      const unsigned char *id = sqlite3_column_text(st, 0);
      snprintf(out_ids[n], DB1_WFE_ID_LEN, "%s", id ? (const char *)id : "");
      n++;
   }
   sqlite3_finalize(st);
   return n;
}

int db1_wfe_active_root_count(void)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   static const char *sql =
       "SELECT COUNT(*) FROM lifecycle_work_item WHERE parent_id='' AND state='active'";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   return count_step(st);
}

int db1_wfe_work_item_id_by_git_proposal(const char *repo, const char *proposal_path,
                                         const char *suffix, char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   if (!repo || !proposal_path || !suffix || !out || n == 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   /* Roots only: a slice child carries its parent's proposal and would answer
    * for a lookup that means the run as a whole. */
   static const char *sql = "SELECT work_item_id FROM lifecycle_work_item "
                            "WHERE repo = ? AND parent_id = '' AND "
                            "(proposal_path = ? OR (substr(proposal_path, 1, 4) = 'git:' AND "
                            "substr(proposal_path, -length(?)) = ?)) "
                            "ORDER BY id LIMIT 1";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, repo, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, proposal_path, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, suffix, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, suffix, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int found = 0;
   if (rc == SQLITE_ROW)
   {
      const unsigned char *id = sqlite3_column_text(st, 0);
      snprintf(out, n, "%s", id ? (const char *)id : "");
      found = 1;
   }
   sqlite3_finalize(st);
   if (rc != SQLITE_ROW && rc != SQLITE_DONE)
      return -1;
   return found;
}

int db1_wfe_executed_turn_count(const char *work_item_id)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   static const char *sql = "SELECT COUNT(*) FROM lifecycle_event "
                            "WHERE work_item_id=? AND kind IN ('advance','loop')";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   return count_step(st);
}

int db1_wfe_stage_loop_count(const char *work_item_id, const char *stage)
{
   if (!work_item_id || !work_item_id[0] || !stage)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   static const char *sql = "SELECT COUNT(*) FROM lifecycle_event "
                            "WHERE work_item_id=? AND stage=? AND kind IN ('loop','advance')";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage, -1, SQLITE_TRANSIENT);
   return count_step(st);
}

/* The two "since progress" counts differ only in which side of the capacity
 * predicate they take, so they share a body. Splitting them at the call site
 * rather than in the SQL would mean two nearly-identical statements to keep in
 * step, and they are the kind that drift. */
static int since_progress(const char *work_item_id, const char *stage, int capacity)
{
   if (!work_item_id || !work_item_id[0] || !stage)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   static const char *sql_other =
       "SELECT COUNT(*) FROM lifecycle_event "
       "WHERE work_item_id=? AND stage=? AND kind='pause' "
       "AND detail NOT LIKE 'capacity_backpressure:%' "
       "AND id > COALESCE((SELECT MAX(id) FROM lifecycle_event "
       "WHERE work_item_id=? AND kind IN ('advance','loop','create')), 0)";
   static const char *sql_capacity =
       "SELECT COUNT(*) FROM lifecycle_event "
       "WHERE work_item_id=? AND stage=? AND kind='pause' "
       "AND detail LIKE 'capacity_backpressure:%' "
       "AND id > COALESCE((SELECT MAX(id) FROM lifecycle_event "
       "WHERE work_item_id=? AND kind IN ('advance','loop','create')), 0)";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, capacity ? sql_capacity : sql_other, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, work_item_id, -1, SQLITE_TRANSIENT);
   return count_step(st);
}

int db1_wfe_runner_failures_since_progress(const char *work_item_id, const char *stage)
{
   return since_progress(work_item_id, stage, 0);
}

int db1_wfe_capacity_waits_since_progress(const char *work_item_id, const char *stage)
{
   return since_progress(work_item_id, stage, 1);
}

/* An UPDATE over many rows, answering with how many moved. sqlite3_changes is
 * the count for the last statement on this connection, so it is read
 * immediately after the step and never after a finalize that could run
 * something else. */
static long long run_update(sqlite3_stmt *st)
{
   sqlite3 *db = sqlite3_db_handle(st);
   int rc = sqlite3_step(st);
   long long changed = (rc == SQLITE_DONE) ? (long long)sqlite3_changes(db) : -1;
   sqlite3_finalize(st);
   return changed;
}

int db1_wfe_descendant_ids(const char *work_item_id, char (*out_ids)[DB1_WFE_ID_LEN], int max)
{
   if (!out_ids || max <= 0 || !work_item_id || !work_item_id[0])
      return -1;
   if (max > DB1_WFE_ID_LIST_MAX)
      max = DB1_WFE_ID_LIST_MAX;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   /* The seed is the item itself, so the answer is the subtree INCLUDING its
    * root. Callers stop or delete what this returns; excluding the root would
    * leave the thing they named running. */
   static const char *sql = "WITH RECURSIVE tree(id) AS ("
                            "SELECT ? UNION ALL "
                            "SELECT w.work_item_id FROM lifecycle_work_item w "
                            "JOIN tree t ON w.parent_id=t.id) "
                            "SELECT id FROM tree";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW)
   {
      const unsigned char *id = sqlite3_column_text(st, 0);
      snprintf(out_ids[n], DB1_WFE_ID_LEN, "%s", id ? (const char *)id : "");
      n++;
   }
   sqlite3_finalize(st);
   return n;
}

long long db1_wfe_resume_transient(const char *pause_reason, int older_than_secs)
{
   if (!pause_reason || !pause_reason[0] || older_than_secs < 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   static const char *sql = "UPDATE lifecycle_work_item "
                            "SET pause_reason='', paused_state='', updated_at=datetime('now') "
                            "WHERE state='active' AND pause_reason=? "
                            "AND updated_at <= datetime('now', ?)";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   char window[48];
   snprintf(window, sizeof window, "-%d seconds", older_than_secs);
   sqlite3_bind_text(st, 1, pause_reason, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, window, -1, SQLITE_TRANSIENT);
   return run_update(st);
}

long long db1_wfe_resume_wall_caps(int max_resumes)
{
   if (max_resumes < 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   /* override_count is bumped as it resumes: the run gets max_resumes chances
    * and the counter is what spends them. */
   static const char *sql = "UPDATE lifecycle_work_item "
                            "SET pause_reason='', paused_state='', "
                            "override_count=override_count+1, updated_at=datetime('now') "
                            "WHERE state='active' AND pause_reason='wall_cap' "
                            "AND override_count<?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(st, 1, max_resumes);
   return run_update(st);
}

long long db1_wfe_abandon_exhausted_wall_caps(int max_resumes, int grace_secs)
{
   if (max_resumes < 0 || grace_secs < 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   /* The grace period is measured from the last time the row moved, so a run
    * that was resumed a moment ago is not abandoned in the same sweep that
    * resumed it. */
   static const char *sql = "UPDATE lifecycle_work_item "
                            "SET state='abandoned', pause_reason='', paused_state='', "
                            "updated_at=datetime('now') "
                            "WHERE state='active' AND pause_reason='wall_cap' "
                            "AND override_count>=? AND updated_at < datetime('now', ?)";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   char window[48];
   snprintf(window, sizeof window, "-%d seconds", grace_secs);
   sqlite3_bind_int(st, 1, max_resumes);
   sqlite3_bind_text(st, 2, window, -1, SQLITE_TRANSIENT);
   return run_update(st);
}

long long db1_wfe_resume_ready_parents(void)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   /* Both halves matter: EXISTS proves the parent actually fanned out, and NOT
    * EXISTS(active) proves every slice has finished. Dropping the first would
    * resume parents whose children were never created. */
   static const char *sql = "UPDATE lifecycle_work_item AS parent "
                            "SET pause_reason='', paused_state='', updated_at=datetime('now') "
                            "WHERE parent.state='active' AND parent.pause_reason='slices_running' "
                            "AND EXISTS (SELECT 1 FROM lifecycle_work_item child "
                            "WHERE child.parent_id=parent.work_item_id) "
                            "AND NOT EXISTS (SELECT 1 FROM lifecycle_work_item child "
                            "WHERE child.parent_id=parent.work_item_id AND child.state='active')";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   return run_update(st);
}

int db1_wfe_delegate_job_save(const char *execution_key, const char *work_item_id, long long job_id,
                              const char *participant_token)
{
   if (!execution_key || !execution_key[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   /* Re-executing a step reuses its key and gets a new job, so the conflict
    * path resets cancel_attempts: the attempts recorded against the old job are
    * not attempts against this one. */
   static const char *sql =
       "INSERT INTO lifecycle_delegate_job"
       "(execution_key,job_id,work_item_id,participant_token) VALUES(?,?,?,?) "
       "ON CONFLICT(execution_key) DO UPDATE SET job_id=excluded.job_id,"
       "work_item_id=excluded.work_item_id,participant_token=excluded.participant_token,"
       "cancel_attempts=0,updated_at=datetime('now')";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, execution_key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 2, job_id);
   sqlite3_bind_text(st, 3, work_item_id ? work_item_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, participant_token ? participant_token : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_wfe_delegate_jobs_terminal_claim(db1_wfe_delegate_job_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > DB1_WFE_ID_LIST_MAX)
      max = DB1_WFE_ID_LIST_MAX;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   /* Read and claim in one transaction. The caller cancels what this returns;
    * if the read committed and the claim did not, the same jobs would come back
    * on the next sweep forever, and the cancel_attempts ordering that spreads
    * retries would never advance. */
   if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
      return -1;
   static const char *select_sql =
       "SELECT mapping.execution_key, mapping.job_id FROM lifecycle_delegate_job mapping "
       "JOIN lifecycle_work_item item ON item.work_item_id=mapping.work_item_id "
       "JOIN agent_jobs job ON job.id=mapping.job_id "
       "WHERE item.state IN ('accepted','rejected','stopped','abandoned') "
       "AND job.status IN ('pending','running') "
       "ORDER BY mapping.cancel_attempts, mapping.job_id LIMIT ?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, select_sql, -1, &st, NULL) != SQLITE_OK)
   {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return -1;
   }
   sqlite3_bind_int(st, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW)
   {
      const unsigned char *key = sqlite3_column_text(st, 0);
      snprintf(out[n].execution_key, sizeof out[n].execution_key, "%s",
               key ? (const char *)key : "");
      out[n].job_id = sqlite3_column_int64(st, 1);
      n++;
   }
   sqlite3_finalize(st);

   static const char *claim_sql = "UPDATE lifecycle_delegate_job "
                                  "SET cancel_attempts=cancel_attempts+1, "
                                  "updated_at=datetime('now') "
                                  "WHERE execution_key=? AND job_id=?";
   for (int i = 0; i < n; i++)
   {
      sqlite3_stmt *up = NULL;
      if (sqlite3_prepare_v2(db, claim_sql, -1, &up, NULL) != SQLITE_OK)
      {
         sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
         return -1;
      }
      sqlite3_bind_text(up, 1, out[i].execution_key, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(up, 2, out[i].job_id);
      int rc = sqlite3_step(up);
      sqlite3_finalize(up);
      if (rc != SQLITE_DONE)
      {
         sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
         return -1;
      }
   }
   if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
   {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return -1;
   }
   return n;
}
