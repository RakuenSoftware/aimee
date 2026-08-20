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
