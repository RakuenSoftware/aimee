/* cross_repo_review.c: S4b — the AMBIGUOUS cross-repo review queue + adjudication
 * (§3.8). Portable SQL over db2 (Postgres + sqlite shim). See cross_repo_review.h. */

#include "cross_repo_review.h"

#include "aimee.h"
#include "db2.h"
#include "db2_internal.h" /* db2_now_utc */
#include "db_postgres.h"
#include "log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CRR_TAG "cross_repo"
#define CRR_ERR 256

static void *crr_conn(void)
{
   return db2_conn();
}

int db2_cross_repo_review_upsert(const char *repo_set_hash, const char *symbol,
                                 const char *caller_repo, const char *candidate_definer,
                                 const char *evidence_json, double evidence_score,
                                 const char *review_class, int cross_lang, int queue_max)
{
   void *conn = crr_conn();
   if (!conn || !symbol || !caller_repo)
      return -1;
   if (!isfinite(evidence_score)) /* NaN/Inf would defeat ASC eviction / DESC list ordering */
      evidence_score = 0.0;
   char err[CRR_ERR] = "";
   char ts[32];
   db2_now_utc(ts, sizeof(ts));

   /* No separate arrival_seq counter (a MAX+1 read-then-write would race): the
    * row's DB-assigned monotonic `id` is the FIFO tie-break — atomic and preserved
    * across ON CONFLICT upserts. arrival_seq keeps its column default (unused). */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO cross_repo_review_queue (repo_set_hash, symbol, caller_repo, "
       "candidate_definer, evidence, evidence_score, status, review_class, cross_lang, updated_at) "
       "VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'open', ?7, ?8, ?9) "
       "ON CONFLICT (repo_set_hash, symbol, caller_repo, candidate_definer) DO UPDATE SET "
       "evidence = EXCLUDED.evidence, evidence_score = EXCLUDED.evidence_score, "
       "review_class = EXCLUDED.review_class, cross_lang = EXCLUDED.cross_lang, updated_at = ?9",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", repo_set_hash ? repo_set_hash : "");
   aimee_pg_bind_text(st, "?2", symbol);
   aimee_pg_bind_text(st, "?3", caller_repo);
   aimee_pg_bind_text(st, "?4", candidate_definer ? candidate_definer : "");
   aimee_pg_bind_text(st, "?5", evidence_json ? evidence_json : "{}");
   aimee_pg_bind_double(st, "?6", evidence_score);
   aimee_pg_bind_text(st, "?7", (review_class && review_class[0]) ? review_class : "ambiguous");
   aimee_pg_bind_int(st, "?8", cross_lang ? 1 : 0);
   aimee_pg_bind_text(st, "?9", ts);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc < 0)
   {
      LOG_ERROR(CRR_TAG, "review upsert failed: %s", err);
      return -1;
   }

   /* Overflow eviction: drop the lowest (evidence_score, arrival_seq) 'open' rows
    * beyond queue_max, accounting the drops (never silent). */
   if (queue_max > 0)
   {
      int64_t cnt = 0;
      aimee_pg_stmt_t *c = aimee_pg_prepare(
          conn, "SELECT COUNT(*) FROM cross_repo_review_queue WHERE status = 'open'", err,
          sizeof(err));
      if (c)
      {
         if (aimee_pg_step(c, err, sizeof(err)) == AIMEE_PG_ROW)
            cnt = aimee_pg_column_int64(c, 0);
         aimee_pg_finalize(c);
      }
      if (cnt > queue_max)
      {
         int excess = (int)(cnt - queue_max);
         aimee_pg_stmt_t *d =
             aimee_pg_prepare(conn,
                              "DELETE FROM cross_repo_review_queue WHERE id IN ("
                              "  SELECT id FROM cross_repo_review_queue WHERE status = 'open' "
                              "  ORDER BY evidence_score ASC, id ASC LIMIT ?1)",
                              err, sizeof(err));
         if (d)
         {
            aimee_pg_bind_int(d, "?1", excess);
            (void)aimee_pg_step(d, err, sizeof(err));
            aimee_pg_finalize(d);
         }
         aimee_pg_stmt_t *u = aimee_pg_prepare(
             conn,
             "UPDATE cross_repo_meta SET review_overflow_dropped = review_overflow_dropped + ?1 "
             "WHERE id = 1",
             err, sizeof(err));
         if (u)
         {
            aimee_pg_bind_int(u, "?1", excess);
            (void)aimee_pg_step(u, err, sizeof(err));
            aimee_pg_finalize(u);
         }
         LOG_WARN(CRR_TAG, "review queue overflow: evicted %d lowest-evidence entries (cap %d)",
                  excess, queue_max);
      }
   }
   return 0;
}

