/* wfe_transition_store.c: the engine's stage transitions, as whole transactions.
 *
 * Each function here is one transaction the Go engine ran against the module's
 * own file. They are single operations rather than compositions of the existing
 * ones for the reason wfe_engine.c's sixteen writes became
 * db1_work_item_record_outcome: a transaction cannot be assembled from separate
 * calls across a wire, and half of one of these applied is worse than none of
 * it. A move that recorded its event but did not clear the reservation would
 * leave the tree's budget permanently short; one that moved the stage without
 * recording the event would lose the run's history.
 *
 * They share a shape:
 *
 *   1. an optimistic UPDATE on the work item, guarded by the stage the caller
 *      believes it is in and by the run still being runnable. Matching zero rows
 *      means someone else moved it, and the whole call fails rather than
 *      writing an event for a transition that did not happen.
 *   2. the lifecycle event that records what happened.
 *   3. whatever attempt or freeze bookkeeping the transition implies.
 *
 * Every transition also releases the run's budget reservation -- reserved to 0,
 * state, owner and lease cleared -- because the invocation the reservation
 * authorized has finished. That release is part of the same transaction for the
 * same reason as everything else here.
 */
#include "wfe_engine_store.h"

#include "db1_internal.h"

#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* The columns every transition clears: the reservation is spent once the
 * invocation it authorized has ended. */
#define CLEAR_RESERVATION                                                                          \
   "reserved_cost_usd=0, reservation_state='', reservation_owner='', reservation_lease_until=''"

static void tx_rollback(sqlite3 *db)
{
   sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
}

static int tx_begin(sqlite3 *db)
{
   return sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static int tx_commit(sqlite3 *db)
{
   if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK)
      return 0;
   tx_rollback(db);
   return -1;
}

/* Insert one lifecycle event. Every transition records exactly one, and getting
 * the column order wrong in six places is the kind of mistake that only shows up
 * as a mislabelled history months later. */
