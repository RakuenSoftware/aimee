/* db1/wfe_store.c: workflow-engine work-item state + audit log accessors. */
#include "wfe_store.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

/* General work-item insert (interactive + internal callers, e.g. the sweep path).
 * The PUBLIC autonomous intake (POST /v1/dev/submit) must NOT use this directly —
 * it goes through db1_work_item_submit_capped, which binds a submitter atomically
 * so every externally-submitted autonomous run is capped + attributed. (A DB CHECK
 * is intentionally avoided: engine tests legitimately create autonomous rows here
 * without a submitter.) */
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
   db1_copy_col_text(o->pr_ref, sizeof o->pr_ref, st, 14);
   db1_copy_col_text(o->worktree, sizeof o->worktree, st, 15);
   db1_copy_col_text(o->submitter, sizeof o->submitter, st, 16);
   db1_copy_col_text(o->parent_id, sizeof o->parent_id, st, 17);
}

/* pr_ref/worktree/submitter/parent_id are appended last so the existing column
 * indices (0-16) are unchanged. */
#define WI_COLS                                                                                    \
   "work_item_id, repo, proposal_path, workflow_name, workflow_version, current_stage, "           \
   "state, mode, pause_reason, paused_state, content_hash, cum_cost_usd, "                         \
   "work_item_max_cost_usd, override_count, pr_ref, worktree, submitter, parent_id"

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

int db1_work_item_id_by_proposal(const char *repo, const char *proposal_path, char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   if (!proposal_path || !out || !n)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   static const char *sql =
       "SELECT work_item_id FROM lifecycle_work_item WHERE repo = ? AND proposal_path = ?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, repo ? repo : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, proposal_path, -1, SQLITE_TRANSIENT);
   int found = 0;
   if (sqlite3_step(st) == SQLITE_ROW)
   {
      const char *id = (const char *)sqlite3_column_text(st, 0);
      snprintf(out, n, "%s", id ? id : "");
      found = 1;
   }
   sqlite3_finalize(st);
   return found;
}

int db1_work_item_id_by_pr_ref(const char *pr_ref, char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   if (!pr_ref || !pr_ref[0] || !out || !n)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   static const char *sql = "SELECT work_item_id FROM lifecycle_work_item WHERE pr_ref = ? LIMIT 1";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, pr_ref, -1, SQLITE_TRANSIENT);
   int found = 0;
   if (sqlite3_step(st) == SQLITE_ROW)
   {
      const char *id = (const char *)sqlite3_column_text(st, 0);
      snprintf(out, n, "%s", id ? id : "");
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

int db1_work_item_set_pr_ref(const char *wi, const char *pr_ref)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi)
      return -1;
   static const char *sql = "UPDATE lifecycle_work_item SET pr_ref=?, "
                            "updated_at=datetime('now') WHERE work_item_id=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, pr_ref ? pr_ref : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, wi, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_work_item_set_worktree(const char *wi, const char *worktree)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi)
      return -1;
   static const char *sql = "UPDATE lifecycle_work_item SET worktree=?, "
                            "updated_at=datetime('now') WHERE work_item_id=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, worktree ? worktree : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, wi, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_work_item_set_submitter(const char *wi, const char *submitter)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi)
      return -1;
   static const char *sql = "UPDATE lifecycle_work_item SET submitter=?, "
                            "updated_at=datetime('now') WHERE work_item_id=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, submitter ? submitter : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, wi, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   /* Fail closed: a step that "succeeds" but matched no row (unknown work item)
    * would silently lose the audit binding, so require exactly one changed row. */
   if (rc != SQLITE_DONE || sqlite3_changes(db) != 1)
      return -1;
   return 0;
}

int db1_work_item_set_parent(const char *wi, const char *parent_id)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi)
      return -1;
   static const char *sql = "UPDATE lifecycle_work_item SET parent_id=?, "
                            "updated_at=datetime('now') WHERE work_item_id=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, parent_id ? parent_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, wi, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   /* Fail closed: an update that matched no row (unknown work item) must not
    * silently report success — the child would be orphaned from its parent. */
   if (rc != SQLITE_DONE || sqlite3_changes(db) != 1)
      return -1;
   return 0;
}

