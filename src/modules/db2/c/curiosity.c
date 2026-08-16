/* db2/curiosity.c: curiosity backlog — Postgres via libpq.
 *
 * Implements the typed db2_curiosity_* API declared in curiosity.h. */

#include "curiosity.h"
#include "util.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CUR_ERRBUF 256

static const char *CANONICAL_GAP_TYPES[] = {
    CURIOSITY_GAP_MISSING_FACT,  CURIOSITY_GAP_CONTRADICTION,         CURIOSITY_GAP_STALE_FACT,
    CURIOSITY_GAP_WEAK_COVERAGE, CURIOSITY_GAP_UNVERIFIED_ASSUMPTION, NULL};

static const char *CANONICAL_STATES[] = {CURIOSITY_STATE_OPEN, CURIOSITY_STATE_IN_PROGRESS,
                                         CURIOSITY_STATE_RESOLVED, CURIOSITY_STATE_SUPPRESSED,
                                         NULL};

int db2_curiosity_gap_type_is_canonical(const char *gap_type)
{
   if (!gap_type || !gap_type[0])
      return 0;
   for (int i = 0; CANONICAL_GAP_TYPES[i]; i++)
      if (strcmp(CANONICAL_GAP_TYPES[i], gap_type) == 0)
         return 1;
   return 0;
}

int db2_curiosity_state_is_valid(const char *state)
{
   if (!state || !state[0])
      return 0;
   for (int i = 0; CANONICAL_STATES[i]; i++)
      if (strcmp(CANONICAL_STATES[i], state) == 0)
         return 1;
   return 0;
}

#define CURIOSITY_SELECT_COLS                                                                      \
   "id, gap_type, target_entity, target_topic, evidence, importance, novelty, progress, "          \
   "routing_score, state, source_session, created_at, updated_at"

static void row_to_item_pg(aimee_pg_stmt_t *st, curiosity_item_t *out)
{
   memset(out, 0, sizeof(*out));
   out->id = aimee_pg_column_int64(st, 0);
   db2_copy_col_text(out->gap_type, sizeof(out->gap_type), st, 1);
   db2_copy_col_text(out->target_entity, sizeof(out->target_entity), st, 2);
   db2_copy_col_text(out->target_topic, sizeof(out->target_topic), st, 3);
   db2_copy_col_text(out->evidence, sizeof(out->evidence), st, 4);
   out->importance = aimee_pg_column_double(st, 5);
   out->novelty = aimee_pg_column_double(st, 6);
   out->progress = aimee_pg_column_double(st, 7);
   out->routing_score = aimee_pg_column_double(st, 8);
   db2_copy_col_text(out->state, sizeof(out->state), st, 9);
   db2_copy_col_text(out->source_session, sizeof(out->source_session), st, 10);
   db2_copy_col_text(out->created_at, sizeof(out->created_at), st, 11);
   db2_copy_col_text(out->updated_at, sizeof(out->updated_at), st, 12);
}

