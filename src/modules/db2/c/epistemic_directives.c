/* db2/epistemic_directives.c: storage primitives for epistemic_directives
 * — Postgres via libpq. */

#include "../headers/aimee.h" /* memory_directive_t and friends */
#include "db_postgres.h"
#include "epistemic_directives.h"
#include "db2_internal.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define ED_ERRBUF 256

static const char *ED_SELECT_COLS =
    "id, question, topic, anchor_entity, anchor_file, cause, priority, state,"
    " memory_a_id, memory_b_id, resolution_memory_id, evidence, source_session,"
    " surfaced_count, last_surfaced_at, resolved_at, valid_until, created_at, updated_at";

static void ed_row_from_pg_stmt(aimee_pg_stmt_t *st, memory_directive_t *out)
{
   memset(out, 0, sizeof(*out));
   out->id = aimee_pg_column_int64(st, 0);
   db2_copy_text(out->question, sizeof(out->question), aimee_pg_column_text(st, 1));
   db2_copy_text(out->topic, sizeof(out->topic), aimee_pg_column_text(st, 2));
   db2_copy_text(out->anchor_entity, sizeof(out->anchor_entity), aimee_pg_column_text(st, 3));
   db2_copy_text(out->anchor_file, sizeof(out->anchor_file), aimee_pg_column_text(st, 4));
   db2_copy_text(out->cause, sizeof(out->cause), aimee_pg_column_text(st, 5));
   out->priority = aimee_pg_column_int(st, 6);
   db2_copy_text(out->state, sizeof(out->state), aimee_pg_column_text(st, 7));
   out->memory_a_id = aimee_pg_column_int64(st, 8);
   out->memory_b_id = aimee_pg_column_int64(st, 9);
   out->resolution_memory_id = aimee_pg_column_int64(st, 10);
   db2_copy_text(out->evidence, sizeof(out->evidence), aimee_pg_column_text(st, 11));
   db2_copy_text(out->source_session, sizeof(out->source_session), aimee_pg_column_text(st, 12));
   out->surfaced_count = aimee_pg_column_int(st, 13);
   db2_copy_text(out->last_surfaced_at, sizeof(out->last_surfaced_at),
                 aimee_pg_column_text(st, 14));
   db2_copy_text(out->resolved_at, sizeof(out->resolved_at), aimee_pg_column_text(st, 15));
   db2_copy_text(out->valid_until, sizeof(out->valid_until), aimee_pg_column_text(st, 16));
   db2_copy_text(out->created_at, sizeof(out->created_at), aimee_pg_column_text(st, 17));
   db2_copy_text(out->updated_at, sizeof(out->updated_at), aimee_pg_column_text(st, 18));
}