int db1_work_item_child_counts(const char *parent_id, int *total, int *accepted, int *failed)
{
   if (total)
      *total = 0;
   if (accepted)
      *accepted = 0;
   if (failed)
      *failed = 0;
   sqlite3 *db = db1_conn();
   if (!db || !parent_id || !parent_id[0])
      return -1;
   /* One aggregate pass: total children + those terminal-accepted + those terminal
    * NOT-accepted ('rejected' or 'abandoned' -> a slice that will never merge). The
    * foreach gate advances only when total>0 AND accepted==total (every slice merged);
    * any failed child means a slice will not merge, so the parent parks for a human. */
   static const char *sql = "SELECT COUNT(*), "
                            "SUM(CASE WHEN state='accepted' THEN 1 ELSE 0 END), "
                            "SUM(CASE WHEN state IN ('rejected','abandoned') THEN 1 ELSE 0 END) "
                            "FROM lifecycle_work_item WHERE parent_id=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, parent_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   if (rc == SQLITE_ROW)
   {
      if (total)
         *total = sqlite3_column_int(st, 0);
      if (accepted)
         *accepted = sqlite3_column_int(st, 1);
      if (failed)
         *failed = sqlite3_column_int(st, 2);
   }
   sqlite3_finalize(st);
   return rc == SQLITE_ROW ? 0 : -1;
}

int db1_work_item_count_active_by_submitter(const char *submitter)
{
   sqlite3 *db = db1_conn();
   if (!db || !submitter)
      return -1;
   /* Count NON-TERMINAL autonomous runs (active or human-parked) — keyed off
    * terminal-ness, not the literal state='active' label, so a pause/resume
    * transition can't free a concurrency slot while the run still holds its
    * worktree + budget. Mirror the scheduler's terminal whitelist. */
   static const char *sql =
       "SELECT COUNT(*) FROM lifecycle_work_item WHERE submitter=? "
       "AND state NOT IN ('accepted','rejected','abandoned') AND mode='autonomous'";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, submitter, -1, SQLITE_TRANSIENT);
   int count = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
   sqlite3_finalize(st);
   return count;
}

int db1_work_item_count_recent_by_submitter(const char *submitter, int secs)
{
   sqlite3 *db = db1_conn();
   if (!db || !submitter || secs <= 0)
      return -1;
   /* mode='autonomous' kept symmetric with the active-count helper: both caps
    * mean the same "this autonomous principal", never an interactive row. */
   static const char *sql = "SELECT COUNT(*) FROM lifecycle_work_item WHERE submitter=? "
                            "AND mode='autonomous' AND created_at > datetime('now', ?)";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   char win[32];
   snprintf(win, sizeof win, "-%d seconds", secs);
   sqlite3_bind_text(st, 1, submitter, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, win, -1, SQLITE_TRANSIENT);
   int count = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
   sqlite3_finalize(st);
   return count;
}

int db1_work_item_submit_capped(const char *work_item_id, const char *repo,
                                const char *proposal_path, const char *workflow_name,
                                const char *workflow_version, const char *start_stage,
                                const char *submitter, int max_active, int rate_max, int rate_secs)
{
   if (!work_item_id || !work_item_id[0] || !submitter || !submitter[0])
      return -1;
   /* One BEGIN IMMEDIATE around cap-check + create + submitter-bind + audit so
    * concurrent submits from one principal serialize (the second blocks on the
    * write lock, then its COUNT sees the first's row) — the cap can't be exceeded
    * by the request-burst factor. Every step is fail-CLOSED: a -1 from a count
    * (DB fault) or any failed write rolls back, so a partial/uncapped/unattributed
    * run never escapes. Returns 0=created, 1=concurrency cap, 2=rate cap, -1=error. */
   if (db1_lifecycle_txn_begin() != 0)
      return -1;
   int active = db1_work_item_count_active_by_submitter(submitter);
   if (active < 0)
   {
      db1_lifecycle_txn_rollback();
      return -1;
   }
   if (max_active > 0 && active >= max_active)
   {
      db1_lifecycle_txn_rollback();
      return 1;
   }
   if (rate_max > 0 && rate_secs > 0)
   {
      int recent = db1_work_item_count_recent_by_submitter(submitter, rate_secs);
      if (recent < 0)
      {
         db1_lifecycle_txn_rollback();
         return -1;
      }
      if (recent >= rate_max)
      {
         db1_lifecycle_txn_rollback();
         return 2;
      }
   }
   const char *start = start_stage ? start_stage : "intake";
   if (db1_work_item_create(work_item_id, repo, proposal_path, workflow_name, workflow_version,
                            start, "autonomous") != 0 ||
       db1_work_item_set_submitter(work_item_id, submitter) != 0)
   {
      db1_lifecycle_txn_rollback();
      return -1;
   }
   /* Parity "create" event (matches the interactive wfe_work_item_create path) plus
    * the attributed, self-locating "submit" audit row (repo + proposal + id). */
   char detail[512];
   snprintf(detail, sizeof detail, "submit repo=%s proposal=%s id=%s", repo ? repo : "",
            proposal_path ? proposal_path : "", work_item_id);
   if (db1_lifecycle_event_add(work_item_id, start, "create", submitter,
                               workflow_name ? workflow_name : "", workflow_version, 0) != 0 ||
       db1_lifecycle_event_add(work_item_id, start, "submit", submitter, detail, "", 0) != 0)
   {
      db1_lifecycle_txn_rollback();
      return -1;
   }
   if (db1_lifecycle_txn_commit() != 0)
   {
      db1_lifecycle_txn_rollback();
      return -1;
   }
   return 0;
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

int db1_work_item_gate_apply(const char *wi, const char *expect_stage, const char *expect_hash,
                             const char *new_stage, const char *terminal_state)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi)
      return -1;
   int loopback = new_stage && new_stage[0];
   int terminal = !loopback && terminal_state && terminal_state[0];
