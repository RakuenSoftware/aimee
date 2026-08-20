/* wfe_budget_store.c: the workflow engine's budget reservation, moved behind
 * the module.
 *
 * This is the part of the engine's store access that is not a query but a
 * protocol. A run may not spend money twice, a crashed process may not strand
 * the budget it authorized, and two processes may not both decide they are the
 * one replaying the same invocation. The Go engine did all of that in a
 * transaction against the module's own file; it is here now, unchanged in
 * behaviour, because a protocol assembled from separate calls across a wire is
 * not a protocol.
 *
 * The states a reservation moves through:
 *
 *   ""           nothing reserved. A capped tree computes a fair share of what
 *                is left and takes it; an uncapped one reserves zero, which
 *                still records ownership and a lease.
 *   "reserved"   this run holds an estimate. Its owner refreshes the lease; a
 *                different owner may take it only once the lease has lapsed,
 *                and taking it converts the estimate to "unresolved" rather
 *                than releasing it -- the invocation may already have crossed
 *                the provider boundary and spent the money.
 *   "unresolved" authorized spend of unknown size. Replay only.
 *   "actual"     measured cost has replaced the estimate. Replay only.
 *
 * The retry loop is here rather than at the caller for the same reason the
 * transaction is: a lost race is discovered by an UPDATE matching zero rows
 * inside the transaction, and only the process holding that transaction can
 * decide to try again. Six attempts with exponential backoff, matching the
 * engine's own loop.
 */
#include "wfe_engine_store.h"

#include "db1_internal.h"

#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BUDGET_LEASE    "+2 minutes"
#define BUDGET_ATTEMPTS 6

/* Signals from one attempt at the transaction. RETRY means a concurrent writer
 * won a race this attempt could see; the caller waits and tries again. */
typedef enum
{
   ATTEMPT_DONE = 0,
   ATTEMPT_RETRY = 1,
   ATTEMPT_FAILED = -1
} attempt_result_t;

static void rollback(sqlite3 *db)
{
   sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
}

/* How many rows the last statement changed. */
static int changed_rows(sqlite3 *db, sqlite3_stmt *st)
{
   int rc = sqlite3_step(st);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(st);
   return changed;
}

/* The root of the tree this run belongs to, and the cap declared on it. The cap
 * lives on the root because a budget is a property of the whole run, not of the
 * slice that happens to be executing. */
static int budget_root(sqlite3 *db, const char *work_item_id, char *root_id, size_t root_len,
                       double *max_usd)
{
   static const char *sql =
       "WITH RECURSIVE ancestors(id,parent_id,max_usd) AS ("
       "SELECT work_item_id,parent_id,work_item_max_cost_usd FROM lifecycle_work_item "
       "WHERE work_item_id=? "
       "UNION ALL "
       "SELECT parent.work_item_id,parent.parent_id,parent.work_item_max_cost_usd "
       "FROM lifecycle_work_item parent JOIN ancestors child "
       "ON child.parent_id=parent.work_item_id) "
       "SELECT id,max_usd FROM ancestors WHERE parent_id='' LIMIT 1";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int found = 0;
   if (rc == SQLITE_ROW)
   {
      const unsigned char *id = sqlite3_column_text(st, 0);
      snprintf(root_id, root_len, "%s", id ? (const char *)id : "");
      *max_usd = sqlite3_column_double(st, 1);
      found = 1;
   }
   sqlite3_finalize(st);
   return found ? 0 : -1;
}

/* Whether the row's lease is still in the future. A lapsed lease is what makes
 * another owner's takeover legitimate. */
static int lease_is_live(sqlite3 *db, const char *work_item_id, int *live)
{
   static const char *sql = "SELECT reservation_lease_until > datetime('now') "
                            "FROM lifecycle_work_item WHERE work_item_id=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int value = (rc == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
   sqlite3_finalize(st);
   if (rc != SQLITE_ROW)
      return -1;
   *live = value;
   return 0;
}

/* The tree totals a fair share is computed from: what the tree has already
 * spent, what it has outstanding in other reservations, and how many runnable
 * items are waiting to be given a share. */