static int add_event(sqlite3 *db, const char *work_item_id, const char *stage, const char *kind,
                     const char *actor, const char *detail, const char *content_hash, double cost)
{
   static const char *sql = "INSERT INTO lifecycle_event "
                            "(work_item_id, stage, kind, actor, detail, content_hash, cost_usd) "
                            "VALUES (?,?,?,?,?,?,?)";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
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

static int drop_stage_attempts(sqlite3 *db, const char *work_item_id, const char *stage)
{
   static const char *sql = "DELETE FROM lifecycle_stage_attempt WHERE work_item_id=? AND stage=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage ? stage : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

/* A cost that is not finite poisons every later comparison against a cap and
 * strands the whole tree, so it is refused at the boundary next to the write. */
static int cost_is_sane(double cost)
{
   return !(isnan(cost) || isinf(cost) || cost < 0);
}

int db1_wfe_move(const char *work_item_id, const char *from_stage, const char *to_stage,
                 const char *kind, const char *detail, const char *content_hash, double cost)
{
   if (!work_item_id || !work_item_id[0] || !from_stage || !from_stage[0] || !to_stage ||
       !to_stage[0] || !kind || !kind[0] || !cost_is_sane(cost))
      return -1;
   sqlite3 *db = db1_conn();
   if (!db || tx_begin(db) != 0)
      return -1;

   static const char *move_sql =
       "UPDATE lifecycle_work_item "
       "SET current_stage=?, content_hash=?, pause_reason='', paused_state='', "
       "cum_cost_usd=cum_cost_usd+?, " CLEAR_RESERVATION ", updated_at=datetime('now') "
       "WHERE work_item_id=? AND current_stage=? AND state='active' AND pause_reason=''";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, move_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, to_stage, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, content_hash ? content_hash : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(st, 3, cost);
   sqlite3_bind_text(st, 4, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 5, from_stage, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(st);
   if (changed != 1)
   {
      /* Someone else moved it, or it is paused or terminal. Recording the event
       * anyway would write history for a transition that did not happen. */
      tx_rollback(db);
      return -1;
   }

   if (add_event(db, work_item_id, from_stage, kind, "go-wfe", detail, content_hash, cost) != 0)
   {
      tx_rollback(db);
      return -1;
   }

   if (strcmp(kind, "loop") != 0)
   {
      /* Clear the stage being LEFT, not the one being entered. The stage
       * completed, so a later revisit starts with a fresh budget. Clearing the
       * one being entered resets the counter that bounds a refinement loop: a
       * gate that loops back to its author is re-entered by the author's own
       * advance, and that advance was wiping the gate's accumulated attempts, so
       * the cap could never be reached. Observed before the fix: a plan gate at
       * 63 loops against a cap of 20, five hours of spend, converging by luck. */
      if (drop_stage_attempts(db, work_item_id, from_stage) != 0)
      {
         tx_rollback(db);
         return -1;
      }
   }
   return tx_commit(db);
}

int db1_wfe_record_retry(const char *work_item_id, const char *stage, const char *to_stage,
                         const char *detail, int max_attempts, double cost)
{
   if (!work_item_id || !work_item_id[0] || !stage || !stage[0] || max_attempts < 1 ||
       !cost_is_sane(cost))
      return -1;
   sqlite3 *db = db1_conn();
   if (!db || tx_begin(db) != 0)
      return -1;

   static const char *bump_sql =
       "INSERT INTO lifecycle_stage_attempt (work_item_id,stage,attempts) VALUES (?,?,1) "
       "ON CONFLICT(work_item_id,stage) DO UPDATE SET attempts=attempts+1";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, bump_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
   {
      tx_rollback(db);
      return -1;
   }

   static const char *read_sql =
       "SELECT attempts FROM lifecycle_stage_attempt WHERE work_item_id=? AND stage=?";
   if (sqlite3_prepare_v2(db, read_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage, -1, SQLITE_TRANSIENT);
   int attempts = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
   sqlite3_finalize(st);
   if (attempts < 0)
   {
      tx_rollback(db);
      return -1;
   }

   /* Out of attempts: park in place rather than advancing, and say why. */
   int parked = attempts >= max_attempts;
   const char *reason = parked ? "retry_limit" : "";
   const char *paused_state = parked ? stage : "";
   const char *target = parked ? stage : (to_stage ? to_stage : stage);

   static const char *move_sql =
       "UPDATE lifecycle_work_item "
       "SET current_stage=?, pause_reason=?, paused_state=?, "
       "cum_cost_usd=cum_cost_usd+?, " CLEAR_RESERVATION ", updated_at=datetime('now') "
       "WHERE work_item_id=? AND current_stage=? AND state='active' AND pause_reason=''";
   if (sqlite3_prepare_v2(db, move_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, target, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, reason, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, paused_state, -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(st, 4, cost);
   sqlite3_bind_text(st, 5, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 6, stage, -1, SQLITE_TRANSIENT);
   rc = sqlite3_step(st);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(st);
   if (changed != 1)
   {
      tx_rollback(db);
      return -1;
   }

   if (add_event(db, work_item_id, stage, parked ? "pause" : "loop", "go-wfe", detail, "", cost) !=
       0)
   {
      tx_rollback(db);
      return -1;
   }
   if (tx_commit(db) != 0)
      return -1;
   return parked ? 1 : 0;
}

int db1_wfe_park_with_detail(const char *work_item_id, const char *stage, const char *reason,
                             const char *detail, double cost)
{
   if (!work_item_id || !work_item_id[0] || !stage || !cost_is_sane(cost))
      return -1;
   sqlite3 *db = db1_conn();
   if (!db || tx_begin(db) != 0)
      return -1;

   static const char *park_sql =
       "UPDATE lifecycle_work_item "
       "SET pause_reason=?, paused_state=?, cum_cost_usd=cum_cost_usd+?, " CLEAR_RESERVATION ", "
       "updated_at=datetime('now') "
       "WHERE work_item_id=? AND current_stage=? AND state='active' AND pause_reason=''";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, park_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, reason ? reason : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage, -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(st, 3, cost);
   sqlite3_bind_text(st, 4, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 5, stage, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(st);
   if (changed != 1)
   {
      tx_rollback(db);
      return -1;
   }
   if (add_event(db, work_item_id, stage, "pause", "go-wfe", detail, "", cost) != 0)
   {
      tx_rollback(db);
      return -1;
   }
   return tx_commit(db);
}

int db1_wfe_resume(const char *work_item_id)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db || tx_begin(db) != 0)
      return -1;

   /* The stage it is parked in and why, read inside the transaction: the event
    * this records names the reason being cleared, and reading it outside would
    * let it change under us. */
   static const char *read_sql = "SELECT current_stage, pause_reason FROM lifecycle_work_item "
                                 "WHERE work_item_id=? AND state='active'";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, read_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   char stage[64] = "";
   char reason[64] = "";
   if (sqlite3_step(st) != SQLITE_ROW)
   {
      sqlite3_finalize(st);
      tx_rollback(db);
      return -1;
   }
   const unsigned char *stage_text = sqlite3_column_text(st, 0);
   const unsigned char *reason_text = sqlite3_column_text(st, 1);
   snprintf(stage, sizeof stage, "%s", stage_text ? (const char *)stage_text : "");
   snprintf(reason, sizeof reason, "%s", reason_text ? (const char *)reason_text : "");
   sqlite3_finalize(st);

   static const char *clear_sql =
       "UPDATE lifecycle_work_item SET pause_reason='', paused_state='', "
       "updated_at=datetime('now') WHERE work_item_id=? AND state='active'";
   if (sqlite3_prepare_v2(db, clear_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(st);
   if (changed != 1)
   {
      tx_rollback(db);
      return -1;
   }

   /* A resumed stage starts over: the attempts that exhausted its retry budget
    * are what parked it, and leaving them would park it again immediately. */
   if (drop_stage_attempts(db, work_item_id, stage) != 0)
   {
      tx_rollback(db);
      return -1;
   }
   if (add_event(db, work_item_id, stage, "resume", "operator", reason, "", 0) != 0)
   {
      tx_rollback(db);
      return -1;
   }
   return tx_commit(db);
}

int db1_wfe_finish(const char *work_item_id, const char *stage, const char *state,
                   const char *detail, const char *content_hash, double cost)
{
   if (!work_item_id || !work_item_id[0] || !stage || !state || !state[0] || !cost_is_sane(cost))
      return -1;
   sqlite3 *db = db1_conn();
   if (!db || tx_begin(db) != 0)
      return -1;

   /* Note the guard: active and in the expected stage, but NOT "unpaused". A run
    * parked for a human is still finishable by the decision that unparks it. */
   static const char *finish_sql =
       "UPDATE lifecycle_work_item "
       "SET state=?, pause_reason='', paused_state='', content_hash=?, "
       "cum_cost_usd=cum_cost_usd+?, " CLEAR_RESERVATION ", updated_at=datetime('now') "
       "WHERE work_item_id=? AND current_stage=? AND state='active'";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, finish_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, state, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, content_hash ? content_hash : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(st, 3, cost);
   sqlite3_bind_text(st, 4, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 5, stage, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(st);
   if (changed != 1)
   {
      tx_rollback(db);
      return -1;
   }
   if (add_event(db, work_item_id, stage, "terminal", "go-wfe", detail, content_hash, cost) != 0)
   {
      tx_rollback(db);
      return -1;
   }

   /* A finished run's frozen create records are spent: they exist to make a
    * re-run reuse the children it already made, and there will be no re-run. */
   static const char *thaw_sql = "DELETE FROM wfe_frozen_create WHERE work_item_id=?";
   if (sqlite3_prepare_v2(db, thaw_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
   {
      tx_rollback(db);
      return -1;
   }
   return tx_commit(db);
}

/* A review gate that asked for changes, and the two ways a refinement loop can
 * fail to converge.
 *
 * Two independent bounds, because they catch different pathologies:
 *
 *   attempts >= max_iterations   the loop has run too many times, whatever it
 *                                produced. 'convergence_limit'.
 *   repeats  >= max_identical    the loop produced the SAME artifact against the
 *                                SAME feedback that many times running. Nothing
 *                                is changing, so more rounds will not help.
 *                                'convergence_no_progress'.
 *
 * The second is checked first: a run that is repeating itself should be reported
 * as repeating itself, not as merely having run out of rounds. Repeats reset the
 * moment either hash changes, because a different artifact or different feedback
 * is progress even if the round still failed.
 */
int db1_wfe_record_requested_changes(const char *work_item_id, const char *gate,
                                     const char *plan_stage, const char *plan_hash,
                                     const char *feedback_hash, const char *unresolved,
                                     int max_iterations, int max_identical, double cost,
                                     db1_wfe_review_outcome_t *out)
{
   if (!work_item_id || !work_item_id[0] || !gate || !gate[0] || !plan_stage || !plan_stage[0] ||
       !plan_hash || !plan_hash[0] || !feedback_hash || !feedback_hash[0] || max_iterations < 1 ||
       max_identical < 1 || !out || !cost_is_sane(cost))
      return -1;
   memset(out, 0, sizeof *out);
   sqlite3 *db = db1_conn();
   if (!db || tx_begin(db) != 0)
      return -1;

   /* Only an active run can be routed anywhere. */
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "SELECT state FROM lifecycle_work_item WHERE work_item_id=?", -1, &st,
                          NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   int have = sqlite3_step(st) == SQLITE_ROW;
   char state[24] = "";
   if (have)
   {
      const unsigned char *text = sqlite3_column_text(st, 0);
      snprintf(state, sizeof state, "%s", text ? (const char *)text : "");
   }
   sqlite3_finalize(st);
   if (!have || strcmp(state, "active") != 0)
   {
      tx_rollback(db);
      return -1;
   }

   static const char *bump_sql =
       "INSERT INTO lifecycle_stage_attempt (work_item_id, stage, attempts) VALUES (?,?,1) "
       "ON CONFLICT(work_item_id, stage) DO UPDATE SET attempts = attempts + 1";
   if (sqlite3_prepare_v2(db, bump_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, gate, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
   {
      tx_rollback(db);
      return -1;
   }
   if (sqlite3_prepare_v2(
           db, "SELECT attempts FROM lifecycle_stage_attempt WHERE work_item_id=? AND stage=?", -1,
           &st, NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, gate, -1, SQLITE_TRANSIENT);
   int attempts = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
   sqlite3_finalize(st);
   if (attempts < 0)
   {
      tx_rollback(db);
      return -1;
   }

   /* Has this gate seen exactly this artifact against exactly this feedback
    * before? Missing is not an error: the first round has nothing to compare. */
   int repeats = 1;
   static const char *seen_sql = "SELECT artifact_hash, feedback_hash, identical_repeats "
                                 "FROM wfe_convergence WHERE work_item_id=? AND gate=?";
   if (sqlite3_prepare_v2(db, seen_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, gate, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(st) == SQLITE_ROW)
   {
      const unsigned char *old_plan = sqlite3_column_text(st, 0);
      const unsigned char *old_feedback = sqlite3_column_text(st, 1);
      int old_repeats = sqlite3_column_int(st, 2);
      if (old_plan && old_feedback && strcmp((const char *)old_plan, plan_hash) == 0 &&
          strcmp((const char *)old_feedback, feedback_hash) == 0)
         repeats = old_repeats + 1;
   }
   sqlite3_finalize(st);

   static const char *observe_sql =
       "INSERT INTO wfe_convergence "
       "(work_item_id, gate, artifact_hash, feedback_hash, identical_repeats, updated_at) "
       "VALUES (?,?,?,?,?,datetime('now')) "
       "ON CONFLICT(work_item_id, gate) DO UPDATE SET artifact_hash=excluded.artifact_hash, "
       "feedback_hash=excluded.feedback_hash, identical_repeats=excluded.identical_repeats, "
       "updated_at=datetime('now')";
   if (sqlite3_prepare_v2(db, observe_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tx_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, gate, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, plan_hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, feedback_hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 5, repeats);
   rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
   {
      tx_rollback(db);
      return -1;
   }

   out->attempts = attempts;
   out->identical_repeats = repeats;
   if (repeats >= max_identical)
   {
      out->parked = 1;
      snprintf(out->pause_reason, sizeof out->pause_reason, "convergence_no_progress");
   }
   else if (attempts >= max_iterations)
   {
      out->parked = 1;
      snprintf(out->pause_reason, sizeof out->pause_reason, "convergence_limit");
   }

   char detail[512];
   if (out->parked)
   {
      static const char *park_sql =
          "UPDATE lifecycle_work_item "
          "SET current_stage=?, pause_reason=?, paused_state=?, content_hash=?, "
          "cum_cost_usd=cum_cost_usd+?, " CLEAR_RESERVATION ", updated_at=datetime('now') "
          "WHERE work_item_id=?";
      if (sqlite3_prepare_v2(db, park_sql, -1, &st, NULL) != SQLITE_OK)
      {
         tx_rollback(db);
         return -1;
      }
      sqlite3_bind_text(st, 1, plan_stage, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, out->pause_reason, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 3, gate, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 4, plan_hash, -1, SQLITE_TRANSIENT);
      sqlite3_bind_double(st, 5, cost);
      sqlite3_bind_text(st, 6, work_item_id, -1, SQLITE_TRANSIENT);
      rc = sqlite3_step(st);
      sqlite3_finalize(st);
      if (rc != SQLITE_DONE)
      {
         tx_rollback(db);
         return -1;
      }
      /* The detail names what is still unresolved when the reviewer said so:
       * the person who reads this event is deciding whether to intervene, and
       * "convergence_limit" alone does not tell them what to look at. */
      if (unresolved && unresolved[0])
         snprintf(detail, sizeof detail, "%s after %d rounds; still unresolved: %s",
                  out->pause_reason, attempts, unresolved);
      else
         snprintf(detail, sizeof detail, "%s", out->pause_reason);
      if (add_event(db, work_item_id, gate, "pause", "go-wfe", detail, plan_hash, cost) != 0)
      {
         tx_rollback(db);
         return -1;
      }
   }
   else
   {
      static const char *loop_sql =
          "UPDATE lifecycle_work_item "
          "SET current_stage=?, pause_reason='', paused_state='', content_hash=?, "
          "cum_cost_usd=cum_cost_usd+?, " CLEAR_RESERVATION ", updated_at=datetime('now') "
          "WHERE work_item_id=?";
      if (sqlite3_prepare_v2(db, loop_sql, -1, &st, NULL) != SQLITE_OK)
      {
         tx_rollback(db);
         return -1;
      }
      sqlite3_bind_text(st, 1, plan_stage, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, plan_hash, -1, SQLITE_TRANSIENT);
      sqlite3_bind_double(st, 3, cost);
      sqlite3_bind_text(st, 4, work_item_id, -1, SQLITE_TRANSIENT);
      rc = sqlite3_step(st);
      sqlite3_finalize(st);
      if (rc != SQLITE_DONE)
      {
         tx_rollback(db);
         return -1;
      }
      if (add_event(db, work_item_id, gate, "loop", "go-wfe", "requested_changes", plan_hash,
                    cost) != 0)
      {
         tx_rollback(db);
         return -1;
      }
   }
   return tx_commit(db);
}
