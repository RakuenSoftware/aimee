/* db1/wfe_store.c: workflow-engine work-item state + audit log accessors. */
#include "wfe_store.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

int db1_work_item_create(const char *work_item_id, const char *repo, const char *proposal_path,
                         const char *workflow_name, const char *workflow_version,
                         const char *start_stage, const char *mode)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   static const char *sql =
       "INSERT INTO lifecycle_work_item (work_item_id, repo, proposal_path, "
       "workflow_name, workflow_version, current_stage, mode) VALUES (?,?,?,?,?,?,?)";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, repo ? repo : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, proposal_path ? proposal_path : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, (workflow_name && workflow_name[0]) ? workflow_name : "build", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 5, workflow_version ? workflow_version : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 6, start_stage ? start_stage : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 7, (mode && mode[0]) ? mode : "interactive", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

static void fill_wi(db1_work_item_t *o, sqlite3_stmt *st)
{
   memset(o, 0, sizeof *o);
   db1_copy_col_text(o->work_item_id, sizeof o->work_item_id, st, 0);
   db1_copy_col_text(o->repo, sizeof o->repo, st, 1);
   db1_copy_col_text(o->proposal_path, sizeof o->proposal_path, st, 2);
   db1_copy_col_text(o->workflow_name, sizeof o->workflow_name, st, 3);
   db1_copy_col_text(o->workflow_version, sizeof o->workflow_version, st, 4);
   db1_copy_col_text(o->current_stage, sizeof o->current_stage, st, 5);
   db1_copy_col_text(o->state, sizeof o->state, st, 6);
   db1_copy_col_text(o->mode, sizeof o->mode, st, 7);
   db1_copy_col_text(o->pause_reason, sizeof o->pause_reason, st, 8);
   db1_copy_col_text(o->paused_state, sizeof o->paused_state, st, 9);
   db1_copy_col_text(o->content_hash, sizeof o->content_hash, st, 10);
   o->cum_cost_usd = sqlite3_column_double(st, 11);
   o->work_item_max_cost_usd = sqlite3_column_double(st, 12);
   o->override_count = sqlite3_column_int(st, 13);
}

#define WI_COLS                                                                                    \
   "work_item_id, repo, proposal_path, workflow_name, workflow_version, current_stage, "           \
   "state, mode, pause_reason, paused_state, content_hash, cum_cost_usd, "                         \
   "work_item_max_cost_usd, override_count"

int db1_work_item_get(const char *work_item_id, db1_work_item_t *out)
{
   if (!work_item_id || !out)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   static const char *sql = "SELECT " WI_COLS " FROM lifecycle_work_item WHERE work_item_id = ?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int found = 0;
   if (rc == SQLITE_ROW)
   {
      fill_wi(out, st);
      found = 1;
   }
   sqlite3_finalize(st);
   return found;
}

static int exec_bind1_update(const char *sql, const char *wi)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, wi, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_work_item_set_stage(const char *wi, const char *stage, const char *content_hash)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi)
      return -1;
   static const char *sql = "UPDATE lifecycle_work_item SET current_stage=?, content_hash=?, "
                            "updated_at=datetime('now') WHERE work_item_id=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, stage ? stage : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, content_hash ? content_hash : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, wi, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_work_item_set_terminal(const char *wi, const char *state)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi)
      return -1;
   static const char *sql =
       "UPDATE lifecycle_work_item SET state=?, pause_reason='', paused_state='', "
       "updated_at=datetime('now') WHERE work_item_id=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, state ? state : "accepted", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, wi, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_work_item_set_pause(const char *wi, const char *reason, const char *paused_state)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi)
      return -1;
   static const char *sql = "UPDATE lifecycle_work_item SET pause_reason=?, paused_state=?, "
                            "updated_at=datetime('now') WHERE work_item_id=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, reason ? reason : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, paused_state ? paused_state : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, wi, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_work_item_clear_pause(const char *wi)
{
   return exec_bind1_update("UPDATE lifecycle_work_item SET pause_reason='', "
                            "paused_state='', updated_at=datetime('now') "
                            "WHERE work_item_id=?",
                            wi);
}

