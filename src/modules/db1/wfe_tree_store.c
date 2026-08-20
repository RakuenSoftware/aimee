/* wfe_tree_store.c: the engine's operations over a whole run tree.
 *
 * A run can fan out into child slices, and four things have to happen to the
 * tree as a unit rather than to its members one at a time: stopping it,
 * reconciling children whose ancestor has already finished, parking it when the
 * budget runs out, and deleting it.
 *
 * Each of these was a transaction in the Go engine built from a recursive CTE.
 * Doing them member-by-member across a wire would not just be slow -- it would
 * be wrong. A stop that reached half the tree leaves agents running under a run
 * the operator believes is stopped, and that is the failure mode that costs
 * money while nobody is looking at it.
 *
 * The recursion seeds differ, and the difference matters:
 *
 *   tree(id)    seeds with the named run, so it includes the run itself.
 *   orphan(id)  seeds with children whose PARENT is already terminal, so it
 *               includes no root -- it is looking for work that outlived the
 *               reason it existed.
 */
#include "wfe_engine_store.h"

#include "db1_internal.h"

#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* The subtree of a run, the run included. Used by three of the four below. */
#define TREE_CTE                                                                                   \
   "WITH RECURSIVE tree(id) AS ("                                                                  \
   "SELECT ? UNION ALL "                                                                           \
   "SELECT child.work_item_id FROM lifecycle_work_item child "                                     \
   "JOIN tree parent ON child.parent_id=parent.id) "

/* Active runs whose parent has already reached a terminal state, and their
 * active descendants. Seeded from the children rather than from a root: this is
 * a sweep looking for orphans anywhere, not a walk from somewhere known. */
#define ORPHAN_CTE                                                                                 \
   "WITH RECURSIVE orphan(id) AS ("                                                                \
   "SELECT child.work_item_id FROM lifecycle_work_item child "                                     \
   "JOIN lifecycle_work_item parent ON parent.work_item_id=child.parent_id "                       \
   "WHERE child.state='active' AND parent.state IN "                                               \
   "('accepted','rejected','stopped','abandoned') "                                                \
   "UNION "                                                                                        \
   "SELECT child.work_item_id FROM lifecycle_work_item child "                                     \
   "JOIN orphan parent ON child.parent_id=parent.id WHERE child.state='active') "

static void tree_rollback(sqlite3 *db)
{
   sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
}

static int tree_begin(sqlite3 *db)
{
   return sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static int tree_commit(sqlite3 *db)
{
   if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK)
      return 0;
   tree_rollback(db);
   return -1;
}

/* One terminal event per run being ended, carrying the stage and hash it was at
 * when it ended. Written before the state changes, while those values are still
 * the run's own. */