/* The SET clause varies by decision; the WHERE guard is identical in all three
 * cases (the row must still be parked exactly as the caller observed), so it lives
 * in one place via compile-time literal concatenation. */
#define GATE_GUARD                                                                                 \
   " updated_at=datetime('now') WHERE work_item_id=?2 AND current_stage=?3 AND content_hash=?4 "   \
   "AND pause_reason='pending_human'"
   const char *sql;
   if (loopback)
      sql = "UPDATE lifecycle_work_item SET current_stage=?1, pause_reason='', "
            "paused_state=''," GATE_GUARD;
   else if (terminal)
      sql = "UPDATE lifecycle_work_item SET state=?1, pause_reason='', paused_state=''," GATE_GUARD;
   else /* approve: clear pause only, stage unchanged */
      sql = "UPDATE lifecycle_work_item SET pause_reason='', paused_state=''," GATE_GUARD;
#undef GATE_GUARD
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   /* ?1 is the variable SET value (stage for loopback, state for terminal, unused
    * for approve — bound to "" harmlessly since the approve SQL omits ?1). */
   sqlite3_bind_text(st, 1, loopback ? new_stage : (terminal ? terminal_state : ""), -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, wi, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, expect_stage ? expect_stage : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, expect_hash ? expect_hash : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
      return -1;
   return sqlite3_changes(db) == 1 ? 1 : 0; /* 0 rows => precondition not met (409) */
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

int db1_work_item_clear_pause_if(const char *wi, const char *expect_reason,
                                 const char *expect_stage)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi)
      return -1;
   /* Compare-and-clear: clear the pause ONLY while the row still shows exactly the
    * (pause_reason, current_stage) the caller observed. A single UPDATE, so two
    * drivers of the same parked item cannot both win — the loser's WHERE no longer
    * matches (pause already ''), guaranteeing at most one retry per park even if
    * the single-threaded-scheduler invariant is ever relaxed. */
   static const char *sql = "UPDATE lifecycle_work_item SET pause_reason='', paused_state='', "
                            "updated_at=datetime('now') "
                            "WHERE work_item_id=? AND pause_reason=? AND current_stage=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, wi, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, expect_reason ? expect_reason : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, expect_stage ? expect_stage : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
      return -1;
   return sqlite3_changes(db) > 0 ? 1 : 0; /* 1 = won the clear, 0 = row no longer matched */
}

int db1_work_item_delete(const char *wi)
{
   if (!wi || !wi[0])
      return -1;
   /* No FK cascade: the three lifecycle tables are independent, so remove the
    * item's rows from each under one transaction (all-or-nothing). Events and
    * stage-attempts are deleted first, then the item row, so a partial failure
    * never leaves an item row without its history or vice versa. */
   if (db1_lifecycle_txn_begin() != 0)
      return -1;
   if (exec_bind1_update("DELETE FROM lifecycle_event WHERE work_item_id=?", wi) != 0 ||
       exec_bind1_update("DELETE FROM lifecycle_stage_attempt WHERE work_item_id=?", wi) != 0 ||
       exec_bind1_update("DELETE FROM lifecycle_work_item WHERE work_item_id=?", wi) != 0)
   {
      db1_lifecycle_txn_rollback();
      return -1;
   }
   if (db1_lifecycle_txn_commit() != 0)
   {
      db1_lifecycle_txn_rollback();
      return -1;
   }
   return 0;
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

/* Reset a stage's loop-attempt budget (operator resume of an escalated
 * roundtable park re-arms the refinement loop). 0 on success. */
int db1_stage_attempt_reset(const char *wi, const char *stage)
{
   sqlite3 *db = db1_conn();
   if (!db || !wi || !stage)
      return -1;
   static const char *sql = "DELETE FROM lifecycle_stage_attempt WHERE work_item_id=? AND stage=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, wi, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
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
