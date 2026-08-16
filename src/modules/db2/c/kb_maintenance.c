/* db2/kb_maintenance.c: KB temporal confidence decay and orphan pruning.
 *
 * Two passes run inside a single Postgres transaction:
 *   1. Decay pass  — reduces confidence of committed artifacts using
 *      exponential decay: new_conf = max(floor, conf * exp(-lambda * age_days))
 *   2. Orphan pass — marks committed artifacts with no citations, no outbound
 *      links, decayed below the confidence floor, and older than orphan_prune_days
 *      as state = 'retired'.
 *
 * When dry_run=1 the transaction is rolled back so no state changes persist,
 * but out->rows_decayed / out->orphans_pruned are still populated from the
 * affected-row counts that Postgres reports before rollback.
 *
 * A record is inserted into kb_maintenance_runs at the end of each
 * successful (or dry-run) execution.  That table is created by the
 * accompanying migration.
 */

#include "kb_maintenance.h"
#include "artifacts.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "aimee.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Defaults                                                            */
/* ------------------------------------------------------------------ */

void kb_maintenance_config_defaults(kb_maintenance_config_t *cfg)
{
   if (!cfg)
      return;
   cfg->lambda = 0.005;
   cfg->confidence_floor = 0.10;
   cfg->min_age_days = 7;
   cfg->orphan_prune_days = 90;
   cfg->dry_run = 0;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Elapsed milliseconds since a struct timespec recorded with clock_gettime. */
static long elapsed_ms_since(const struct timespec *start)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   long sec_ms = (long)(now.tv_sec - start->tv_sec) * 1000L;
   long nsec_ms = (long)(now.tv_nsec - start->tv_nsec) / 1000000L;
   return sec_ms + nsec_ms;
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                    */
/* ------------------------------------------------------------------ */

int kb_maintenance_run(const kb_maintenance_config_t *cfg, kb_maintenance_result_t *out)
{
   if (!cfg || !out)
      return -1;

   memset(out, 0, sizeof(*out));

   double lambda = cfg->lambda;
   if (!isfinite(lambda) || lambda < 0.0)
      lambda = 0.005;
   double confidence_floor = cfg->confidence_floor;
   if (!isfinite(confidence_floor) || confidence_floor < 0.0)
      confidence_floor = 0.10;
   if (confidence_floor > 1.0)
      confidence_floor = 1.0;
   int min_age_days = cfg->min_age_days < 0 ? 0 : cfg->min_age_days;
   int orphan_prune_days = cfg->orphan_prune_days < 0 ? 0 : cfg->orphan_prune_days;

   void *conn = db2_conn();
   if (!conn)
   {
      snprintf(out->error, sizeof(out->error), "db2_conn() returned NULL");
      return -1;
   }

   /* Generate a run-id UUID for kb_maintenance_runs. */
   db2_artifact_gen_id(out->run_id, sizeof(out->run_id));

   struct timespec t0;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   char err[256] = "";

   /* ---- BEGIN transaction ---------------------------------------- */
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
   {
      snprintf(out->error, sizeof(out->error), "BEGIN failed: %s", err);
      return -1;
   }

   int rc = 0;

   /* ================================================================
    * Pass 1: Confidence decay
    *
    * UPDATE artifacts SET
    *   confidence    = GREATEST(:floor,
    *                    confidence * EXP(-:lambda *
    *                      EXTRACT(EPOCH FROM (
    *                        now() -
    *                        COALESCE(last_accessed_at::timestamptz,
    *                                 committed_at::timestamptz,
    *                                 created_at::timestamptz)
    *                      )) / 86400.0)),
    *   last_decay_at = pg_now_text()
    * WHERE state = 'committed'
    *   AND confidence > :floor
    *   AND COALESCE(last_accessed_at::timestamptz,
    *                committed_at::timestamptz,
    *                created_at::timestamptz)
    *       < now() - (:min_age_days || ' days')::INTERVAL
    *
    * Note: last_accessed_at and last_decay_at are added by the
    * accompanying migration; this SQL assumes they exist.
    * ================================================================ */

   /* Embed the two scalar int config values into the SQL string (they are
    * caller-controlled internal ints, not user input). */
   char decay_sql[4096];
   if (aimee_pg_is_shim())
   {
      snprintf(decay_sql, sizeof(decay_sql),
               "UPDATE artifacts"
               "   SET confidence = MAX(?1, confidence * EXP(-?2 *"
               "       (julianday('now') - julianday(MAX("
               "          COALESCE(NULLIF(last_decay_at,''),"
               "                   COALESCE(last_accessed_at, NULLIF(committed_at,''),"
               "                            created_at)),"
               "          COALESCE(last_accessed_at, NULLIF(committed_at,''), created_at)"
               "       ))))),"
               "       last_decay_at = datetime('now')"
               " WHERE state = 'committed'"
               "   AND confidence > ?1"
               "   AND julianday(COALESCE(last_accessed_at, NULLIF(committed_at,''),"
               "                         created_at)) < julianday('now') - ?3"
               "   AND MAX(?1, confidence * EXP(-?2 *"
               "       (julianday('now') - julianday(MAX("
               "          COALESCE(NULLIF(last_decay_at,''),"
               "                   COALESCE(last_accessed_at, NULLIF(committed_at,''),"
               "                            created_at)),"
               "          COALESCE(last_accessed_at, NULLIF(committed_at,''), created_at)"
               "       ))))) < confidence - 0.000001");
   }
   else
   {
      const char *access_ts = "COALESCE(last_accessed_at, NULLIF(committed_at,'')::timestamptz,"
                              " created_at::timestamptz)";
      const char *base_ts = "GREATEST(COALESCE(NULLIF(last_decay_at,'')::timestamptz,"
                            " COALESCE(last_accessed_at, NULLIF(committed_at,'')::timestamptz,"
                            " created_at::timestamptz)),"
                            " COALESCE(last_accessed_at, NULLIF(committed_at,'')::timestamptz,"
                            " created_at::timestamptz))";
      snprintf(decay_sql, sizeof(decay_sql),
               "UPDATE artifacts"
               "   SET confidence = GREATEST(?1, confidence * EXP(-?2 *"
               "       EXTRACT(EPOCH FROM (now() - %s)) / 86400.0)),"
               "       last_decay_at = pg_now_text()"
               " WHERE state = 'committed'"
               "   AND confidence > ?1"
               "   AND %s < now() - ('%d days')::INTERVAL"
               "   AND GREATEST(?1, confidence * EXP(-?2 *"
               "       EXTRACT(EPOCH FROM (now() - %s)) / 86400.0))"
               "       < confidence - 0.000001",
               base_ts, access_ts, min_age_days, base_ts);
   }

   aimee_pg_stmt_t *st_decay = aimee_pg_prepare(conn, decay_sql, err, sizeof(err));
   if (!st_decay)
   {
      snprintf(out->error, sizeof(out->error), "decay prepare failed: %s", err);
      rc = -1;
      goto done;
   }

   aimee_pg_bind_double(st_decay, "?1", confidence_floor);
   aimee_pg_bind_double(st_decay, "?2", lambda);
   if (aimee_pg_is_shim())
      aimee_pg_bind_int(st_decay, "?3", min_age_days);

   if (aimee_pg_step(st_decay, err, sizeof(err)) != AIMEE_PG_DONE)
   {
      snprintf(out->error, sizeof(out->error), "decay step failed: %s", err);
      aimee_pg_finalize(st_decay);
      rc = -1;
      goto done;
   }

   out->rows_decayed = aimee_pg_stmt_changes(st_decay);
   aimee_pg_finalize(st_decay);

   /* ================================================================
    * Pass 2: Orphan pruning
    *
    * Mark as 'retired' any committed artifact that:
    *   - has no artifact_citations row pointing to it (not cited by any source)
    *   - has no artifact_links row with from_id = artifact.id (no outbound links)
    *   - confidence is at or below the floor (fully decayed)
    *   - created more than orphan_prune_days ago
    * ================================================================ */

   char orphan_sql[2048];
   if (aimee_pg_is_shim())
   {
      snprintf(orphan_sql, sizeof(orphan_sql),
               "UPDATE artifacts"
               "   SET state = 'retired', retired_at = datetime('now')"
               " WHERE state = 'committed'"
               "   AND confidence <= ?1"
               "   AND julianday(COALESCE(last_accessed_at, NULLIF(committed_at,''),"
               "                         created_at)) < julianday('now') - ?2"
               "   AND NOT EXISTS ("
               "         SELECT 1 FROM artifact_citations ac"
               "          WHERE ac.artifact_id = artifacts.id"
               "       )"
               "   AND NOT EXISTS ("
               "         SELECT 1 FROM artifact_links al"
               "          WHERE al.from_id = artifacts.id OR al.to_id = artifacts.id"
               "       )");
   }
   else
   {
      const char *access_ts = "COALESCE(last_accessed_at, NULLIF(committed_at,'')::timestamptz,"
                              " created_at::timestamptz)";
      snprintf(orphan_sql, sizeof(orphan_sql),
               "UPDATE artifacts"
               "   SET state = 'retired', retired_at = pg_now_text()"
               " WHERE state = 'committed'"
               "   AND confidence <= ?1"
               "   AND %s < now() - ('%d days')::INTERVAL"
               "   AND NOT EXISTS ("
               "         SELECT 1 FROM artifact_citations ac"
               "          WHERE ac.artifact_id = artifacts.id"
               "       )"
               "   AND NOT EXISTS ("
               "         SELECT 1 FROM artifact_links al"
               "          WHERE al.from_id = artifacts.id OR al.to_id = artifacts.id"
               "       )",
               access_ts, orphan_prune_days);
   }

   aimee_pg_stmt_t *st_orphan = aimee_pg_prepare(conn, orphan_sql, err, sizeof(err));
   if (!st_orphan)
   {
      snprintf(out->error, sizeof(out->error), "orphan prepare failed: %s", err);
      rc = -1;
      goto done;
   }

   aimee_pg_bind_double(st_orphan, "?1", confidence_floor);
   if (aimee_pg_is_shim())
      aimee_pg_bind_int(st_orphan, "?2", orphan_prune_days);

   if (aimee_pg_step(st_orphan, err, sizeof(err)) != AIMEE_PG_DONE)
   {
      snprintf(out->error, sizeof(out->error), "orphan step failed: %s", err);
      aimee_pg_finalize(st_orphan);
      rc = -1;
      goto done;
   }

   out->orphans_pruned = aimee_pg_stmt_changes(st_orphan);
   aimee_pg_finalize(st_orphan);

   /* ================================================================
    * Pass 3: Record maintenance run
    *
    * kb_maintenance_runs is created by the accompanying migration.
    * Columns: id, rows_decayed, orphans_pruned, elapsed_ms, lambda,
    *          confidence_floor, dry_run, run_at
    * ================================================================ */
   out->elapsed_ms = elapsed_ms_since(&t0);

   static const char *ins_sql = "INSERT INTO kb_maintenance_runs"
                                "  (id, rows_decayed, orphans_pruned, elapsed_ms,"
                                "   lambda, confidence_floor, dry_run, run_at)"
                                " VALUES"
                                "  (?1, ?2, ?3, ?4, ?5, ?6, ?7, pg_now_text())";

   aimee_pg_stmt_t *st_ins = aimee_pg_prepare(conn, ins_sql, err, sizeof(err));
   if (!st_ins)
   {
      /* Non-fatal — results are still valid even if we can't record the run. */
      snprintf(out->error, sizeof(out->error), "maintenance_runs insert prepare failed: %s", err);
   }
   else
   {
      aimee_pg_bind_text(st_ins, "?1", out->run_id);
      aimee_pg_bind_int(st_ins, "?2", out->rows_decayed);
      aimee_pg_bind_int(st_ins, "?3", out->orphans_pruned);
      aimee_pg_bind_int64(st_ins, "?4", (int64_t)out->elapsed_ms);
      aimee_pg_bind_double(st_ins, "?5", lambda);
      aimee_pg_bind_double(st_ins, "?6", confidence_floor);
      aimee_pg_bind_int(st_ins, "?7", cfg->dry_run);
      if (aimee_pg_step(st_ins, err, sizeof(err)) != AIMEE_PG_DONE)
      {
         /* Non-fatal. */
         snprintf(out->error, sizeof(out->error), "maintenance_runs insert failed: %s", err);
      }
      aimee_pg_finalize(st_ins);
   }

done:
   if (rc != 0)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }

   /* Dry-run: roll back so no state changes persist, but keep counts. */
   if (cfg->dry_run)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   }
   else
   {
      if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      {
         snprintf(out->error, sizeof(out->error), "COMMIT failed: %s", err);
         aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
         return -1;
      }
   }

   out->elapsed_ms = elapsed_ms_since(&t0);
   return 0;
}
