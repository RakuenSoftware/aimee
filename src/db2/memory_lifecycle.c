/* db2/memory_lifecycle.c: memory lifecycle SQL primitives — Postgres via libpq. */

#include "memory_lifecycle.h"
#include "memory_scope_query.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define ML_ERRBUF            256
#define ML_CONFLICT_A_FILTER DB2_MEMORY_SCOPE_FILTER_SQL("ma.id")
#define ML_CONFLICT_B_FILTER DB2_MEMORY_SCOPE_FILTER_SQL("mb.id")
#define ML_CONFLICT_A_RANK   DB2_MEMORY_SCOPE_RANK_SQL("ma.id")
#define ML_CONFLICT_B_RANK   DB2_MEMORY_SCOPE_RANK_SQL("mb.id")

int db2_memory_lifecycle_get_state(int64_t memory_id, char *out, size_t out_len)
{
   if (memory_id <= 0 || !out || out_len == 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[ML_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT lifecycle_state FROM memories WHERE id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_copy_text(out, out_len, aimee_pg_column_text(st, 0));
      if (out[0] == '\0')
         snprintf(out, out_len, "active");
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_memory_lifecycle_update_state(int64_t memory_id, const char *new_state,
                                      const char *archive_reason)
{
   if (memory_id <= 0 || !new_state || !*new_state)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   const char *reason = (archive_reason && archive_reason[0]) ? archive_reason : "";

   /* Same UPDATE shape as the legacy memory_transition_lifecycle: leaving
    * `pending` clears ttl_at; entering `archived` records the reason.
    *
    * Entering `superseded` or `archived` additionally CLOSES the row's event-time
    * interval by stamping valid_until.
    *
    * Why: lifecycle_state answers "is this true NOW", and that is all it can
    * answer. It cannot answer "what did we believe on 12 June" -- a superseded
    * row looks identically superseded whether it stopped being true yesterday or
    * last year. memory_relations already carries valid_at/invalid_at and has a
    * working --as-of read path; memories carried valid_from/valid_until columns
    * that supersession never populated, so the same question about a PREFERENCE
    * or a DECISION -- exactly the facts most likely to change -- had no answer.
    *
    * Only set when currently empty, so a valid_until asserted by a caller who
    * knows the real end date is never overwritten by the transition timestamp.
    * The stamp is when we LEARNED it stopped being true, which is the honest
    * value available here; a caller with better information should say so. */
   static const char *sql =
       "UPDATE memories SET lifecycle_state = ?1,"
       " ttl_at = CASE WHEN ?2 = 'pending' THEN ttl_at ELSE '' END,"
       " archive_reason = CASE WHEN ?3 = 'archived' THEN ?4 ELSE archive_reason END,"
       " valid_until = CASE WHEN ?6 IN ('superseded','archived') AND"
       "                         COALESCE(valid_until,'') = ''"
       "                    THEN pg_now_text() ELSE valid_until END,"
       " updated_at = pg_now_text() WHERE id = ?5";
   char err[ML_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", new_state);
   aimee_pg_bind_text(st, "?2", new_state);
   aimee_pg_bind_text(st, "?3", new_state);
   aimee_pg_bind_text(st, "?4", reason);
   aimee_pg_bind_int64(st, "?5", memory_id);
   aimee_pg_bind_text(st, "?6", new_state);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_memory_valid_at(int64_t memory_id, const char *as_of)
{
   if (memory_id <= 0 || !as_of || !*as_of)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Absent bounds are OPEN, not closed: an empty valid_from means "as far back
    * as we know", an empty valid_until means "still true". Rows written before
    * supersession began stamping valid_until therefore read as current -- the
    * truthful answer, since we genuinely do not know when they stopped being
    * true. Inventing a boundary from updated_at would manufacture history.
    *
    * Normalise the separator before comparing, because these columns genuinely
    * hold TWO formats. schema.sql declares the canonical text form as
    * 'YYYY-MM-DD HH24:MI:SS' and pg_now_text() writes that, but the C writers
    * reach these same columns with now_utc(), which is ISO
    * ("2026-08-09T19:07:23Z"): db2_memory_set_versioned_key and
    * db2_memory_set_valid_from are both handed a now_utc() stamp by
    * memory_supersede, while db2_memory_set_lifecycle_state stamps pg_now_text().
    *
    * A plain text compare therefore decided on character 10, where 'T' (0x54)
    * sorts ABOVE ' ' (0x20). Every ISO bound ranked above every space-separated
    * value, inverting the verdict: a superseded row read as still in force, and
    * a row not yet valid read as valid. It failed silently, since a wrong
    * verdict is indistinguishable from a right one, and it only surfaced when
    * the DATE matched -- decade-apart dates decide at the year and never reach
    * the separator.
    *
    * replace()/rtrim() rather than a ::timestamptz cast: the DB2 unit tests run
    * on the SQLite shim, where the cast is a syntax error, and these two are
    * spelled the same in both engines. Offsets other than 'Z' are still compared
    * textually -- nothing in this tree writes one, and inventing a parser here
    * would be guessing at data we do not produce. */
   static const char *sql =
       "SELECT 1 FROM memories WHERE id = ?1"
       " AND (NULLIF(valid_from,'') IS NULL"
       "      OR rtrim(replace(valid_from,'T',' '),'Z') <= rtrim(replace(?2,'T',' '),'Z'))"
       " AND (NULLIF(valid_until,'') IS NULL"
       "      OR rtrim(replace(valid_until,'T',' '),'Z') > rtrim(replace(?3,'T',' '),'Z'))";
   char err[ML_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", as_of);
   aimee_pg_bind_text(st, "?3", as_of);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc == AIMEE_PG_ROW)
      return 1;
   /* A statement error -- an as_of Postgres cannot parse, or a malformed stored
    * bound -- is "could not tell", not "was not in force". Folding it into 0
    * would answer a question we failed to evaluate, which is the same lie the
    * -1 path upstream exists to avoid. */
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_memory_lifecycle_mark_pending(int64_t memory_id, int ttl_days)
{
   if (memory_id <= 0 || ttl_days <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* ttl_days is a small int we control; embed it into the SQL string
    * (matches the legacy behavior that built the same string). */
   char sql[256];
   snprintf(sql, sizeof(sql),
            "UPDATE memories SET lifecycle_state = 'pending',"
            " ttl_at = pg_now_text('+%d days'),"
            " updated_at = pg_now_text() WHERE id = ?1",
            ttl_days);

   char err[ML_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_memory_lifecycle_sweep_expired(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   /* pg_now_text() produces the DB2 canonical UTC text format used by
    * lifecycle timestamps. */
   static const char *sql = "UPDATE memories SET lifecycle_state = 'archived',"
                            " archive_reason = 'pending_ttl_expired', updated_at = pg_now_text()"
                            " WHERE lifecycle_state = 'pending' AND ttl_at != ''"
                            "   AND ttl_at < pg_now_text()";
   char err[ML_ERRBUF] = "";
   int affected = 0;
   if (aimee_pg_exec_with_changes(conn, sql, err, sizeof(err), &affected) != 0)
      return 0;
   return affected;
}

int db2_memory_lifecycle_counts(db2_memory_lifecycle_counts_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "SELECT lifecycle_state, COUNT(*) FROM memories GROUP BY lifecycle_state";
   char err[ML_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *s = aimee_pg_column_text(st, 0);
      int64_t n = aimee_pg_column_int64(st, 1);
      if (!s)
         continue;
      if (strcmp(s, "active") == 0)
         out->active = n;
      else if (strcmp(s, "pending") == 0)
         out->pending = n;
      else if (strcmp(s, "fulfilled") == 0)
         out->fulfilled = n;
      else if (strcmp(s, "superseded") == 0)
         out->superseded = n;
      else if (strcmp(s, "archived") == 0)
         out->archived = n;
   }
   aimee_pg_finalize(st);
   return 0;
}

int db2_memory_lifecycle_list_stale_pending(db2_memory_lifecycle_stale_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   /* The julianday() helper returns fractional days. Keep CAST AS REAL so the
    * expression remains explicitly fractional when differences are small. */
   static const char *sql =
       "SELECT id, content, created_at, ttl_at,"
       "       CAST((EXTRACT(EPOCH FROM 'now'::timestamp)/86400.0 - EXTRACT(EPOCH FROM "
       "created_at::timestamp)/86400.0) AS REAL) AS age_days,"
       "       CAST((EXTRACT(EPOCH FROM ttl_at::timestamp)/86400.0 - EXTRACT(EPOCH FROM "
       "created_at::timestamp)/86400.0) AS REAL) AS window_days"
       "  FROM memories m"
       " WHERE lifecycle_state = 'pending' AND ttl_at != ''"
       "   AND EXTRACT(EPOCH FROM ttl_at::timestamp)/86400.0 > EXTRACT(EPOCH FROM "
       "created_at::timestamp)/86400.0"
       "   AND (EXTRACT(EPOCH FROM 'now'::timestamp)/86400.0 - EXTRACT(EPOCH FROM "
       "created_at::timestamp)/86400.0) >="
       "       0.8 * (EXTRACT(EPOCH FROM ttl_at::timestamp)/86400.0 - EXTRACT(EPOCH FROM "
       "created_at::timestamp)/86400.0)" DB2_MEMORY_SCOPE_FILTER_SQL(
           "m.id") " ORDER BY " DB2_MEMORY_SCOPE_RANK_SQL("m.id") " DESC, EXTRACT(EPOCH FROM "
                                                                  "'now'::timestamp)/86400.0 - "
                                                                  "EXTRACT(EPOCH FROM "
                                                                  "created_at::timestamp)/86400.0 "
                                                                  "DESC"
                                                                  " LIMIT ?1";
   char err[ML_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);
   db2_memory_scope_bind_current(st);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].memory_id = aimee_pg_column_int64(st, 0);
      db2_copy_text(out[n].text, sizeof(out[n].text), aimee_pg_column_text(st, 1));
      db2_copy_text(out[n].created_at, sizeof(out[n].created_at), aimee_pg_column_text(st, 2));
      db2_copy_text(out[n].ttl_at, sizeof(out[n].ttl_at), aimee_pg_column_text(st, 3));
      out[n].age_days = aimee_pg_column_double(st, 4);
      out[n].window_days = aimee_pg_column_double(st, 5);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_lifecycle_list_unresolved_contradictions(db2_memory_lifecycle_conflict_t *out,
                                                        int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   /* The result exposes both memories' keys and content, so both sides must be
    * visible in current scope. A partial/orphan conflict is suppressed rather
    * than leaking the out-of-scope side; explicit all retains it when both rows
    * still exist. E2 owns an orphan-safe alert representation. */
   static const char *sql = "SELECT c.id, c.memory_a, c.memory_b, c.detected_at,"
                            "       ma.key, ma.content, mb.key, mb.content"
                            "  FROM memory_conflicts c"
                            "  LEFT JOIN memories ma ON ma.id = c.memory_a"
                            "  LEFT JOIN memories mb ON mb.id = c.memory_b"
                            " WHERE c.resolved = 0" ML_CONFLICT_A_FILTER ML_CONFLICT_B_FILTER
                            " ORDER BY CASE WHEN " ML_CONFLICT_A_RANK " > " ML_CONFLICT_B_RANK
                            " THEN " ML_CONFLICT_A_RANK " ELSE " ML_CONFLICT_B_RANK " END DESC,"
                            " c.detected_at DESC LIMIT ?1";
   char err[ML_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);
   db2_memory_scope_bind_current(st);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].conflict_id = aimee_pg_column_int64(st, 0);
      out[n].memory_a_id = aimee_pg_column_int64(st, 1);
      out[n].memory_b_id = aimee_pg_column_int64(st, 2);
      db2_copy_text(out[n].detected_at, sizeof(out[n].detected_at), aimee_pg_column_text(st, 3));
      db2_copy_text(out[n].a_key, sizeof(out[n].a_key), aimee_pg_column_text(st, 4));
      db2_copy_text(out[n].a_content, sizeof(out[n].a_content), aimee_pg_column_text(st, 5));
      db2_copy_text(out[n].b_content, sizeof(out[n].b_content), aimee_pg_column_text(st, 7));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_lifecycle_list_newly_superseded(const char *since,
                                               db2_memory_lifecycle_superseded_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[ML_ERRBUF] = "";
   aimee_pg_stmt_t *st = NULL;
   if (since && since[0])
   {
      static const char *sql_s =
          "SELECT m.id, m.key, m.content, m.updated_at FROM memories m"
          " WHERE m.lifecycle_state = 'superseded' AND m.updated_at >= "
          "?1" DB2_MEMORY_SCOPE_FILTER_SQL("m.id") " ORDER BY " DB2_MEMORY_SCOPE_RANK_SQL(
              "m.id") " DESC, m.updated_at DESC LIMIT ?2";
      st = aimee_pg_prepare(conn, sql_s, err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_text(st, "?1", since);
      aimee_pg_bind_int(st, "?2", max);
      db2_memory_scope_bind_current(st);
   }
   else
   {
      static const char *sql_d =
          "SELECT m.id, m.key, m.content, m.updated_at FROM memories m"
          " WHERE m.lifecycle_state = 'superseded' AND m.updated_at >= pg_now_text('-7 "
          "days')" DB2_MEMORY_SCOPE_FILTER_SQL("m.id") " ORDER BY " DB2_MEMORY_SCOPE_RANK_SQL(
              "m.id") " DESC, m.updated_at DESC LIMIT ?1";
      st = aimee_pg_prepare(conn, sql_d, err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_int(st, "?1", max);
      db2_memory_scope_bind_current(st);
   }

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].memory_id = aimee_pg_column_int64(st, 0);
      db2_copy_text(out[n].key, sizeof(out[n].key), aimee_pg_column_text(st, 1));
      db2_copy_text(out[n].text, sizeof(out[n].text), aimee_pg_column_text(st, 2));
      db2_copy_text(out[n].superseded_at, sizeof(out[n].superseded_at),
                    aimee_pg_column_text(st, 3));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}