static int terminal_event(sqlite3 *db, const char *work_item_id, const char *stage,
                          const char *actor, const char *detail, const char *content_hash)
{
   static const char *sql = "INSERT INTO lifecycle_event "
                            "(work_item_id,stage,kind,actor,detail,content_hash) "
                            "VALUES (?,?,'terminal',?,?,?)";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, stage ? stage : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, actor, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, detail, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 5, content_hash ? content_hash : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

/* Collect the active members of a set, writing a terminal event for each and
 * returning their ids. Shared by stop and orphan reconciliation, which differ
 * only in how the set is defined and what the event says. */
static int end_active_set(sqlite3 *db, const char *select_sql, const char *seed, const char *detail,
                          char (*out_ids)[DB1_WFE_ID_LEN], int max)
{
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, select_sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   if (seed)
      sqlite3_bind_text(st, 1, seed, -1, SQLITE_TRANSIENT);
   int n = 0;
   int failed = 0;
   while (sqlite3_step(st) == SQLITE_ROW)
   {
      const unsigned char *id = sqlite3_column_text(st, 0);
      const unsigned char *stage = sqlite3_column_text(st, 1);
      const unsigned char *hash = sqlite3_column_text(st, 2);
      if (!id)
         continue;
      if (terminal_event(db, (const char *)id, stage ? (const char *)stage : "", "go-wfe", detail,
                         hash ? (const char *)hash : "") != 0)
      {
         failed = 1;
         break;
      }
      if (n < max)
      {
         snprintf(out_ids[n], DB1_WFE_ID_LEN, "%s", (const char *)id);
         n++;
      }
   }
   sqlite3_finalize(st);
   return failed ? -1 : n;
}

static int run_bound(sqlite3 *db, const char *sql, const char *bind1)
{
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   if (bind1)
      sqlite3_bind_text(st, 1, bind1, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_wfe_stop_tree(const char *work_item_id, char (*out_ids)[DB1_WFE_ID_LEN], int max)
{
   if (!work_item_id || !work_item_id[0] || !out_ids || max <= 0)
      return -1;
   if (max > DB1_WFE_ID_LIST_MAX)
      max = DB1_WFE_ID_LIST_MAX;
   sqlite3 *db = db1_conn();
   if (!db || tree_begin(db) != 0)
      return -1;

   /* Touch the named run first, under the same transaction, so a concurrent
    * writer moving it is serialised against this stop rather than racing it. */
   static const char *touch_sql =
       "UPDATE lifecycle_work_item SET state=state WHERE work_item_id=? AND state='active'";
   if (run_bound(db, touch_sql, work_item_id) != 0)
   {
      tree_rollback(db);
      return -1;
   }

   static const char *select_sql =
       TREE_CTE "SELECT item.work_item_id, item.current_stage, item.content_hash "
                "FROM lifecycle_work_item item JOIN tree ON tree.id=item.work_item_id "
                "WHERE item.state='active'";
   int n = end_active_set(db, select_sql, work_item_id, "operator_stop", out_ids, max);
   if (n < 0)
   {
      tree_rollback(db);
      return -1;
   }

   static const char *stop_sql =
       TREE_CTE "UPDATE lifecycle_work_item SET state='stopped', pause_reason='', paused_state='', "
                "reserved_cost_usd=0, reservation_state='', reservation_owner='', "
                "reservation_lease_until='', updated_at=datetime('now') "
                "WHERE state='active' AND work_item_id IN (SELECT id FROM tree)";
   if (run_bound(db, stop_sql, work_item_id) != 0)
   {
      tree_rollback(db);
      return -1;
   }

   /* A stopped tree will not resume, so its frozen create records are spent. */
   static const char *thaw_sql =
       TREE_CTE "DELETE FROM wfe_frozen_create WHERE work_item_id IN (SELECT id FROM tree)";
   if (run_bound(db, thaw_sql, work_item_id) != 0)
   {
      tree_rollback(db);
      return -1;
   }
   return (tree_commit(db) == 0) ? n : -1;
}

int db1_wfe_reconcile_orphans(char (*out_ids)[DB1_WFE_ID_LEN], int max)
{
   if (!out_ids || max <= 0)
      return -1;
   if (max > DB1_WFE_ID_LIST_MAX)
      max = DB1_WFE_ID_LIST_MAX;
   sqlite3 *db = db1_conn();
   if (!db || tree_begin(db) != 0)
      return -1;

   static const char *select_sql =
       ORPHAN_CTE "SELECT item.work_item_id, item.current_stage, item.content_hash "
                  "FROM lifecycle_work_item item JOIN orphan ON orphan.id=item.work_item_id";
   int n = end_active_set(db, select_sql, NULL, "ancestor_terminal", out_ids, max);
   if (n < 0)
   {
      tree_rollback(db);
      return -1;
   }

   static const char *stop_sql = ORPHAN_CTE
       "UPDATE lifecycle_work_item SET state='stopped', pause_reason='', paused_state='', "
       "reserved_cost_usd=0, reservation_state='', reservation_owner='', "
       "reservation_lease_until='', updated_at=datetime('now') "
       "WHERE state='active' AND work_item_id IN (SELECT id FROM orphan)";
   if (run_bound(db, stop_sql, NULL) != 0)
   {
      tree_rollback(db);
      return -1;
   }

   static const char *thaw_sql = "DELETE FROM wfe_frozen_create WHERE work_item_id=?";
   for (int i = 0; i < n; i++)
   {
      if (run_bound(db, thaw_sql, out_ids[i]) != 0)
      {
         tree_rollback(db);
         return -1;
      }
   }
   return (tree_commit(db) == 0) ? n : -1;
}

int db1_wfe_park_budget_tree(const char *root_id, const char *completed_item_id, double added_cost)
{
   if (!root_id || !root_id[0] || !completed_item_id || isnan(added_cost) || isinf(added_cost))
      return -1;
   sqlite3 *db = db1_conn();
   if (!db || tree_begin(db) != 0)
      return -1;

   /* Charge the completed item and release its reservation: this call happens
    * when an invocation has finished and its cost turned out to exceed what the
    * tree had left. */
   static const char *charge_sql = "UPDATE lifecycle_work_item "
                                   "SET cum_cost_usd=cum_cost_usd+?, reserved_cost_usd=0, "
                                   "reservation_state='', reservation_owner='', "
                                   "reservation_lease_until='' "
                                   "WHERE work_item_id=?";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, charge_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tree_rollback(db);
      return -1;
   }
   sqlite3_bind_double(st, 1, added_cost);
   sqlite3_bind_text(st, 2, completed_item_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
   {
      tree_rollback(db);
      return -1;
   }

   /* Park every runnable member of the tree, recording the stage each was in so
    * a later resume can put it back. Already-parked runs keep the reason they
    * have: whatever parked them first is the more specific explanation. */
   static const char *park_sql = TREE_CTE
       "UPDATE lifecycle_work_item "
       "SET pause_reason='budget_cap', paused_state=current_stage, "
       "updated_at=datetime('now') "
       "WHERE state='active' AND pause_reason='' AND work_item_id IN (SELECT id FROM tree)";
   if (run_bound(db, park_sql, root_id) != 0)
   {
      tree_rollback(db);
      return -1;
   }

   /* One event against the item that tipped the tree over, carrying the cost
    * that did it. */
   static const char *event_sql =
       "INSERT INTO lifecycle_event (work_item_id,stage,kind,actor,detail,cost_usd) "
       "SELECT work_item_id,current_stage,'pause','go-wfe','budget_cap',? "
       "FROM lifecycle_work_item WHERE work_item_id=?";
   if (sqlite3_prepare_v2(db, event_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tree_rollback(db);
      return -1;
   }
   sqlite3_bind_double(st, 1, added_cost);
   sqlite3_bind_text(st, 2, completed_item_id, -1, SQLITE_TRANSIENT);
   rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
   {
      tree_rollback(db);
      return -1;
   }
   return tree_commit(db);
}

int db1_wfe_delete_tree(const char *work_item_id)
{
   if (!work_item_id || !work_item_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db || tree_begin(db) != 0)
      return -1;

   /* Refuse to delete a tree with anything still running. Deleting the row does
    * not stop the agent working under it, and the run would keep spending with
    * nothing left to record it against. */
   static const char *active_sql =
       TREE_CTE "SELECT count(*) FROM lifecycle_work_item "
                "WHERE work_item_id IN (SELECT id FROM tree) AND state='active'";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, active_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tree_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   int active = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
   sqlite3_finalize(st);
   if (active != 0)
   {
      tree_rollback(db);
      return -1;
   }

   /* Children before parents is not required here -- every statement is scoped
    * by the same CTE and the whole set goes in one transaction -- but the work
    * item row goes last so that a failure part-way cannot leave rows pointing at
    * a work item that no longer exists. */
   static const char *const deletes[] = {
       TREE_CTE "DELETE FROM wfe_frozen_create "
                "WHERE work_item_id IN (SELECT id FROM tree) OR parent_id IN (SELECT id FROM tree)",
       TREE_CTE "DELETE FROM wfe_convergence WHERE work_item_id IN (SELECT id FROM tree)",
       TREE_CTE "DELETE FROM lifecycle_stage_attempt WHERE work_item_id IN (SELECT id FROM tree)",
       TREE_CTE "DELETE FROM lifecycle_event WHERE work_item_id IN (SELECT id FROM tree)",
       TREE_CTE "DELETE FROM lifecycle_work_item WHERE work_item_id IN (SELECT id FROM tree)",
   };
   for (size_t i = 0; i < sizeof deletes / sizeof deletes[0]; i++)
   {
      if (run_bound(db, deletes[i], work_item_id) != 0)
      {
         tree_rollback(db);
         return -1;
      }
   }
   return tree_commit(db);
}

/* The human gate. Distinct from db1_work_item_gate_apply, which serves the
 * daemon's own gate flow and guards on pause_reason='pending_human' plus a
 * content hash. The engine's gate parks on 'human_gate' and does not compare
 * hashes, so these are two different gates that happen to share a word. */
int db1_wfe_resolve_gate(const char *work_item_id, const char *from_stage, const char *to_stage,
                         const char *decision, const char *content_hash)
{
   if (!work_item_id || !work_item_id[0] || !from_stage || !to_stage || !to_stage[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db || tree_begin(db) != 0)
      return -1;

   static const char *sql = "UPDATE lifecycle_work_item "
                            "SET current_stage=?, pause_reason='', paused_state='', "
                            "content_hash=?, updated_at=datetime('now') "
                            "WHERE work_item_id=? AND current_stage=? AND state='active' "
                            "AND pause_reason='human_gate'";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
   {
      tree_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, to_stage, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, content_hash ? content_hash : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, from_stage, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(st);
   if (changed != 1)
   {
      tree_rollback(db);
      return -1;
   }

   static const char *event_sql =
       "INSERT INTO lifecycle_event (work_item_id,stage,kind,actor,detail,content_hash) "
       "VALUES (?,?,'gate','operator',?,?)";
   if (sqlite3_prepare_v2(db, event_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tree_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, from_stage, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, decision ? decision : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, content_hash ? content_hash : "", -1, SQLITE_TRANSIENT);
   rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE)
   {
      tree_rollback(db);
      return -1;
   }
   return tree_commit(db);
}

int db1_wfe_reject_gate(const char *work_item_id, const char *stage, const char *content_hash)
{
   if (!work_item_id || !work_item_id[0] || !stage)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db || tree_begin(db) != 0)
      return -1;

   static const char *sql = "UPDATE lifecycle_work_item "
                            "SET state='rejected', pause_reason='', paused_state='', "
                            "content_hash=?, updated_at=datetime('now') "
                            "WHERE work_item_id=? AND current_stage=? AND state='active' "
                            "AND pause_reason='human_gate'";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
   {
      tree_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, content_hash ? content_hash : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, stage, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(st);
   if (changed != 1)
   {
      tree_rollback(db);
      return -1;
   }
   /* 'operator', not 'go-wfe': a rejection is a person's decision, and the
    * history is read to find out who ended a run. */
   if (terminal_event(db, work_item_id, stage, "operator", "human rejection", content_hash) != 0)
   {
      tree_rollback(db);
      return -1;
   }
   return tree_commit(db);
}

int db1_wfe_claim_frozen_creates(const db1_wfe_frozen_claim_t *claim,
                                 db1_wfe_frozen_conflict_t *out)
{
   if (!claim || !out || !claim->parent_id[0] || !claim->work_item_id[0])
      return -1;
   memset(out, 0, sizeof *out);
   int count = claim->create_count;
   if (count < 0 || count > DB1_WFE_FROZEN_MAX)
      return -1;
   if (count == 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db || tree_begin(db) != 0)
      return -1;

   /* The first statement is a WRITE on purpose. It reserves SQLite's sole
    * writer before the conflict read below, so two claims can never both
    * observe an empty claim set and then race independent inserts. It also
    * validates the owner: only an active child of this parent may freeze. */
   static const char *own_sql = "UPDATE lifecycle_work_item SET updated_at=updated_at "
                                "WHERE work_item_id=? AND parent_id=? AND state='active'";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, own_sql, -1, &st, NULL) != SQLITE_OK)
   {
      tree_rollback(db);
      return -1;
   }
   sqlite3_bind_text(st, 1, claim->work_item_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, claim->parent_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   int eligible = (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
   sqlite3_finalize(st);
   if (eligible != 1)
   {
      tree_rollback(db);
      return -1;
   }

   for (int i = 0; i < count; i++)
   {
      const char *path = claim->creates[i].path;
      const char *hash = claim->creates[i].content_hash;
      if (!path[0] || !hash[0])
      {
         tree_rollback(db);
         return -1;
      }
      /* A sibling that froze this path with DIFFERENT content. Identical
       * content is two slices agreeing and is allowed to coexist. */
      static const char *clash_sql = "SELECT work_item_id FROM wfe_frozen_create "
                                     "WHERE parent_id=? AND path=? AND work_item_id<>? "
                                     "AND content_hash<>? ORDER BY work_item_id LIMIT 1";
      if (sqlite3_prepare_v2(db, clash_sql, -1, &st, NULL) != SQLITE_OK)
      {
         tree_rollback(db);
         return -1;
      }
      sqlite3_bind_text(st, 1, claim->parent_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, path, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 3, claim->work_item_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 4, hash, -1, SQLITE_TRANSIENT);
      int clashed = sqlite3_step(st) == SQLITE_ROW;
      if (clashed)
      {
         const unsigned char *existing = sqlite3_column_text(st, 0);
         snprintf(out->path, sizeof out->path, "%s", path);
         snprintf(out->existing_work_item, sizeof out->existing_work_item, "%s",
                  existing ? (const char *)existing : "");
         snprintf(out->conflicting_work_item, sizeof out->conflicting_work_item, "%s",
                  claim->work_item_id);
      }
      sqlite3_finalize(st);
      if (clashed)
      {
         /* Roll back the whole claim: a partial path set would let the loser's
          * earlier paths stand while its later ones were refused. */
         tree_rollback(db);
         return 0;
      }

      static const char *publish_sql =
          "INSERT INTO wfe_frozen_create (parent_id,path,work_item_id,content_hash) "
          "VALUES (?,?,?,?) "
          "ON CONFLICT(parent_id,path,work_item_id) DO UPDATE SET "
          "content_hash=excluded.content_hash, updated_at=datetime('now')";
      if (sqlite3_prepare_v2(db, publish_sql, -1, &st, NULL) != SQLITE_OK)
      {
         tree_rollback(db);
         return -1;
      }
      sqlite3_bind_text(st, 1, claim->parent_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, path, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 3, claim->work_item_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 4, hash, -1, SQLITE_TRANSIENT);
      rc = sqlite3_step(st);
      sqlite3_finalize(st);
      if (rc != SQLITE_DONE)
      {
         tree_rollback(db);
         return -1;
      }
   }
   return tree_commit(db);
}