int db2_cross_repo_review_list(const char *caller_repo, const char *status, xrepo_review_row_t *out,
                               int max, int64_t *overflow_dropped)
{
   void *conn = crr_conn();
   if (!conn || !out || max <= 0)
      return -1;
   const char *st_filter = (status && status[0]) ? status : "open";
   int by_caller = caller_repo && caller_repo[0];
   char err[CRR_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       by_caller
           ? "SELECT id, symbol, caller_repo, candidate_definer, review_class, evidence_score, "
             "cross_lang, status, evidence FROM cross_repo_review_queue "
             "WHERE status = ?1 AND caller_repo = ?2 "
             "ORDER BY evidence_score DESC, id DESC LIMIT ?3"
           : "SELECT id, symbol, caller_repo, candidate_definer, review_class, evidence_score, "
             "cross_lang, status, evidence FROM cross_repo_review_queue WHERE status = ?1 "
             "ORDER BY evidence_score DESC, id DESC LIMIT ?2",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", st_filter);
   if (by_caller)
   {
      aimee_pg_bind_text(st, "?2", caller_repo);
      aimee_pg_bind_int(st, "?3", max);
   }
   else
      aimee_pg_bind_int(st, "?2", max);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      xrepo_review_row_t *r = &out[n];
      memset(r, 0, sizeof(*r));
      r->id = aimee_pg_column_int64(st, 0);
      snprintf(r->symbol, sizeof(r->symbol), "%s", aimee_pg_column_text(st, 1));
      snprintf(r->caller_repo, sizeof(r->caller_repo), "%s", aimee_pg_column_text(st, 2));
      snprintf(r->candidate_definer, sizeof(r->candidate_definer), "%s",
               aimee_pg_column_text(st, 3));
      snprintf(r->review_class, sizeof(r->review_class), "%s", aimee_pg_column_text(st, 4));
      r->evidence_score = aimee_pg_column_double(st, 5);
      r->cross_lang = aimee_pg_column_int(st, 6);
      snprintf(r->status, sizeof(r->status), "%s", aimee_pg_column_text(st, 7));
      snprintf(r->evidence, sizeof(r->evidence), "%s", aimee_pg_column_text(st, 8));
      n++;
   }
   aimee_pg_finalize(st);

   if (overflow_dropped)
   {
      *overflow_dropped = 0;
      aimee_pg_stmt_t *m =
          aimee_pg_prepare(conn, "SELECT review_overflow_dropped FROM cross_repo_meta WHERE id = 1",
                           err, sizeof(err));
      if (m)
      {
         if (aimee_pg_step(m, err, sizeof(err)) == AIMEE_PG_ROW)
            *overflow_dropped = aimee_pg_column_int64(m, 0);
         aimee_pg_finalize(m);
      }
   }
   return n;
}

int db2_cross_repo_review_set_status(int64_t id, const char *status)
{
   void *conn = crr_conn();
   if (!conn || !status)
      return -1;
   if (strcmp(status, "accepted") != 0 && strcmp(status, "rejected") != 0)
      return -1;
   char err[CRR_ERR] = "";
   char ts[32];
   db2_now_utc(ts, sizeof(ts));
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE cross_repo_review_queue SET status = ?1, updated_at = ?2 WHERE id = ?3", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", status);
   aimee_pg_bind_text(st, "?2", ts);
   aimee_pg_bind_int64(st, "?3", id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return rc < 0 ? -1 : 0;
}
