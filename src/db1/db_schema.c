/* DB1 (SQLite) idempotent schema bootstrap — single CREATE-IF-NOT-EXISTS
 * pass called at DB1 open time. The DB2 half of the split lives in
 * db2/db_schema.c. See docs/STORAGE_TIERS.md. */

#include "db_schema.h"

#include "../schema_data.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static void copy_err(char *errbuf, size_t errlen, const char *src)
{
   if (!errbuf || errlen == 0)
      return;
   snprintf(errbuf, errlen, "%s", src ? src : "");
}

static void db1_run_migrations(sqlite3 *db)
{
   /* Each statement is executed independently; errors are silently ignored so
    * "duplicate column name" on re-runs doesn't abort the sequence. */
   static const char *migrations[] = {
       "ALTER TABLE payload_rewrite_state ADD COLUMN consecutive_deferred_count"
       " INTEGER NOT NULL DEFAULT 0",
       "ALTER TABLE eval_results ADD COLUMN ablation TEXT NOT NULL DEFAULT 'full'",
       "ALTER TABLE eval_results ADD COLUMN tool_call_failures INTEGER NOT NULL DEFAULT 0",
       "ALTER TABLE eval_results ADD COLUMN rescue_recoveries INTEGER NOT NULL DEFAULT 0",
       "ALTER TABLE eval_results ADD COLUMN dataset_hash TEXT NOT NULL DEFAULT ''",
       "ALTER TABLE eval_results ADD COLUMN target_hash TEXT NOT NULL DEFAULT ''",
       "ALTER TABLE eval_results ADD COLUMN harness_version TEXT NOT NULL DEFAULT '1'",
       "ALTER TABLE eval_results ADD COLUMN hardware_profile TEXT NOT NULL DEFAULT ''",
       "ALTER TABLE eval_results ADD COLUMN seed INTEGER NOT NULL DEFAULT 0",
       "ALTER TABLE coord_job_tasks ADD COLUMN preempt_requeues INTEGER NOT NULL DEFAULT 0",
       /* Gateway ambient-presence: track which platform/channel originated a session */
       "ALTER TABLE server_sessions ADD COLUMN source TEXT NOT NULL DEFAULT ''",
       "ALTER TABLE server_sessions ADD COLUMN chat_key TEXT NOT NULL DEFAULT ''",
       /* Autonomous-dev: persist the forge PR ref opened by pr.open so the
        * downstream gate.ci / check.mergeable / merge blocks resolve the real PR
        * instead of the work-item id (full-autonomous-development Phase A). */
       "ALTER TABLE lifecycle_work_item ADD COLUMN pr_ref TEXT NOT NULL DEFAULT ''",
       /* F2 (full-autonomous-development): the per-work-item git worktree
        * (aimee/wi/<id>) the autonomous run's delegates + freeze act in, so
        * concurrent runs don't share one checkout. */
       "ALTER TABLE lifecycle_work_item ADD COLUMN worktree TEXT NOT NULL DEFAULT ''",
       /* intake-auth: the attested principal that submitted this autonomous run, for
        * audit binding + per-principal concurrency/rate caps on POST /v1/dev/submit. */
       "ALTER TABLE lifecycle_work_item ADD COLUMN submitter TEXT NOT NULL DEFAULT ''",
       /* sliced-lifecycle build: the parent work item of a child "slice" run
        * (foreach.workflow fans one child per split packet). "" for a top-level
        * run; set to the parent's work_item_id for a slice child, so the parent's
        * foreach gate can aggregate its children's terminal states. */
       "ALTER TABLE lifecycle_work_item ADD COLUMN parent_id TEXT NOT NULL DEFAULT ''",
       /* Rename the multi-agent "workflow session" store to "ensemble" so it no
        * longer collides with the workflow ENGINE. Runs before the canonical
        * schema SQL: on a legacy DB the RENAME preserves every row and the
        * subsequent CREATE TABLE IF NOT EXISTS ensembles no-ops; on a fresh or
        * already-migrated DB the RENAME fails silently ("no such table") and the
        * schema SQL builds it. Drop the legacy-named index that RENAME carries
        * over, leaving only idx_ensembles_status (created by the schema SQL). */
       "ALTER TABLE workflow_sessions RENAME TO ensembles",
       "DROP INDEX IF EXISTS idx_workflow_sessions_status",
       /* The inter-session work queue is retired. Its tables are dropped rather
        * than left behind: an upgraded DB would otherwise keep them forever while
        * a fresh install never creates them, so the two would diverge by
        * installation history — and any code that still touched them would work
        * on one and fail on the other. Nothing reads these rows now; the queue's
        * whole surface (CLI, /v1 routes, MCP tools, handlers) is gone. Dropped
        * child-first, though there is no FK between them. */
       "DROP TABLE IF EXISTS work_queue_log",
       "DROP TABLE IF EXISTS work_queue",
       NULL,
   };
   for (int i = 0; migrations[i]; i++)
      sqlite3_exec(db, migrations[i], NULL, NULL, NULL);
}