static int tree_availability(sqlite3 *db, const char *root_id, double *spent, double *outstanding,
                             int *runnable)
{
   static const char *sql =
       "WITH RECURSIVE tree(id) AS ("
       "SELECT ? UNION ALL "
       "SELECT child.work_item_id FROM lifecycle_work_item child "
       "JOIN tree parent ON child.parent_id=parent.id) "
       "SELECT COALESCE(SUM(cum_cost_usd),0),COALESCE(SUM(reserved_cost_usd),0),"
       "SUM(CASE WHEN state='active' AND pause_reason='' AND reserved_cost_usd=0 THEN 1 ELSE 0 "
       "END) "
       "FROM lifecycle_work_item WHERE work_item_id IN tree";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, root_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   if (rc == SQLITE_ROW)
   {
      *spent = sqlite3_column_double(st, 0);
      *outstanding = sqlite3_column_double(st, 1);
      *runnable = sqlite3_column_int(st, 2);
   }
   sqlite3_finalize(st);
   return (rc == SQLITE_ROW) ? 0 : -1;
}

static attempt_result_t reserve_once(sqlite3 *db, const char *work_item_id, const char *owner,
                                     db1_wfe_budget_reservation_t *out)
{
   memset(out, 0, sizeof *out);
   if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
      return ATTEMPT_RETRY; /* another writer holds it; back off and retry */

   double max_usd = 0;
   if (budget_root(db, work_item_id, out->root_id, sizeof out->root_id, &max_usd) != 0)
   {
      rollback(db);
      return ATTEMPT_FAILED;
   }
   out->max_usd = max_usd;
   out->allowed = 1;

   /* The row as it stands, and only if it is runnable: a paused or terminal run
    * has no business taking a reservation. */
   static const char *load_sql =
       "SELECT reserved_cost_usd,reservation_state,reservation_owner FROM lifecycle_work_item "
       "WHERE work_item_id=? AND state='active' AND pause_reason=''";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, load_sql, -1, &st, NULL) != SQLITE_OK)
   {
      rollback(db);
      return ATTEMPT_FAILED;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(st) != SQLITE_ROW)
   {
      sqlite3_finalize(st);
      rollback(db);
      return ATTEMPT_FAILED;
   }
   double current = sqlite3_column_double(st, 0);
   char state[24];
   char holder[128];
   const unsigned char *state_text = sqlite3_column_text(st, 1);
   const unsigned char *owner_text = sqlite3_column_text(st, 2);
   snprintf(state, sizeof state, "%s", state_text ? (const char *)state_text : "");
   snprintf(holder, sizeof holder, "%s", owner_text ? (const char *)owner_text : "");
   sqlite3_finalize(st);

   if (state[0])
   {
      out->amount = current;
      int replay_state = (strcmp(state, "actual") == 0 || strcmp(state, "unresolved") == 0);
      if (replay_state)
      {
         /* Replay is still exactly-once work: take ownership only from an owner
          * whose lease has lapsed, and hold a fresh lease while replaying so a
          * concurrent process cannot steal the reservation mid-reconciliation. */
         out->replay_only = 1;
         if (strcmp(holder, owner) != 0)
         {
            int live = 0;
            if (lease_is_live(db, work_item_id, &live) != 0)
            {
               rollback(db);
               return ATTEMPT_FAILED;
            }
            if (live)
            {
               out->allowed = 0;
               out->busy = 1;
               out->replay_only = 0;
               return (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) ? ATTEMPT_DONE
                                                                                  : ATTEMPT_FAILED;
            }
         }
         static const char *take_sql =
             "UPDATE lifecycle_work_item "
             "SET reservation_owner=?,reservation_lease_until=datetime('now','" BUDGET_LEASE "') "
             "WHERE work_item_id=? AND reservation_state=? AND reservation_owner=? "
             "AND (reservation_owner=? OR reservation_lease_until='' "
             "OR reservation_lease_until<=datetime('now'))";
         sqlite3_stmt *up = NULL;
         if (sqlite3_prepare_v2(db, take_sql, -1, &up, NULL) != SQLITE_OK)
         {
            rollback(db);
            return ATTEMPT_FAILED;
         }
         sqlite3_bind_text(up, 1, owner, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(up, 2, work_item_id, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(up, 3, state, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(up, 4, holder, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(up, 5, owner, -1, SQLITE_TRANSIENT);
         int changed = changed_rows(db, up);
         if (changed != 1)
         {
            rollback(db);
            return ATTEMPT_RETRY; /* acquired concurrently */
         }
         return (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) ? ATTEMPT_DONE
                                                                            : ATTEMPT_FAILED;
      }

      if (strcmp(state, "reserved") == 0 && strcmp(holder, owner) != 0)
      {
         int live = 0;
         if (lease_is_live(db, work_item_id, &live) != 0)
         {
            rollback(db);
            return ATTEMPT_FAILED;
         }
         if (live)
         {
            out->allowed = 0;
            out->busy = 1;
            return (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) ? ATTEMPT_DONE
                                                                               : ATTEMPT_FAILED;
         }
         /* An expired invocation may have crossed the provider boundary. Retain
          * its authorization as unresolved spend and permit only a durable
          * replay, which may replace this estimate with a measured actual
          * exactly once. Releasing it instead would let the tree spend the same
          * money twice. */
         out->replay_only = 1;
         static const char *steal_sql =
             "UPDATE lifecycle_work_item "
             "SET reservation_state='unresolved',reservation_owner=?,"
             "reservation_lease_until=datetime('now','" BUDGET_LEASE "') "
             "WHERE work_item_id=? AND reservation_state='reserved' AND reservation_owner=? "
             "AND reservation_lease_until<=datetime('now')";
         sqlite3_stmt *up = NULL;
         if (sqlite3_prepare_v2(db, steal_sql, -1, &up, NULL) != SQLITE_OK)
         {
            rollback(db);
            return ATTEMPT_FAILED;
         }
         sqlite3_bind_text(up, 1, owner, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(up, 2, work_item_id, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(up, 3, holder, -1, SQLITE_TRANSIENT);
         int changed = changed_rows(db, up);
         if (changed != 1)
         {
            rollback(db);
            return ATTEMPT_RETRY;
         }
         return (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) ? ATTEMPT_DONE
                                                                            : ATTEMPT_FAILED;
      }

      if (strcmp(state, "reserved") == 0)
      {
         /* The holder is asking again: refresh the lease. Best effort, as in the
          * engine -- failing to extend a lease this process already holds is not
          * a reason to refuse it the reservation it already has. */
         static const char *touch_sql =
             "UPDATE lifecycle_work_item "
             "SET reservation_lease_until=datetime('now','" BUDGET_LEASE "') "
             "WHERE work_item_id=? AND reservation_owner=?";
         sqlite3_stmt *up = NULL;
         if (sqlite3_prepare_v2(db, touch_sql, -1, &up, NULL) == SQLITE_OK)
         {
            sqlite3_bind_text(up, 1, work_item_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(up, 2, owner, -1, SQLITE_TRANSIENT);
            sqlite3_step(up);
            sqlite3_finalize(up);
         }
      }
      return (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) ? ATTEMPT_DONE
                                                                         : ATTEMPT_FAILED;
   }

   if (max_usd <= 0)
   {
      /* Uncapped: nothing to divide, but ownership and a lease are still
       * recorded so the same exactly-once rules apply to the invocation. */
      static const char *uncapped_sql =
          "UPDATE lifecycle_work_item "
          "SET reserved_cost_usd=0,reservation_state='reserved',reservation_owner=?,"
          "reservation_lease_until=datetime('now','" BUDGET_LEASE "') "
          "WHERE work_item_id=? AND state='active' AND pause_reason='' AND reservation_state=''";
      sqlite3_stmt *up = NULL;
      if (sqlite3_prepare_v2(db, uncapped_sql, -1, &up, NULL) != SQLITE_OK)
      {
         rollback(db);
         return ATTEMPT_FAILED;
      }
      sqlite3_bind_text(up, 1, owner, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(up, 2, work_item_id, -1, SQLITE_TRANSIENT);
      int changed = changed_rows(db, up);
      if (changed != 1)
      {
         rollback(db);
         return ATTEMPT_RETRY;
      }
      return (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) ? ATTEMPT_DONE
                                                                         : ATTEMPT_FAILED;
   }

   double spent = 0, outstanding = 0;
   int runnable = 0;
   if (tree_availability(db, out->root_id, &spent, &outstanding, &runnable) != 0)
   {
      rollback(db);
      return ATTEMPT_FAILED;
   }
   double remaining = max_usd - spent - outstanding;
   if (remaining <= 0)
   {
      out->allowed = 0;
      return (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) ? ATTEMPT_DONE
                                                                         : ATTEMPT_FAILED;
   }
   if (runnable < 1)
      runnable = 1;
   out->amount = remaining / (double)runnable;

   static const char *reserve_sql =
       "UPDATE lifecycle_work_item "
       "SET reserved_cost_usd=?,reservation_state='reserved',reservation_owner=?,"
       "reservation_lease_until=datetime('now','" BUDGET_LEASE "') "
       "WHERE work_item_id=? AND state='active' AND pause_reason='' AND reserved_cost_usd=0";
   sqlite3_stmt *up = NULL;
   if (sqlite3_prepare_v2(db, reserve_sql, -1, &up, NULL) != SQLITE_OK)
   {
      rollback(db);
      return ATTEMPT_FAILED;
   }
   sqlite3_bind_double(up, 1, out->amount);
   sqlite3_bind_text(up, 2, owner, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(up, 3, work_item_id, -1, SQLITE_TRANSIENT);
   int changed = changed_rows(db, up);
   if (changed != 1)
   {
      rollback(db);
      return ATTEMPT_RETRY;
   }
   return (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) ? ATTEMPT_DONE
                                                                      : ATTEMPT_FAILED;
}

int db1_wfe_budget_reserve(const char *work_item_id, const char *owner,
                           db1_wfe_budget_reservation_t *out)
{
   if (!work_item_id || !work_item_id[0] || !owner || !owner[0] || !out)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   for (int attempt = 0; attempt < BUDGET_ATTEMPTS; attempt++)
   {
      attempt_result_t rc = reserve_once(db, work_item_id, owner, out);
      if (rc == ATTEMPT_DONE)
         return 0;
      if (rc == ATTEMPT_FAILED)
         return -1;
      usleep((useconds_t)(1u << attempt) * 1000u);
   }
   return -1;
}

int db1_wfe_budget_totals(const char *work_item_id, db1_wfe_budget_totals_t *out)
{
   if (!work_item_id || !work_item_id[0] || !out)
      return -1;
   memset(out, 0, sizeof *out);
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   if (budget_root(db, work_item_id, out->root_id, sizeof out->root_id, &out->max_usd) != 0)
      return -1;
   static const char *sql = "WITH RECURSIVE tree(id) AS ("
                            "SELECT ? UNION ALL "
                            "SELECT w.work_item_id FROM lifecycle_work_item w "
                            "JOIN tree t ON w.parent_id=t.id) "
                            "SELECT COALESCE(SUM(cum_cost_usd),0) "
                            "FROM lifecycle_work_item WHERE work_item_id IN tree";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, out->root_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   if (rc == SQLITE_ROW)
      out->spent = sqlite3_column_double(st, 0);
   sqlite3_finalize(st);
   return (rc == SQLITE_ROW) ? 0 : -1;
}

int db1_wfe_budget_release(const char *work_item_id, const char *owner)
{
   if (!work_item_id || !work_item_id[0] || !owner)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   /* Only a live estimate is released. An 'actual' or 'unresolved' reservation
    * is authorized spend that has already happened, and clearing it would let
    * the tree hand the same money out again. */
   static const char *sql = "UPDATE lifecycle_work_item "
                            "SET reserved_cost_usd=0,reservation_state='',reservation_owner='',"
                            "reservation_lease_until='' "
                            "WHERE work_item_id=? AND reservation_owner=? "
                            "AND reservation_state='reserved'";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, owner, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_wfe_budget_heartbeat(const char *work_item_id, const char *owner)
{
   if (!work_item_id || !work_item_id[0] || !owner)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   static const char *sql = "UPDATE lifecycle_work_item "
                            "SET reservation_lease_until=datetime('now','" BUDGET_LEASE "') "
                            "WHERE work_item_id=? AND reservation_owner=? "
                            "AND reservation_state IN ('reserved','actual','unresolved')";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, owner, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_wfe_budget_reconcile(const char *work_item_id, const char *owner, double actual)
{
   if (!work_item_id || !work_item_id[0] || !owner)
      return -1;
   /* Reject non-finite cost at the durable boundary: a NaN or Inf written into
    * a cost column makes every later budget comparison fail and strands the
    * whole tree. The engine guards the same way on its side; this is the guard
    * that matters, because it is the one next to the write. */
   if (actual < 0 || isnan(actual) || isinf(actual))
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   static const char *sql = "UPDATE lifecycle_work_item "
                            "SET reserved_cost_usd=?,reservation_state='actual' "
                            "WHERE work_item_id=? AND reservation_owner=? "
                            "AND reservation_state IN ('reserved','unresolved')";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_double(st, 1, actual);
   sqlite3_bind_text(st, 2, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, owner, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(st);
   if (changed < 0)
      return -1;
   return (changed == 1) ? 1 : 0;
}

/* A runner failure, parked with whatever is known about what it cost.
 *
 * Three cases, and the difference between them is money:
 *
 *   not dispatched          the invocation never left, so nothing was spent and
 *                           the reservation is dropped outright.
 *   dispatched, cost known  the measured cost is committed and the reservation
 *                           is dropped: the story is complete.
 *   dispatched, cost NOT    the invocation crossed the provider boundary and may
 *   known                   have spent an unknown amount. The known prefix is
 *                           committed and the REST of the authorization is
 *                           retained as 'unresolved' -- releasing it would let
 *                           the tree hand out money that may already be gone.
 */
int db1_wfe_park_runner_failure(const char *work_item_id, const char *stage, const char *owner,
                                const char *reason, const char *detail, int dispatched,
                                int cost_known, double actual)
{
   if (!work_item_id || !work_item_id[0] || !stage || !stage[0] || !owner || !owner[0] || !reason ||
       !reason[0] || actual < 0 || isnan(actual) || isinf(actual))
      return -1;
   sqlite3 *db = db1_conn();
   if (!db || sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
      return -1;

   const char *state = "";
   double amount = 0;
   double event_cost = 0;
   if (dispatched && cost_known)
   {
      event_cost = actual;
   }
   else if (dispatched)
   {
      state = "unresolved";
      static const char *held_sql = "SELECT reserved_cost_usd FROM lifecycle_work_item "
                                    "WHERE work_item_id=? AND reservation_owner=? "
                                    "AND reservation_state IN ('reserved','unresolved')";
      sqlite3_stmt *st = NULL;
      if (sqlite3_prepare_v2(db, held_sql, -1, &st, NULL) != SQLITE_OK)
      {
         rollback(db);
         return -1;
      }
      sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, owner, -1, SQLITE_TRANSIENT);
      int have = sqlite3_step(st) == SQLITE_ROW;
      amount = have ? sqlite3_column_double(st, 0) : 0;
      sqlite3_finalize(st);
      if (!have)
      {
         rollback(db);
         return -1;
      }
      /* Measured spend from completed reroute attempts is committed now; only
       * what is left of the authorization stays unresolved. */
      event_cost = actual;
      if (amount > actual)
         amount -= actual;
      else if (amount > 0)
         amount = 0;
   }

   static const char *park_sql =
       "UPDATE lifecycle_work_item "
       "SET pause_reason=?, paused_state=?, cum_cost_usd=cum_cost_usd+?, reserved_cost_usd=?, "
       "reservation_state=?, reservation_owner=CASE WHEN ?='' THEN '' ELSE ? END, "
       "reservation_lease_until='', updated_at=datetime('now') "
       "WHERE work_item_id=? AND current_stage=? AND state='active' AND pause_reason='' "
       "AND reservation_owner=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, park_sql, -1, &st, NULL) != SQLITE_OK)
   {
      rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, reason, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage, -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(st, 3, event_cost);
   sqlite3_bind_double(st, 4, amount);
   sqlite3_bind_text(st, 5, state, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 6, state, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 7, owner, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 8, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 9, stage, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 10, owner, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(st);
   if (changed != 1)
   {
      rollback(db);
      return -1;
   }

   static const char *event_sql =
       "INSERT INTO lifecycle_event (work_item_id,stage,kind,actor,detail,cost_usd) "
       "VALUES (?,?,'pause','go-wfe',?,?)";
   if (sqlite3_prepare_v2(db, event_sql, -1, &st, NULL) != SQLITE_OK)
   {
      rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, detail ? detail : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(st, 4, event_cost);
   rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
   {
      rollback(db);
      return -1;
   }
   return (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) ? 0 : -1;
}

/* A replay whose result never came back. What happens next depends entirely on
 * whether the money is known to have been spent:
 *
 *   'unresolved'  the estimate was never committed, so drop it and let the run
 *                 be re-dispatched fresh. Returns 1: redispatch.
 *   'actual'      the cost was measured but the RESULT is unreproducible.
 *                 Commit the spend and park for a human rather than silently
 *                 paying for the same work twice. Returns 0: do not redispatch.
 *
 * Any other state is not replayable and is an error rather than a guess.
 */
int db1_wfe_recover_lost_replay(const char *work_item_id, const char *stage, const char *owner)
{
   if (!work_item_id || !work_item_id[0] || !owner)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db || sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
      return -1;

   static const char *load_sql = "SELECT reservation_state,reservation_owner,reserved_cost_usd "
                                 "FROM lifecycle_work_item WHERE work_item_id=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, load_sql, -1, &st, NULL) != SQLITE_OK)
   {
      rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(st) != SQLITE_ROW)
   {
      sqlite3_finalize(st);
      rollback(db);
      return -1;
   }
   char state[24];
   char holder[128];
   const unsigned char *state_text = sqlite3_column_text(st, 0);
   const unsigned char *owner_text = sqlite3_column_text(st, 1);
   snprintf(state, sizeof state, "%s", state_text ? (const char *)state_text : "");
   snprintf(holder, sizeof holder, "%s", owner_text ? (const char *)owner_text : "");
   double amount = sqlite3_column_double(st, 2);
   sqlite3_finalize(st);

   if (strcmp(holder, owner) != 0)
   {
      /* Someone else's invocation. Recovering it would decide the fate of money
       * this caller did not authorize. */
      rollback(db);
      return -1;
   }

   const char *update_sql = NULL;
   const char *event_kind = NULL;
   const char *event_detail = NULL;
   double event_cost = 0;
   int redispatch = 0;
   if (strcmp(state, "unresolved") == 0)
   {
      update_sql =
          "UPDATE lifecycle_work_item "
          "SET reserved_cost_usd=0, reservation_state='', reservation_owner='', "
          "reservation_lease_until='', updated_at=datetime('now') "
          "WHERE work_item_id=? AND reservation_owner=? AND reservation_state='unresolved'";
      event_kind = "redispatch";
      event_detail = "replay result lost; re-dispatching fresh";
      redispatch = 1;
   }
   else if (strcmp(state, "actual") == 0)
   {
      update_sql = "UPDATE lifecycle_work_item "
                   "SET cum_cost_usd=cum_cost_usd+reserved_cost_usd, reserved_cost_usd=0, "
                   "reservation_state='', reservation_owner='', reservation_lease_until='', "
                   "pause_reason='replay_unrecoverable', paused_state=current_stage, "
                   "updated_at=datetime('now') "
                   "WHERE work_item_id=? AND reservation_owner=? AND reservation_state='actual'";
      event_kind = "pause";
      event_detail = "replay_unrecoverable: reconciled result lost, parked for human";
      event_cost = amount;
   }
   else
   {
      rollback(db);
      return -1;
   }

   if (sqlite3_prepare_v2(db, update_sql, -1, &st, NULL) != SQLITE_OK)
   {
      rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, owner, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(st);
   if (changed != 1)
   {
      rollback(db);
      return -1;
   }

   static const char *event_sql =
       "INSERT INTO lifecycle_event (work_item_id,stage,kind,actor,detail,cost_usd) "
       "VALUES (?,?,?,'go-wfe',?,?)";
   if (sqlite3_prepare_v2(db, event_sql, -1, &st, NULL) != SQLITE_OK)
   {
      rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage ? stage : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, event_kind, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, event_detail, -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(st, 5, event_cost);
   rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
   {
      rollback(db);
      return -1;
   }
   return (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) ? redispatch : -1;
}
