/* db2/approach_failures.c: approach-level negative knowledge — Postgres via libpq. */

#include "approach_failures.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

#define AF_ERRBUF 256

static void af_copy(char *dst, size_t dstsz, const char *src)
{
   snprintf(dst, dstsz, "%s", src ? src : "");
}

/* Column order shared by every SELECT below. */
#define AF_COLUMNS                                                                                 \
   "id, goal_signature, goal_text, goal_tokens, approach_signature, approach_text,"                \
   " failure_mode, source, source_ref, occurrences, created_at, updated_at"

static void af_load_row(aimee_pg_stmt_t *st, approach_failure_t *out)
{
   memset(out, 0, sizeof(*out));
   out->id = aimee_pg_column_int64(st, 0);
   af_copy(out->goal_signature, sizeof(out->goal_signature), aimee_pg_column_text(st, 1));
   af_copy(out->goal_text, sizeof(out->goal_text), aimee_pg_column_text(st, 2));
   af_copy(out->goal_tokens, sizeof(out->goal_tokens), aimee_pg_column_text(st, 3));
   af_copy(out->approach_signature, sizeof(out->approach_signature), aimee_pg_column_text(st, 4));
   af_copy(out->approach_text, sizeof(out->approach_text), aimee_pg_column_text(st, 5));
   af_copy(out->failure_mode, sizeof(out->failure_mode), aimee_pg_column_text(st, 6));
   af_copy(out->source, sizeof(out->source), aimee_pg_column_text(st, 7));
   af_copy(out->source_ref, sizeof(out->source_ref), aimee_pg_column_text(st, 8));
   out->occurrences = aimee_pg_column_int64(st, 9);
   af_copy(out->created_at, sizeof(out->created_at), aimee_pg_column_text(st, 10));
   af_copy(out->updated_at, sizeof(out->updated_at), aimee_pg_column_text(st, 11));
}

int db2_approach_failure_record(const char *goal_signature, const char *goal_text,
                                const char *goal_tokens, const char *approach_signature,
                                const char *approach_text, const char *failure_mode,
                                const char *source, const char *source_ref)
{
   if (!goal_signature || !goal_signature[0] || !approach_signature || !approach_signature[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return DB2_APPROACH_UNAVAILABLE;

   /* The (goal, approach) pair is the identity: seeing the same dead end again
    * is more evidence for one row, not a second row. */
   static const char *sql =
       "INSERT INTO approach_failures (goal_signature, goal_text, goal_tokens,"
       " approach_signature, approach_text, failure_mode, source, source_ref, occurrences)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 1)"
       " ON CONFLICT (goal_signature, approach_signature) DO UPDATE SET"
       " occurrences = approach_failures.occurrences + 1,"
       " failure_mode = ?9,"
       " updated_at = pg_now_text()";
   char err[AF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", goal_signature);
   aimee_pg_bind_text(st, "?2", goal_text ? goal_text : "");
   aimee_pg_bind_text(st, "?3", goal_tokens ? goal_tokens : "");
   aimee_pg_bind_text(st, "?4", approach_signature);
   aimee_pg_bind_text(st, "?5", approach_text ? approach_text : "");
   aimee_pg_bind_text(st, "?6", failure_mode ? failure_mode : "");
   aimee_pg_bind_text(st, "?7", source ? source : "");
   aimee_pg_bind_text(st, "?8", source_ref ? source_ref : "");
   aimee_pg_bind_text(st, "?9", failure_mode ? failure_mode : "");
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_approach_failure_candidates(const char *must_contain, approach_failure_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return DB2_APPROACH_UNAVAILABLE;

   const char *needle = (must_contain && must_contain[0]) ? must_contain : "";
   char like[APPROACH_TOKENS_LEN + 8];
   snprintf(like, sizeof(like), "%%%s%%", needle);

   static const char *sql = "SELECT " AF_COLUMNS " FROM approach_failures"
                            " WHERE (?1 = '' OR goal_tokens LIKE ?2)"
                            " ORDER BY occurrences DESC, updated_at DESC, id DESC"
                            " LIMIT ?3";
   char err[AF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", needle);
   aimee_pg_bind_text(st, "?2", like);
   aimee_pg_bind_int(st, "?3", max);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      af_load_row(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_approach_failure_get(const char *goal_signature, const char *approach_signature,
                             approach_failure_t *out)
{
   if (!goal_signature || !approach_signature || !out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT " AF_COLUMNS " FROM approach_failures"
                            " WHERE goal_signature = ?1 AND approach_signature = ?2";
   char err[AF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", goal_signature);
   aimee_pg_bind_text(st, "?2", approach_signature);
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      af_load_row(st, out);
      found = 1;
   }
   aimee_pg_finalize(st);
   return found;
}

int64_t db2_approach_failure_count(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT COUNT(*) FROM approach_failures";
   char err[AF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   int64_t n = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}
