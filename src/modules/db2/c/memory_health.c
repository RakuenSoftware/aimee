/* db2/memory_health.c: memory_health table SQL primitives — Postgres
 * via libpq. */

#include "../headers/aimee.h" /* memory_stats_t + TIER_/KIND_ constants */
#include "memory_health.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MH_ERRBUF 256

void db2_memory_health_record(int total_memories, int contradictions_detected, int promotions,
                              int demotions, int expirations)
{
   void *conn = db2_conn();
   if (!conn)
      return;

   char ts[32];
   db2_now_utc(ts, sizeof(ts));

   static const char *sql = "INSERT INTO memory_health"
                            " (cycle_at, total_memories, contradictions_detected,"
                            "  promotions, demotions, expirations)"
                            " VALUES (?1, ?2, ?3, ?4, ?5, ?6)";
   char err[MH_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", ts);
   aimee_pg_bind_int(st, "?2", total_memories);
   aimee_pg_bind_int(st, "?3", contradictions_detected);
   aimee_pg_bind_int(st, "?4", promotions);
   aimee_pg_bind_int(st, "?5", demotions);
   aimee_pg_bind_int(st, "?6", expirations);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_memory_health_count_memories(void)
{
   return db2_scalar_int("SELECT COUNT(*) FROM memories", 0);
}

int db2_memory_health_count_recent_conflicts(int days)
{
   if (days <= 0)
      days = 1;
   char window[32];
   snprintf(window, sizeof(window), "-%d days", days);
   return db2_scalar_int_text("SELECT COUNT(*) FROM memory_conflicts"
                              " WHERE detected_at >= pg_now_text(?1)",
                              window, 0);
}

int db2_memory_health_prune_old(int days)
{
   if (days <= 0)
      days = 90;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char window[32];
   snprintf(window, sizeof(window), "-%d days", days);

   static const char *sql = "DELETE FROM memory_health WHERE cycle_at < pg_now_text(?1)";
   char err[MH_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", window);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_health_prune_old_contradictions(int days)
{
   if (days <= 0)
      days = 90;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char window[32];
   snprintf(window, sizeof(window), "-%d days", days);

   static const char *sql = "DELETE FROM contradiction_log"
                            " WHERE detected_at < pg_now_text(?1)";
   char err[MH_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", window);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_health_demote_low_effectiveness(double threshold)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char ts[32];
   db2_now_utc(ts, sizeof(ts));

   static const char *sql = "UPDATE memories SET tier = 'L1', updated_at = ?1"
                            " WHERE tier = 'L2'"
                            "   AND effectiveness IS NOT NULL"
                            "   AND effectiveness < ?2";
   char err[MH_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", ts);
   aimee_pg_bind_double(st, "?2", threshold);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_health_effectiveness_stats(double low_threshold, double *avg_effectiveness,
                                          int *low_effectiveness, int *high_impact)
{
   if (avg_effectiveness)
      *avg_effectiveness = 0.0;
   if (low_effectiveness)
      *low_effectiveness = 0;
   if (high_impact)
      *high_impact = 0;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT"
                            " AVG(CASE WHEN effectiveness IS NOT NULL THEN effectiveness END),"
                            " SUM(CASE WHEN effectiveness IS NOT NULL"
                            "             AND effectiveness < ?1 THEN 1 ELSE 0 END),"
                            " SUM(CASE WHEN effectiveness IS NOT NULL"
                            "             AND effectiveness > 0.8 AND use_count >= 10"
                            "          THEN 1 ELSE 0 END)"
                            " FROM memories";
   char err[MH_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_double(st, "?1", low_threshold);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (avg_effectiveness && !aimee_pg_column_is_null(st, 0))
         *avg_effectiveness = aimee_pg_column_double(st, 0);
      if (low_effectiveness)
         *low_effectiveness = aimee_pg_column_int(st, 1);
      if (high_impact)
         *high_impact = aimee_pg_column_int(st, 2);
   }
   aimee_pg_finalize(st);
   return 0;
}

int db2_memory_health_list_l2_memory_ids(int64_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[MH_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT id FROM memories WHERE tier = 'L2'", err, sizeof(err));
   if (!st)
      return 0;
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      out[n++] = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_health_query_counters(int promote_use_count, double promote_confidence,
                                     db2_memory_health_query_counters_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[MH_ERRBUF] = "";

   /* Cycle aggregation over the rolling 7-day health window. */
   {
      static const char *sql = "SELECT COUNT(*),"
                               " COALESCE(SUM(contradictions_detected), 0),"
                               " COALESCE(SUM(promotions), 0),"
                               " COALESCE(SUM(demotions), 0),"
                               " COALESCE(SUM(expirations), 0)"
                               " FROM memory_health"
                               " WHERE cycle_at >= pg_now_text('-7 days')";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            out->cycles = aimee_pg_column_int(st, 0);
            out->total_contradictions = aimee_pg_column_int(st, 1);
            out->total_promotions = aimee_pg_column_int(st, 2);
            out->total_demotions = aimee_pg_column_int(st, 3);
            out->total_expirations = aimee_pg_column_int(st, 4);
         }
         aimee_pg_finalize(st);
      }
   }

   /* New-memory count over the same 7-day window. */
   {
      static const char *sql = "SELECT COUNT(*) FROM memories"
                               " WHERE created_at >= pg_now_text('-7 days')";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
            out->new_memories = aimee_pg_column_int(st, 0);
         aimee_pg_finalize(st);
      }
   }

   /* L1 rows that currently meet the kind-default promotion thresholds. */
   {
      static const char *sql = "SELECT COUNT(*) FROM memories"
                               " WHERE tier = 'L1'"
                               "   AND (use_count >= ?1 OR confidence >= ?2)";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int(st, "?1", promote_use_count);
         aimee_pg_bind_double(st, "?2", promote_confidence);
         if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
            out->l1_eligible = aimee_pg_column_int(st, 0);
         aimee_pg_finalize(st);
      }
   }

   /* L2 totals + stale (no last_used_at OR last_used_at < 30 days ago). */
   {
      static const char *sql = "SELECT COUNT(*),"
                               " SUM(CASE WHEN last_used_at IS NULL OR last_used_at = ''"
                               "          OR last_used_at < pg_now_text('-30 days')"
                               "          THEN 1 ELSE 0 END)"
                               " FROM memories WHERE tier = 'L2'";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            out->l2_total = aimee_pg_column_int(st, 0);
            out->l2_stale_30_days = aimee_pg_column_int(st, 1);
         }
         aimee_pg_finalize(st);
      }
   }
   return 0;
}

static int mh_update_effectiveness(int64_t memory_id, int has_value, double value)
{
   if (memory_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "UPDATE memories SET effectiveness = ?1 WHERE id = ?2";
   char err[MH_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   if (has_value)
      aimee_pg_bind_double(st, "?1", value);
   else
      aimee_pg_bind_null(st, "?1");
   aimee_pg_bind_int64(st, "?2", memory_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_memory_health_set_effectiveness(int64_t memory_id, double value)
{
   return mh_update_effectiveness(memory_id, 1, value);
}

int db2_memory_health_clear_effectiveness(int64_t memory_id)
{
   return mh_update_effectiveness(memory_id, 0, 0.0);
}

int db2_memory_health_delete_by_sensitivity(const char *sensitivity, int days)
{
   if (!sensitivity || !*sensitivity || days <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char window[32];
   snprintf(window, sizeof(window), "-%d days", days);

   static const char *sql = "DELETE FROM memories"
                            " WHERE sensitivity = ?1"
                            "   AND created_at < pg_now_text(?2)";
   char err[MH_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", sensitivity);
   aimee_pg_bind_text(st, "?2", window);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_stats_counts(memory_stats_t *out)
{
   if (!out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[MH_ERRBUF] = "";

   /* Per-tier counts */
   {
      static const char *sql = "SELECT tier, COUNT(*) FROM memories GROUP BY tier";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            const char *t = aimee_pg_column_text(st, 0);
            int c = aimee_pg_column_int(st, 1);
            if (!t)
               continue;
            if (strcmp(t, TIER_L0) == 0)
               out->tier_counts[0] = c;
            else if (strcmp(t, TIER_L1) == 0)
               out->tier_counts[1] = c;
            else if (strcmp(t, TIER_L2) == 0)
               out->tier_counts[2] = c;
            else if (strcmp(t, TIER_L3) == 0)
               out->tier_counts[3] = c;
            else if (strcmp(t, TIER_L4) == 0)
               out->tier_counts[4] = c;
            else if (strcmp(t, TIER_L5) == 0)
               out->tier_counts[5] = c;
            out->total += c;
         }
         aimee_pg_finalize(st);
      }
   }

   /* Per-kind counts */
   {
      static const char *sql = "SELECT kind, COUNT(*) FROM memories GROUP BY kind";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            const char *k = aimee_pg_column_text(st, 0);
            int c = aimee_pg_column_int(st, 1);
            if (!k)
               continue;
            if (strcmp(k, KIND_FACT) == 0)
               out->kind_counts[0] = c;
            else if (strcmp(k, KIND_PREFERENCE) == 0)
               out->kind_counts[1] = c;
            else if (strcmp(k, KIND_DECISION) == 0)
               out->kind_counts[2] = c;
            else if (strcmp(k, KIND_EPISODE) == 0)
               out->kind_counts[3] = c;
            else if (strcmp(k, KIND_TASK) == 0)
               out->kind_counts[4] = c;
            else if (strcmp(k, KIND_SCRATCH) == 0)
               out->kind_counts[5] = c;
            else if (strcmp(k, KIND_PROCEDURE) == 0)
               out->kind_counts[6] = c;
            else if (strcmp(k, KIND_POLICY) == 0)
               out->kind_counts[7] = c;
            else if (strcmp(k, KIND_WORKFLOW) == 0)
               out->kind_counts[8] = c;
         }
         aimee_pg_finalize(st);
      }
   }

   /* Unresolved conflict count */
   {
      static const char *sql = "SELECT COUNT(*) FROM memory_conflicts WHERE resolved = 0";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
            out->conflicts = aimee_pg_column_int(st, 0);
         aimee_pg_finalize(st);
      }
   }
   return 0;
}