int db1_work_item_add_cost(const char *wi, double cost)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi)
      return -1;
   static const char *sql = "UPDATE lifecycle_work_item SET cum_cost_usd = cum_cost_usd + ?, "
                            "updated_at=datetime('now') WHERE work_item_id=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_double(st, 1, cost);
   sqlite3_bind_text(st, 2, wi, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_work_item_set_cost_cap(const char *wi, double cap)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi)
      return -1;
   static const char *sql = "UPDATE lifecycle_work_item SET work_item_max_cost_usd=? "
                            "WHERE work_item_id=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_double(st, 1, cap);
   sqlite3_bind_text(st, 2, wi, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_work_item_inc_override(const char *wi)
{
   if (exec_bind1_update("UPDATE lifecycle_work_item SET override_count=override_count+1 "
                         "WHERE work_item_id=?",
                         wi) != 0)
      return -1;
   db1_work_item_t w;
   if (db1_work_item_get(wi, &w) == 1)
      return w.override_count;
   return -1;
}

int db1_work_item_list(db1_work_item_t **out)
{
   if (out)
      *out = NULL;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   static const char *sql = "SELECT " WI_COLS " FROM lifecycle_work_item ORDER BY id DESC";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   int cap = 8, n = 0;
   db1_work_item_t *arr = malloc((size_t)cap * sizeof *arr);
   if (!arr)
   {
      sqlite3_finalize(st);
      return -1;
   }
   while (sqlite3_step(st) == SQLITE_ROW)
   {
      if (n == cap)
      {
         cap *= 2;
         db1_work_item_t *na = realloc(arr, (size_t)cap * sizeof *arr);
         if (!na)
            break;
         arr = na;
      }
      fill_wi(&arr[n++], st);
   }
   sqlite3_finalize(st);
   if (out)
      *out = arr;
   else
      free(arr);
   return n;
}

int db1_lifecycle_event_add(const char *wi, const char *stage, const char *kind, const char *actor,
                            const char *detail, const char *content_hash, double cost)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi)
      return -1;
   static const char *sql =
       "INSERT INTO lifecycle_event (work_item_id, stage, kind, actor, detail, "
       "content_hash, cost_usd) VALUES (?,?,?,?,?,?,?)";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, wi, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage ? stage : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, kind ? kind : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, actor ? actor : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 5, detail ? detail : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 6, content_hash ? content_hash : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(st, 7, cost);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_lifecycle_event_list(const char *wi, db1_lifecycle_event_t **out)
{
   if (out)
      *out = NULL;
   sqlite3 *db = db1_conn();
   if (!db || !wi)
      return -1;
   static const char *sql =
       "SELECT id, stage, kind, actor, detail, content_hash, cost_usd, created_at "
       "FROM lifecycle_event WHERE work_item_id=? ORDER BY id ASC";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, wi, -1, SQLITE_TRANSIENT);
   int cap = 8, n = 0;
   db1_lifecycle_event_t *arr = malloc((size_t)cap * sizeof *arr);
   if (!arr)
   {
      sqlite3_finalize(st);
      return -1;
   }
   while (sqlite3_step(st) == SQLITE_ROW)
   {
      if (n == cap)
      {
         cap *= 2;
         db1_lifecycle_event_t *na = realloc(arr, (size_t)cap * sizeof *arr);
         if (!na)
            break;
         arr = na;
      }
      db1_lifecycle_event_t *e = &arr[n++];
      memset(e, 0, sizeof *e);
      e->id = sqlite3_column_int64(st, 0);
      db1_copy_col_text(e->stage, sizeof e->stage, st, 1);
      db1_copy_col_text(e->kind, sizeof e->kind, st, 2);
      db1_copy_col_text(e->actor, sizeof e->actor, st, 3);
      db1_copy_col_text(e->detail, sizeof e->detail, st, 4);
      db1_copy_col_text(e->content_hash, sizeof e->content_hash, st, 5);
      e->cost_usd = sqlite3_column_double(st, 6);
      db1_copy_col_text(e->created_at, sizeof e->created_at, st, 7);
   }
   sqlite3_finalize(st);
   if (out)
      *out = arr;
   else
      free(arr);
   return n;
}

int db1_stage_attempt_inc(const char *wi, const char *stage)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi || !stage)
      return -1;
   static const char *sql =
       "INSERT INTO lifecycle_stage_attempt (work_item_id, stage, attempts) VALUES (?,?,1) "
       "ON CONFLICT(work_item_id, stage) DO UPDATE SET attempts = attempts + 1";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, wi, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
      return -1;
   return db1_stage_attempt_get(wi, stage);
}

int db1_stage_attempt_get(const char *wi, const char *stage)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi || !stage)
      return -1;
   static const char *sql =
       "SELECT attempts FROM lifecycle_stage_attempt WHERE work_item_id=? AND stage=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, wi, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage, -1, SQLITE_TRANSIENT);
   int v = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
   sqlite3_finalize(st);
   return v;
}

static int txn_exec(const char *cmd)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   return sqlite3_exec(db, cmd, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
int db1_lifecycle_txn_begin(void)
{
   return txn_exec("BEGIN IMMEDIATE");
}
int db1_lifecycle_txn_commit(void)
{
   return txn_exec("COMMIT");
}
int db1_lifecycle_txn_rollback(void)
{
   return txn_exec("ROLLBACK");
}