int db2_directive_insert_ignore(const char *question, const char *topic, const char *anchor_entity,
                                const char *anchor_file, const char *cause, int priority,
                                int64_t memory_a_id, int64_t memory_b_id, const char *evidence,
                                const char *source_session, const char *valid_until,
                                int64_t *out_id, int *out_existed)
{
   if (out_id)
      *out_id = 0;
   if (out_existed)
      *out_existed = 0;
   if (!question || !*question || !cause)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* epistemic_directives has partial UNIQUE indexes:
    *   idx_directives_dedup_contradiction (memory_a_id, memory_b_id)
    *     WHERE cause = 'contradiction' AND memory_a_id != 0 AND memory_b_id != 0
    *   idx_directives_dedup_topic (cause, topic)
    *     WHERE topic != '' AND cause IN ('retrieval_failure', 'missing_config')
    * ON CONFLICT DO NOTHING covers both. RETURNING id yields a row only on
    * actual insert; on dedup we get AIMEE_PG_DONE with no row. */
   char err[ED_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO epistemic_directives"
                        " (question, topic, anchor_entity, anchor_file, cause, priority, state,"
                        "  memory_a_id, memory_b_id, evidence, source_session, valid_until)"
                        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'open', ?7, ?8, ?9, ?10, ?11)"
                        " ON CONFLICT DO NOTHING RETURNING id",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", question);
   aimee_pg_bind_text(st, "?2", topic ? topic : "");
   aimee_pg_bind_text(st, "?3", anchor_entity ? anchor_entity : "");
   aimee_pg_bind_text(st, "?4", anchor_file ? anchor_file : "");
   aimee_pg_bind_text(st, "?5", cause);
   aimee_pg_bind_int(st, "?6", priority);
   aimee_pg_bind_int64(st, "?7", memory_a_id);
   aimee_pg_bind_int64(st, "?8", memory_b_id);
   aimee_pg_bind_text(st, "?9", evidence ? evidence : "");
   aimee_pg_bind_text(st, "?10", source_session ? source_session : "");
   aimee_pg_bind_text(st, "?11", valid_until ? valid_until : "");

   int64_t new_id = 0;
   int inserted = 0;
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   if (rc == AIMEE_PG_ROW)
   {
      new_id = aimee_pg_column_int64(st, 0);
      inserted = 1;
   }
   else if (rc != AIMEE_PG_DONE)
   {
      aimee_pg_finalize(st);
      return -1;
   }
   aimee_pg_finalize(st);

   if (inserted)
   {
      if (out_id)
         *out_id = new_id;
      return 0;
   }

   /* Dedup hit: look up the existing row by the natural-key columns
    * (cause, topic, question) so callers can link to it. */
   if (out_existed)
      *out_existed = 1;

   aimee_pg_stmt_t *sel =
       aimee_pg_prepare(conn,
                        "SELECT id FROM epistemic_directives"
                        " WHERE cause = ?1 AND topic = ?2 AND question = ?3 LIMIT 1",
                        err, sizeof(err));
   if (!sel)
      return 0;
   aimee_pg_bind_text(sel, "?1", cause);
   aimee_pg_bind_text(sel, "?2", topic ? topic : "");
   aimee_pg_bind_text(sel, "?3", question);
   if (aimee_pg_step(sel, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (out_id)
         *out_id = aimee_pg_column_int64(sel, 0);
   }
   aimee_pg_finalize(sel);
   return 0;
}

int db2_directive_get(int64_t id, memory_directive_t *out)
{
   if (!out || id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char sql[512];
   snprintf(sql, sizeof(sql), "SELECT %s FROM epistemic_directives WHERE id = ?1", ED_SELECT_COLS);
   char err[ED_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      ed_row_from_pg_stmt(st, out);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_directive_find_by_cause_topic(const char *cause, const char *topic, memory_directive_t *out)
{
   if (!out || !cause)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char sql[1024];
   snprintf(sql, sizeof(sql),
            "SELECT %s FROM epistemic_directives WHERE cause = ?1 AND topic = ?2 LIMIT 1",
            ED_SELECT_COLS);
   char err[ED_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", cause);
   aimee_pg_bind_text(st, "?2", topic ? topic : "");
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      ed_row_from_pg_stmt(st, out);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_directive_list(const char *state, const char *cause, memory_directive_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   const int have_state = state && state[0];
   const int have_cause = cause && cause[0];
   char sql[1024];
   if (have_state && have_cause)
      snprintf(sql, sizeof(sql),
               "SELECT %s FROM epistemic_directives WHERE state = ?1 AND cause = ?2"
               " ORDER BY priority DESC, created_at DESC, id DESC LIMIT ?3",
               ED_SELECT_COLS);
   else if (have_state)
      snprintf(sql, sizeof(sql),
               "SELECT %s FROM epistemic_directives WHERE state = ?1"
               " ORDER BY priority DESC, created_at DESC, id DESC LIMIT ?2",
               ED_SELECT_COLS);
   else if (have_cause)
      snprintf(sql, sizeof(sql),
               "SELECT %s FROM epistemic_directives WHERE cause = ?1"
               " ORDER BY priority DESC, created_at DESC, id DESC LIMIT ?2",
               ED_SELECT_COLS);
   else
      snprintf(sql, sizeof(sql),
               "SELECT %s FROM epistemic_directives"
               " ORDER BY priority DESC, created_at DESC, id DESC LIMIT ?1",
               ED_SELECT_COLS);

   char err[ED_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   int idx = 1;
   char placeholder[8];
   if (have_state)
   {
      snprintf(placeholder, sizeof(placeholder), "?%d", idx++);
      aimee_pg_bind_text(st, placeholder, state);
   }
   if (have_cause)
   {
      snprintf(placeholder, sizeof(placeholder), "?%d", idx++);
      aimee_pg_bind_text(st, placeholder, cause);
   }
   snprintf(placeholder, sizeof(placeholder), "?%d", idx);
   aimee_pg_bind_int(st, placeholder, max);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      ed_row_from_pg_stmt(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_directive_counts_by_state(int64_t *open, int64_t *suppressed, int64_t *resolved,
                                  int64_t *expired)
{
   if (open)
      *open = 0;
   if (suppressed)
      *suppressed = 0;
   if (resolved)
      *resolved = 0;
   if (expired)
      *expired = 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[ED_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT state, COUNT(*) FROM epistemic_directives GROUP BY state", err, sizeof(err));
   if (!st)
      return -1;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *s = aimee_pg_column_text(st, 0);
      int64_t n = aimee_pg_column_int64(st, 1);
      if (!s)
         continue;
      if (strcmp(s, "open") == 0 && open)
         *open = n;
      else if (strcmp(s, "suppressed") == 0 && suppressed)
         *suppressed = n;
      else if (strcmp(s, "resolved") == 0 && resolved)
         *resolved = n;
      else if (strcmp(s, "expired") == 0 && expired)
         *expired = n;
   }
   aimee_pg_finalize(st);
   return 0;
}

int db2_directive_resolve(int64_t id, int64_t resolution_memory_id)
{
   if (id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[ED_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE epistemic_directives SET state = 'resolved',"
                                          " resolution_memory_id = ?1,"
                                          " resolved_at = pg_now_text(),"
                                          " updated_at = pg_now_text()"
                                          " WHERE id = ?2 AND state = 'open'",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", resolution_memory_id);
   aimee_pg_bind_int64(st, "?2", id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE && changes > 0) ? 0 : -1;
}

int db2_directive_suppress(int64_t id)
{
   if (id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[ED_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE epistemic_directives SET state = 'suppressed',"
                                          " updated_at = pg_now_text()"
                                          " WHERE id = ?1 AND state = 'open'",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE && changes > 0) ? 0 : -1;
}

int db2_directive_sweep_expired(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[ED_ERRBUF] = "";
   int affected = 0;
   if (aimee_pg_exec_with_changes(
           conn,
           "UPDATE epistemic_directives SET state = 'expired', updated_at = pg_now_text()"
           " WHERE state = 'open' AND valid_until != '' AND valid_until < pg_now_text()",
           err, sizeof(err), &affected) != 0)
      return 0;
   return affected;
}

int db2_directive_record_surface(int64_t id)
{
   if (id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[ED_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "UPDATE epistemic_directives SET surfaced_count = surfaced_count + 1,"
                        " last_surfaced_at = pg_now_text(), updated_at = pg_now_text()"
                        " WHERE id = ?1 AND state = 'open'",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE && changes > 0) ? 0 : -1;
}

int db2_directive_match_by_entity(const char *entity_lc, memory_directive_t *out, int max)
{
   if (!entity_lc || !*entity_lc || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char sql[1024];
   snprintf(sql, sizeof(sql),
            "SELECT %s FROM epistemic_directives"
            " WHERE state = 'open' AND LOWER(anchor_entity) = ?1"
            " ORDER BY priority DESC, created_at DESC, id DESC LIMIT ?2",
            ED_SELECT_COLS);
   char err[ED_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", entity_lc);
   aimee_pg_bind_int(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      ed_row_from_pg_stmt(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_directive_match_by_file(const char *file, memory_directive_t *out, int max)
{
   if (!file || !*file || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char sql[1024];
   snprintf(sql, sizeof(sql),
            "SELECT %s FROM epistemic_directives"
            " WHERE state = 'open' AND anchor_file = ?1"
            " ORDER BY priority DESC, created_at DESC, id DESC LIMIT ?2",
            ED_SELECT_COLS);
   char err[ED_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", file);
   aimee_pg_bind_int(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      ed_row_from_pg_stmt(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

/* Directive lexical matching currently parses the caller's quoted OR clause
 * into bare alphanumeric tokens and runs a token-OR ILIKE scan over
 * question || ' ' || topic. A future pgvector directive index can replace
 * this without changing the non-DB2 API surface. */
#define ED_LEXICAL_MAX_TOKENS    16
#define ED_LEXICAL_MAX_TOKEN_LEN 64

int db2_directive_match_by_lexical(const char *match_clause, memory_directive_t *out, int max)
{
   if (!match_clause || !*match_clause || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   /* Parse alphanumeric runs (length >= 2) from the lexical clause. The
    * syntax wraps tokens in quotes and joins with OR; strip those and
    * lowercase what's left. */
   char tokens[ED_LEXICAL_MAX_TOKENS][ED_LEXICAL_MAX_TOKEN_LEN];
   int n_tokens = 0;
   {
      const size_t clen = strlen(match_clause);
      size_t i = 0;
      while (i < clen && n_tokens < ED_LEXICAL_MAX_TOKENS)
      {
         while (i < clen && !isalnum((unsigned char)match_clause[i]))
            i++;
         size_t start = i;
         while (i < clen && (isalnum((unsigned char)match_clause[i]) || match_clause[i] == '_'))
            i++;
         size_t wlen = i - start;
         if (wlen < 2)
            continue;
         /* Skip the literal OR connector emitted by the query builder. */
         if (wlen == 2 && (match_clause[start] == 'O' || match_clause[start] == 'o') &&
             (match_clause[start + 1] == 'R' || match_clause[start + 1] == 'r'))
            continue;
         if (wlen >= ED_LEXICAL_MAX_TOKEN_LEN)
            wlen = ED_LEXICAL_MAX_TOKEN_LEN - 1;
         for (size_t k = 0; k < wlen; k++)
            tokens[n_tokens][k] = (char)tolower((unsigned char)match_clause[start + k]);
         tokens[n_tokens][wlen] = '\0';
         n_tokens++;
      }
   }
   if (n_tokens == 0)
      return 0;

   /* Build SQL: SELECT ... WHERE state='open' AND (LOWER(question || ' ' ||
    * topic) LIKE ?1 OR ... LIKE ?N) ORDER BY ... LIMIT ?{N+1}. */
   char sql[2048];
   char *p = sql;
   size_t left = sizeof(sql);
   int written = snprintf(p, left, "SELECT %s FROM epistemic_directives WHERE state = 'open' AND (",
                          ED_SELECT_COLS);
   if (written < 0 || (size_t)written >= left)
      return 0;
   p += written;
   left -= (size_t)written;
   for (int i = 0; i < n_tokens; i++)
   {
      written =
          snprintf(p, left, "%sLOWER(question || ' ' || topic) LIKE ?%d", i ? " OR " : "", i + 1);
      if (written < 0 || (size_t)written >= left)
         return 0;
      p += written;
      left -= (size_t)written;
   }
   written = snprintf(p, left, ") ORDER BY priority DESC, created_at DESC, id DESC LIMIT ?%d",
                      n_tokens + 1);
   if (written < 0 || (size_t)written >= left)
      return 0;

   char err[ED_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   /* Bind each LIKE pattern as '%token%'. */
   char like_bufs[ED_LEXICAL_MAX_TOKENS][ED_LEXICAL_MAX_TOKEN_LEN + 4];
   char placeholder[8];
   for (int i = 0; i < n_tokens; i++)
   {
      snprintf(like_bufs[i], sizeof(like_bufs[i]), "%%%s%%", tokens[i]);
      snprintf(placeholder, sizeof(placeholder), "?%d", i + 1);
      aimee_pg_bind_text(st, placeholder, like_bufs[i]);
   }
   snprintf(placeholder, sizeof(placeholder), "?%d", n_tokens + 1);
   aimee_pg_bind_int(st, placeholder, max);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      ed_row_from_pg_stmt(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_directive_resolve_contradiction(int64_t memory_a_id, int64_t memory_b_id,
                                        int64_t resolution_memory_id)
{
   if (memory_a_id <= 0 || memory_b_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Pair is symmetric in the source data: resolve regardless of direction. */
   char err[ED_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE epistemic_directives SET state = 'resolved',"
                                          " resolution_memory_id = ?1, resolved_at = pg_now_text(),"
                                          " updated_at = pg_now_text()"
                                          " WHERE state = 'open' AND cause = 'contradiction'"
                                          "   AND ((memory_a_id = ?2 AND memory_b_id = ?3)"
                                          "        OR (memory_a_id = ?3 AND memory_b_id = ?2))",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", resolution_memory_id);
   aimee_pg_bind_int64(st, "?2", memory_a_id);
   aimee_pg_bind_int64(st, "?3", memory_b_id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? changes : -1;
}