int db2_curiosity_create(const char *gap_type, const char *target_entity, const char *target_topic,
                         const char *evidence, double importance, double novelty,
                         const char *source_session, curiosity_item_t *out)
{
   void *conn = db2_conn();
   if (!conn || !gap_type || !gap_type[0])
      return -1;
   if (!db2_curiosity_gap_type_is_canonical(gap_type))
      return -1;

   /* The partial unique index on (gap_type='missing_fact', target_topic,
    * state='open') handles dedup for the most common auto-populated
    * case; other callers may create duplicates deliberately. */
   const char *sql = "INSERT INTO curiosity_items"
                     " (gap_type, target_entity, target_topic, evidence, importance,"
                     "  novelty, state, source_session, created_at, updated_at)"
                     " VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'open', ?7, ?8, ?9) RETURNING id";
   char err[CUR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   char ts[32];
   db2_now_utc(ts, sizeof(ts));
   aimee_pg_bind_text(st, "?1", gap_type);
   aimee_pg_bind_text(st, "?2", target_entity ? target_entity : "");
   aimee_pg_bind_text(st, "?3", target_topic ? target_topic : "");
   aimee_pg_bind_text(st, "?4", evidence ? evidence : "");
   aimee_pg_bind_double(st, "?5", importance);
   aimee_pg_bind_double(st, "?6", novelty);
   aimee_pg_bind_text(st, "?7", source_session ? source_session : "");
   aimee_pg_bind_text(st, "?8", ts);
   aimee_pg_bind_text(st, "?9", ts);

   int64_t new_id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      new_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   if (new_id < 0)
      return -1;

   if (out)
      return db2_curiosity_get(new_id, out) == 1 ? 0 : -1;
   return 0;
}

int db2_curiosity_list(const char *state, curiosity_item_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out || max <= 0)
      return 0;

   char err[CUR_ERRBUF] = "";
   aimee_pg_stmt_t *st = NULL;
   if (state && state[0])
   {
      const char *sql = "SELECT " CURIOSITY_SELECT_COLS " FROM curiosity_items"
                        " WHERE state = ?1 ORDER BY created_at DESC, id DESC LIMIT ?2";
      st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_text(st, "?1", state);
      aimee_pg_bind_int(st, "?2", max);
   }
   else
   {
      const char *sql = "SELECT " CURIOSITY_SELECT_COLS " FROM curiosity_items"
                        " ORDER BY created_at DESC, id DESC LIMIT ?1";
      st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_int(st, "?1", max);
   }

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      row_to_item_pg(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_curiosity_list_top_open_by_score(curiosity_item_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out || max <= 0)
      return 0;

   const char *sql = "SELECT " CURIOSITY_SELECT_COLS " FROM curiosity_items"
                     " WHERE state = 'open' ORDER BY routing_score DESC, created_at ASC LIMIT ?1";
   char err[CUR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      row_to_item_pg(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_curiosity_get(int64_t id, curiosity_item_t *out)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return -1;

   const char *sql = "SELECT " CURIOSITY_SELECT_COLS " FROM curiosity_items WHERE id = ?1";
   char err[CUR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   if (rc == AIMEE_PG_ROW)
   {
      row_to_item_pg(st, out);
      aimee_pg_finalize(st);
      return 1;
   }
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int db2_curiosity_update_state(int64_t id, const char *new_state)
{
   void *conn = db2_conn();
   if (!conn || !db2_curiosity_state_is_valid(new_state))
      return -1;

   const char *sql = "UPDATE curiosity_items SET state = ?1, updated_at = ?2 WHERE id = ?3";
   char err[CUR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   char ts[32];
   db2_now_utc(ts, sizeof(ts));
   aimee_pg_bind_text(st, "?1", new_state);
   aimee_pg_bind_text(st, "?2", ts);
   aimee_pg_bind_int64(st, "?3", id);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_curiosity_sweep_failed_queries(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   /* Materialize the SELECT result so we don't have a SELECT and the
    * inner db2_curiosity_create INSERT pending on the same connection
    * at the same time. */
   const char *select_sql =
       "SELECT query_norm FROM failed_queries"
       " WHERE query_norm NOT IN (SELECT target_topic FROM curiosity_items"
       "                          WHERE gap_type = 'missing_fact' AND state = 'open')";
   char err[CUR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, select_sql, err, sizeof(err));
   if (!st)
      return 0;

   /* Buffer query_norm strings; bounded by failed_queries size. */
   size_t cap = 16, count = 0;
   char **items = (char **)calloc(cap, sizeof(char *));
   if (!items)
   {
      aimee_pg_finalize(st);
      return 0;
   }
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *qn = aimee_pg_column_text(st, 0);
      if (!qn || !qn[0])
         continue;
      if (count == cap)
      {
         size_t ncap = cap * 2;
         char **ni = (char **)realloc(items, ncap * sizeof(char *));
         if (!ni)
            break;
         items = ni;
         cap = ncap;
      }
      items[count] = strdup(qn);
      if (!items[count])
         break;
      count++;
   }
   aimee_pg_finalize(st);

   int created = 0;
   for (size_t i = 0; i < count; i++)
   {
      if (db2_curiosity_create(CURIOSITY_GAP_MISSING_FACT, "", items[i],
                               "auto-populated from failed_queries", 0.0, 0.0, "", NULL) == 0)
         created++;
      free(items[i]);
   }
   free(items);
   return created;
}

int db2_curiosity_reset(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[CUR_ERRBUF] = "";
   if (aimee_pg_exec(conn, "DELETE FROM curiosity_items", err, sizeof(err)) != 0)
      return -1;
   return 0;
}

/* --- Rescore helpers --------------------------------------------------------
 *
 * These reach DB2 `memories` for a coarse project/org-level novelty proxy. */

static double clamp01(double v)
{
   if (v < 0.0)
      return 0.0;
   if (v > 1.0)
      return 1.0;
   return v;
}

static double curiosity_base_weight(const char *gap_type)
{
   if (!gap_type)
      return 0.0;
   if (strcmp(gap_type, CURIOSITY_GAP_CONTRADICTION) == 0)
      return 0.80;
   if (strcmp(gap_type, CURIOSITY_GAP_UNVERIFIED_ASSUMPTION) == 0)
      return 0.60;
   if (strcmp(gap_type, CURIOSITY_GAP_MISSING_FACT) == 0)
      return 0.50;
   if (strcmp(gap_type, CURIOSITY_GAP_WEAK_COVERAGE) == 0)
      return 0.40;
   if (strcmp(gap_type, CURIOSITY_GAP_STALE_FACT) == 0)
      return 0.30;
   return 0.10;
}

static int memory_coverage_count(void *conn, const char *target)
{
   if (!conn || !target || !target[0])
      return 0;
   const char *sql = "SELECT COUNT(*) FROM memories"
                     " WHERE merged_into = 0"
                     "   AND (LOWER(key) LIKE ?1 OR LOWER(content) LIKE ?2)";
   char err[CUR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   char like[288];
   snprintf(like, sizeof(like), "%%%s%%", target);
   for (size_t i = 0; like[i]; i++)
      if (like[i] >= 'A' && like[i] <= 'Z')
         like[i] = (char)(like[i] + ('a' - 'A'));
   aimee_pg_bind_text(st, "?1", like);
   aimee_pg_bind_text(st, "?2", like);
   int count = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      count = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return count;
}

static int memory_total_count(void *conn)
{
   if (!conn)
      return 0;
   char err[CUR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT COUNT(*) FROM memories WHERE merged_into = 0", err, sizeof(err));
   if (!st)
      return 0;
   int count = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      count = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return count;
}

static double age_days(const char *updated_at)
{
   time_t then = str_parse_iso8601(updated_at);
   if (then == (time_t)0)
      return 0.0;
   double age_seconds = difftime(time(NULL), then);
   if (age_seconds < 0.0)
      age_seconds = 0.0;
   return age_seconds / 86400.0;
}

/* Materialized open / in_progress curiosity row used by rescore_all so the
 * UPDATE pass doesn't run while a SELECT is still streaming. */
typedef struct
{
   int64_t id;
   char gap_type[64];
   char target_entity[256];
   char target_topic[256];
   int has_evidence;
   char state[32];
   char updated_at[32];
} curiosity_rescore_row_t;

int db2_curiosity_rescore_all(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   int total = memory_total_count(conn);
   double maturity = clamp01(log10((double)total + 1.0) / 4.0);

   /* Materialize the SELECT before issuing UPDATEs and the per-row
    * memory_coverage_count() probes, so we don't keep two prepared
    * statements active on the connection at once. */
   curiosity_rescore_row_t *rows = NULL;
   size_t cap = 0, count = 0;
   {
      const char *select_sql =
          "SELECT id, gap_type, target_entity, target_topic, evidence, state, updated_at"
          " FROM curiosity_items WHERE state IN ('open', 'in_progress')";
      char err[CUR_ERRBUF] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, select_sql, err, sizeof(err));
      if (!st)
         return 0;
      while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         if (count == cap)
         {
            size_t ncap = cap ? cap * 2 : 16;
            curiosity_rescore_row_t *nrows =
                (curiosity_rescore_row_t *)realloc(rows, ncap * sizeof(curiosity_rescore_row_t));
            if (!nrows)
               break;
            rows = nrows;
            cap = ncap;
         }
         curiosity_rescore_row_t *r = &rows[count++];
         memset(r, 0, sizeof(*r));
         r->id = aimee_pg_column_int64(st, 0);
         const char *s;
         s = aimee_pg_column_text(st, 1);
         snprintf(r->gap_type, sizeof(r->gap_type), "%s", s ? s : "");
         s = aimee_pg_column_text(st, 2);
         snprintf(r->target_entity, sizeof(r->target_entity), "%s", s ? s : "");
         s = aimee_pg_column_text(st, 3);
         snprintf(r->target_topic, sizeof(r->target_topic), "%s", s ? s : "");
         s = aimee_pg_column_text(st, 4);
         r->has_evidence = (s && s[0]) ? 1 : 0;
         s = aimee_pg_column_text(st, 5);
         snprintf(r->state, sizeof(r->state), "%s", s ? s : "");
         s = aimee_pg_column_text(st, 6);
         snprintf(r->updated_at, sizeof(r->updated_at), "%s", s ? s : "");
      }
      aimee_pg_finalize(st);
   }

   const char *update_sql = "UPDATE curiosity_items SET importance = ?1, novelty = ?2,"
                            " progress = ?3, routing_score = ?4,"
                            " updated_at = updated_at WHERE id = ?5";

   int rescored = 0;
   for (size_t i = 0; i < count; i++)
   {
      curiosity_rescore_row_t *r = &rows[i];
      double importance = curiosity_base_weight(r->gap_type);
      if (r->has_evidence)
         importance *= 1.15;
      importance = clamp01(importance);

      const char *needle =
          r->target_topic[0] ? r->target_topic : (r->target_entity[0] ? r->target_entity : "");
      int coverage = memory_coverage_count(conn, needle);
      double novelty = 1.0 / (1.0 + (double)coverage);

      double progress = 0.0;
      if (strcmp(r->state, "resolved") == 0)
         progress = 1.0;
      else if (r->updated_at[0])
         progress = exp(-0.693147 * age_days(r->updated_at) / 30.0);
      progress = clamp01(progress);

      double stale_boost = 0.2 * (1.0 - progress);
      double contradiction_boost =
          (strcmp(r->gap_type, CURIOSITY_GAP_CONTRADICTION) == 0) ? 0.10 : 0.0;
      double routing_score = (1.0 - 0.6 * maturity) * importance + (0.6 * maturity) * novelty +
                             stale_boost + contradiction_boost;
      routing_score = clamp01(routing_score);

      char err[CUR_ERRBUF] = "";
      aimee_pg_stmt_t *upd = aimee_pg_prepare(conn, update_sql, err, sizeof(err));
      if (!upd)
         continue;
      aimee_pg_bind_double(upd, "?1", importance);
      aimee_pg_bind_double(upd, "?2", novelty);
      aimee_pg_bind_double(upd, "?3", progress);
      aimee_pg_bind_double(upd, "?4", routing_score);
      aimee_pg_bind_int64(upd, "?5", r->id);
      if (aimee_pg_step(upd, err, sizeof(err)) == AIMEE_PG_DONE)
         rescored++;
      aimee_pg_finalize(upd);
   }
   free(rows);
   return rescored;
}

int db2_curiosity_promote_corpus_gap(const char *artifact_id, const char *gap_kind,
                                     const char *subject, const char *evidence_ref)
{
   if (!artifact_id || !gap_kind || !subject)
      return -1;

   const char *curiosity_gap_type;
   if (strcmp(gap_kind, "undefined_entity") == 0)
      curiosity_gap_type = CURIOSITY_GAP_MISSING_FACT;
   else if (strcmp(gap_kind, "dangling_reference") == 0)
      curiosity_gap_type = CURIOSITY_GAP_WEAK_COVERAGE;
   else
      curiosity_gap_type = CURIOSITY_GAP_MISSING_FACT;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Skip if an open/in_progress item already exists for this subject. */
   char err[CUR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT COUNT(*) FROM curiosity_items"
                                          " WHERE target_entity = ?1"
                                          "   AND state NOT IN ('resolved','suppressed')",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", subject);
   int already = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      already = aimee_pg_column_int(st, 0) > 0;
   aimee_pg_finalize(st);
   if (already)
      return 0;

   return db2_curiosity_create(curiosity_gap_type, subject, subject,
                               evidence_ref ? evidence_ref : artifact_id, 0.5, 0.7, "corpus.gaps",
                               NULL);
}