int db1_apply_schema_sqlite(sqlite3 *db, char *errbuf, size_t errlen)
{
   if (!db)
      return -1;
   /* Run catch-up ALTERs BEFORE the canonical schema. The schema SQL contains
    * objects (e.g. a partial UNIQUE index over a later-added column) that
    * reference columns added to a CREATE TABLE after the table first shipped.
    * On a legacy DB the table pre-exists, so the table's `IF NOT EXISTS` is a
    * no-op and the dependent index would fail ("no such column"), aborting the
    * whole apply and leaving DB1 uninitialised. Adding the drift columns first
    * makes the index creation succeed. On a fresh DB the ALTERs no-op ("no such
    * table") and the schema SQL builds every table at full shape. */
   db1_run_migrations(db);
   char *err = NULL;
   int rc = sqlite3_exec(db, AIMEE_DB1_SCHEMA_SQL, NULL, NULL, &err);
   if (rc != SQLITE_OK)
   {
      copy_err(errbuf, errlen, err ? err : sqlite3_errmsg(db));
      sqlite3_free(err);
      return -1;
   }
   return 0;
}

/* ── declarative column reconciliation ─────────────────────────────────── */

#define RECONCILE_MAX_COLS 128
#define RECONCILE_NAME_MAX 64
#define RECONCILE_SQL_MAX  512

void db1_reconcile_columns(sqlite3 *db)
{
   if (!db)
      return;

   /* Build reference shape from canonical schema in an in-memory DB */
   sqlite3 *ref = NULL;
   if (sqlite3_open(":memory:", &ref) != SQLITE_OK)
      return;
   sqlite3_exec(ref, AIMEE_DB1_SCHEMA_SQL, NULL, NULL, NULL);

   /* Walk every ordinary table in the reference DB */
   sqlite3_stmt *tbl_st = NULL;
   if (sqlite3_prepare_v2(ref,
                          "SELECT name FROM sqlite_master"
                          " WHERE type='table' ORDER BY name",
                          -1, &tbl_st, NULL) != SQLITE_OK)
   {
      sqlite3_close(ref);
      return;
   }

   while (sqlite3_step(tbl_st) == SQLITE_ROW)
   {
      const char *tbl = (const char *)sqlite3_column_text(tbl_st, 0);
      if (!tbl)
         continue;

      /* Collect live column names */
      char live[RECONCILE_MAX_COLS][RECONCILE_NAME_MAX];
      int nlive = 0;
      {
         char pq[RECONCILE_SQL_MAX];
         snprintf(pq, sizeof(pq), "PRAGMA table_info(\"%s\")", tbl);
         sqlite3_stmt *lc = NULL;
         if (sqlite3_prepare_v2(db, pq, -1, &lc, NULL) == SQLITE_OK)
         {
            while (sqlite3_step(lc) == SQLITE_ROW && nlive < RECONCILE_MAX_COLS)
            {
               const char *n = (const char *)sqlite3_column_text(lc, 1);
               if (n)
                  snprintf(live[nlive++], RECONCILE_NAME_MAX, "%s", n);
            }
            sqlite3_finalize(lc);
         }
      }
      if (nlive == 0)
         continue; /* virtual table or table absent in live DB */

      /* Compare reference columns to live; ADD COLUMN for any gap */
      char pq[RECONCILE_SQL_MAX];
      snprintf(pq, sizeof(pq), "PRAGMA table_info(\"%s\")", tbl);
      sqlite3_stmt *rc_st = NULL;
      if (sqlite3_prepare_v2(ref, pq, -1, &rc_st, NULL) != SQLITE_OK)
         continue;

      while (sqlite3_step(rc_st) == SQLITE_ROW)
      {
         const char *col = (const char *)sqlite3_column_text(rc_st, 1);
         const char *type = (const char *)sqlite3_column_text(rc_st, 2);
         int notnull = sqlite3_column_int(rc_st, 3);
         const char *dflt = (const char *)sqlite3_column_text(rc_st, 4);
         if (!col)
            continue;

         int found = 0;
         for (int i = 0; i < nlive; i++)
         {
            if (strcasecmp(live[i], col) == 0)
            {
               found = 1;
               break;
            }
         }
         if (found)
            continue;

         /* Only add columns that have a DEFAULT — NOT NULL without DEFAULT
          * would require a data backfill and belongs in a migration block. */
         if (notnull && (!dflt || !dflt[0]))
            continue;

         char sql[RECONCILE_SQL_MAX];
         if (dflt && dflt[0])
         {
            if (notnull)
               snprintf(sql, sizeof(sql),
                        "ALTER TABLE \"%s\" ADD COLUMN \"%s\" %s NOT NULL DEFAULT %s", tbl, col,
                        type ? type : "TEXT", dflt);
            else
               snprintf(sql, sizeof(sql), "ALTER TABLE \"%s\" ADD COLUMN \"%s\" %s DEFAULT %s", tbl,
                        col, type ? type : "TEXT", dflt);
         }
         else
         {
            snprintf(sql, sizeof(sql), "ALTER TABLE \"%s\" ADD COLUMN \"%s\" %s", tbl, col,
                     type ? type : "TEXT");
         }

         if (sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK)
            fprintf(stderr, "db1.schema.reconcile: added %s.%s\n", tbl, col);
      }
      sqlite3_finalize(rc_st);
   }

   sqlite3_finalize(tbl_st);
   sqlite3_close(ref);
}
