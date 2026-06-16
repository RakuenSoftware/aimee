/* aimee_pg_sqlite_shim.c — sqlite-backed implementation of the
 * aimee_pg_* libpq surface, linked into unit tests so they don't need
 * a real Postgres instance.
 *
 * Production: src/db_postgres.c uses libpq.
 * Tests: this file gets linked instead. The "conn" returned from
 * aimee_pg_open is the sqlite3* registered via db2_register_shared_sqlite,
 * so the same in-memory sqlite handle backs both the still-on-sqlite
 * db2 modules (db2_shared_sqlite()) and the migrated ones
 * (db2_conn() → which is now also a sqlite3*).
 *
 * SQL translation: the migrated db2 modules emit Postgres-flavored SQL
 * (?N placeholders, ON CONFLICT DO UPDATE, RETURNING, GREATEST,
 * to_tsvector, ::timestamp, datetime() shim usage). Most of that is
 * sqlite-compatible already (?N positional params work, ON CONFLICT,
 * RETURNING, datetime). The remaining incompatibilities get rewritten
 * here via a small text pass before sqlite3_prepare_v2 runs the SQL.
 *
 * Modules / call sites that exercise Postgres-specific SQL the shim
 * can't faithfully emulate (full FTS via tsquery, interval arithmetic
 * with sub-second precision) get coarse stand-ins that keep the test
 * compiling and running. Tests that need the real semantics belong in
 * db2-specific contract tests run against actual Postgres.
 */

#include "db_postgres.h"
#include "../db2/db2.h"
#include "../db2/db2_internal.h"

#include <ctype.h>
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * pgvec_cos_dist(a, b) — SQLite scalar function that computes cosine
 * distance between two pgvector-format text strings ("[f0,f1,...,fn-1]").
 * Registered as a custom function so that SQL containing the rewritten
 * `<=>` operator works against the SQLite shim.
 * ---------------------------------------------------------------------- */

#define PGVEC_SHIM_MAX_DIM 2048

static int pgvec_parse_vec(const char *text, float *vec, int max_dim)
{
   if (!text || !vec || max_dim <= 0)
      return 0;
   const char *p = text;
   while (*p && *p != '[')
      p++;
   if (!*p)
      return 0;
   p++;
   int n = 0;
   while (*p && *p != ']' && n < max_dim)
   {
      while (*p == ' ' || *p == ',')
         p++;
      if (*p == ']' || !*p)
         break;
      char *end;
      double val = strtod(p, &end);
      if (end == p)
         break;
      vec[n++] = (float)val;
      p = end;
   }
   return n;
}

static void pgvec_cos_dist_func(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
   if (argc != 2)
   {
      sqlite3_result_double(ctx, 1.0);
      return;
   }
   const char *at = (const char *)sqlite3_value_text(argv[0]);
   const char *bt = (const char *)sqlite3_value_text(argv[1]);
   if (!at || !bt)
   {
      sqlite3_result_double(ctx, 1.0);
      return;
   }
   float *a = malloc((size_t)PGVEC_SHIM_MAX_DIM * sizeof(float));
   float *b = malloc((size_t)PGVEC_SHIM_MAX_DIM * sizeof(float));
   if (!a || !b)
   {
      free(a);
      free(b);
      sqlite3_result_double(ctx, 1.0);
      return;
   }
   int na = pgvec_parse_vec(at, a, PGVEC_SHIM_MAX_DIM);
   int nb = pgvec_parse_vec(bt, b, PGVEC_SHIM_MAX_DIM);
   double dist = 1.0;
   if (na > 0 && na == nb)
   {
      double dot = 0.0, sa = 0.0, sb = 0.0;
      for (int i = 0; i < na; i++)
      {
         dot += (double)a[i] * b[i];
         sa += (double)a[i] * a[i];
         sb += (double)b[i] * b[i];
      }
      if (sa > 0.0 && sb > 0.0)
         dist = 1.0 - dot / (sqrt(sa) * sqrt(sb));
   }
   free(a);
   free(b);
   sqlite3_result_double(ctx, dist);
}

static void pgvec_register_functions(sqlite3 *db)
{
   if (!db)
      return;
   sqlite3_create_function(db, "pgvec_cos_dist", 2,
                           SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, NULL,
                           pgvec_cos_dist_func, NULL, NULL);
   /* Fake pg_indexes so pgvec_table_ready() returns true for vector tables. */
   sqlite3_exec(
       db,
       "CREATE VIEW IF NOT EXISTS pg_indexes AS "
       "SELECT 'memory_embeddings' AS tablename, 'idx_memory_embeddings_hnsw' AS indexname "
       "UNION ALL "
       "SELECT 'kb_embeddings', 'idx_kb_embeddings_hnsw'",
       NULL, NULL, NULL);
}

#define SHIM_MAX_PARAMS 16

struct aimee_pg_stmt
{
   sqlite3_stmt *st;
   sqlite3 *db;
   int rows_changed; /* captured at step time for aimee_pg_stmt_changes */
};

/* --- Placeholder rewriter (mirrors db_postgres.c, sqlite-friendly) ---
 *
 * The only callers of aimee_pg_rewrite_params are inside db_postgres.c
 * itself. We provide a no-op stub here so the symbol resolves. */

void aimee_pg_free_names(char **names, int count)
{
   if (!names)
      return;
   for (int i = 0; i < count; i++)
      free(names[i]);
   free(names);
}

int aimee_pg_rewrite_params(const char *sql_in, char **out_sql, char ***out_names, int *out_count,
                            char *errbuf, size_t errlen)
{
   /* Tests don't exercise this directly. Provide a passthrough stub. */
   (void)errbuf;
   (void)errlen;
   if (!sql_in || !out_sql || !out_names || !out_count)
      return -1;
   *out_sql = strdup(sql_in);
   *out_names = NULL;
   *out_count = 0;
   return *out_sql ? 0 : -1;
}

/* --- SQL translation: postgres syntax → sqlite-compatible ---
 *
 * Pure text replacement — not a real parser. Targets only the patterns
 * the migrated db2 modules emit: GREATEST, ::timestamp casts,
 * (CURRENT_TIMESTAMP AT TIME ZONE 'UTC'), interval '<n> seconds',
 * to_tsvector / to_tsquery, EXTRACT(EPOCH...). */

static int starts_with(const char *p, const char *prefix)
{
   while (*prefix)
   {
      if (*p++ != *prefix++)
         return 0;
   }
   return 1;
}

static char *translate_sql(const char *sql_in)
{
   if (!sql_in)
      return NULL;
   size_t cap = strlen(sql_in) * 2 + 64;
   char *out = malloc(cap);
   if (!out)
      return NULL;
   size_t o = 0;
   const char *p = sql_in;
   while (*p)
   {
      /* Grow output buffer to at least 64 chars headroom. */
      if (o + 128 >= cap)
      {
         cap *= 2;
         char *nb = realloc(out, cap);
         if (!nb)
         {
            free(out);
            return NULL;
         }
         out = nb;
      }
      /* TRUNCATE <table> -> DELETE FROM <table> */
      if (starts_with(p, "TRUNCATE "))
      {
         memcpy(out + o, "DELETE FROM ", 12);
         o += 12;
         p += 9;
         continue;
      }
      /* FOR UPDATE [SKIP LOCKED | NOWAIT] -> drop. Postgres row-level locking
       * has no analogue in the single-threaded sqlite shim, and sqlite rejects
       * the clause at prepare time. Dropping it lets queue-claim SQL (e.g. the
       * curator/job drains that select-and-lock one pending row) be exercised
       * under the shim. Real Postgres keeps the clause — this rewriter is
       * shim-only. */
      if (starts_with(p, "FOR UPDATE"))
      {
         p += 10; /* skip "FOR UPDATE" */
         if (starts_with(p, " SKIP LOCKED"))
            p += 12;
         else if (starts_with(p, " NOWAIT"))
            p += 7;
         continue;
      }
      /* GREATEST(...) -> MAX(...) — sqlite's MAX is also a scalar. */
      if (starts_with(p, "GREATEST("))
      {
         memcpy(out + o, "MAX(", 4);
         o += 4;
         p += 9;
         continue;
      }
      /* pg_now_text() -> datetime('now')  (sqlite has datetime built-in). */
      if (starts_with(p, "pg_now_text()"))
      {
         memcpy(out + o, "datetime('now')", 15);
         o += 15;
         p += 13;
         continue;
      }
      /* pg_now_text(<modifier>) -> datetime('now', <modifier>). The shim
       * keeps the modifier verbatim; sqlite's datetime accepts the same
       * '+/-N units' grammar postgres'  pg_now_text(modifier) does. */
      if (starts_with(p, "pg_now_text("))
      {
         memcpy(out + o, "datetime('now', ", 16);
         o += 16;
         p += 12;
         continue;
      }
      /* EXTRACT(EPOCH FROM <expr>::timestamp)/86400.0 -> julianday(<expr>).
       * Application SQL emits this expression for fractional days since
       * the epoch; sqlite's julianday() is the equivalent (with a
       * constant offset that always cancels in the differences these
       * expressions appear inside, so the comparison/subtraction
       * results are identical). */
      if (starts_with(p, "EXTRACT(EPOCH FROM "))
      {
         /* Skip "EXTRACT(EPOCH FROM ". Capture the inner expression up
          * to the matching ")::timestamp)/86400.0" suffix. */
         const char *q = p + 19;
         const char *expr = q;
         int depth = 1;
         while (*q && depth > 0)
         {
            if (*q == '(')
               depth++;
            else if (*q == ')')
            {
               depth--;
               if (depth == 0)
                  break;
            }
            q++;
         }
         /* Now q points to the closing ')' of EXTRACT. The original
          * input had the form ...::timestamp inside; the inner expr is
          * everything from `expr` to that ::timestamp marker. */
         size_t inner_len = (size_t)(q - expr);
         /* Trim any trailing "::timestamp" off the inner expression. */
         const char *ts_suffix = "::timestamp";
         size_t ts_len = strlen(ts_suffix);
         if (inner_len >= ts_len && memcmp(expr + inner_len - ts_len, ts_suffix, ts_len) == 0)
            inner_len -= ts_len;
         /* Confirm the trailing "/86400.0" is present immediately after
          * the closing ')'. If not, fall through and emit verbatim. */
         const char *tail = ")/86400.0";
         size_t tail_len = strlen(tail);
         if (*q == ')' && strncmp(q, tail, tail_len) == 0)
         {
            memcpy(out + o, "julianday(", 10);
            o += 10;
            memcpy(out + o, expr, inner_len);
            o += inner_len;
            out[o++] = ')';
            p = q + tail_len;
            continue;
         }
         /* Fallthrough: emit literally if the shape doesn't match. */
      }
      /* ::timestamp -> drop (treat the LHS as TEXT, lexicographic
       * comparison works for the canonical 'YYYY-MM-DD HH:MM:SS'
       * format the schema enforces). */
      if (starts_with(p, "::timestamp"))
      {
         p += 11;
         continue;
      }
      /* ::text -> drop.  JSONB-to-text cast; SQLite stores the column as
       * TEXT already so the bare value is compatible without a cast. */
      if (starts_with(p, "::text"))
      {
         p += 6;
         continue;
      }
      /* ::vector -> drop.  pgvector cast; the SQLite schema stubs store
       * embeddings as TEXT so the bare value is compatible without a cast. */
      if (starts_with(p, "::vector"))
      {
         p += 8;
         continue;
      }
      /* ::halfvec -> drop.  Same as ::vector — the embedding columns are now
       * halfvec (fp16), but the SQLite stub still stores embeddings as TEXT, so
       * the bare value is compatible without a cast. Must precede no other rule
       * (longer literal than ::vector). */
      if (starts_with(p, "::halfvec"))
      {
         p += 9;
         continue;
      }
      /* ILIKE -> LIKE.  SQLite's LIKE is case-insensitive for ASCII by
       * default, which matches ILIKE semantics for the terms this
       * codebase searches. */
      if (starts_with(p, "ILIKE"))
      {
         memcpy(out + o, "LIKE", 4);
         o += 4;
         p += 5;
         continue;
      }
      /* <col> <=> :param — pgvector cosine-distance operator.  Rewrite to
       * pgvec_cos_dist(<col>, :param) by looking back in the output buffer
       * to retrieve the column name that was already emitted. */
      if (starts_with(p, "<=>"))
      {
         p += 3; /* skip <=> */
         /* look back: find the identifier just emitted (skip trailing spaces) */
         size_t id_end = o;
         while (id_end > 0 && out[id_end - 1] == ' ')
            id_end--;
         size_t id_start = id_end;
         while (id_start > 0 &&
                (isalnum((unsigned char)out[id_start - 1]) || out[id_start - 1] == '_'))
            id_start--;
         char lhs[64] = "";
         size_t id_len = id_end - id_start;
         if (id_len < sizeof(lhs))
         {
            memcpy(lhs, out + id_start, id_len);
            lhs[id_len] = '\0';
         }
         o = id_start; /* retreat output past the identifier */

         /* skip whitespace between <=> and param */
         while (*p && isspace((unsigned char)*p))
            p++;
         /* collect :name / ?N / $N param */
         char rhs[64] = "";
         size_t rpos = 0;
         if (*p == ':' || *p == '$' || *p == '?')
         {
            rhs[rpos++] = *p++;
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && rpos < sizeof(rhs) - 1)
               rhs[rpos++] = *p++;
         }
         rhs[rpos] = '\0';

         /* emit pgvec_cos_dist(col, :param); ::vector after :param stripped next */
         int written = snprintf(out + o, cap - o, "pgvec_cos_dist(%s, %s)", lhs, rhs);
         if (written > 0)
            o += (size_t)written;
         continue;
      }
      /* to_char(CURRENT_TIMESTAMP, '<fmt>') -> datetime('now'). Postgres formats
       * the timestamp; sqlite's datetime('now') already yields the canonical
       * 'YYYY-MM-DD HH:MM:SS' the schema uses. Match the to_char(CURRENT_TIMESTAMP
       * prefix and skip to the matching close paren. */
      if (starts_with(p, "to_char(CURRENT_TIMESTAMP"))
      {
         const char *q = p + 8; /* after "to_char(" */
         int depth = 1;
         while (*q && depth > 0)
         {
            if (*q == '(')
               depth++;
            else if (*q == ')')
               depth--;
            q++;
         }
         memcpy(out + o, "datetime('now')", 15);
         o += 15;
         p = q; /* q now points just past the matching ')' */
         continue;
      }
      /* (CURRENT_TIMESTAMP AT TIME ZONE 'UTC') -> datetime('now') */
      if (starts_with(p, "(CURRENT_TIMESTAMP AT TIME ZONE 'UTC')"))
      {
         memcpy(out + o, "datetime('now')", 15);
         o += 15;
         p += 38;
         continue;
      }
      if (starts_with(p, "CURRENT_TIMESTAMP AT TIME ZONE 'UTC'"))
      {
         memcpy(out + o, "datetime('now')", 15);
         o += 15;
         p += 36;
         continue;
      }
      if (starts_with(p, "CURRENT_TIMESTAMP"))
      {
         memcpy(out + o, "datetime('now')", 15);
         o += 15;
         p += 17;
         continue;
      }
      /* interval '<n> seconds' -> hardcoded text on the comparison RHS.
       * The kb_runtime_state lock query is:
       *   state_value > (CURRENT_TIMESTAMP - interval '1800 seconds')
       * After ::timestamp drop and CURRENT_TIMESTAMP rewrite that becomes:
       *   state_value > (datetime('now') - interval '1800 seconds')
       * Translate interval to a datetime() modifier so the RHS becomes
       *   state_value > datetime('now', '-1800 seconds')
       * To do this cleanly we need to detect '- interval '<n> seconds''
       * and replace the whole expression. Search for the previous token
       * '- ' before this point. */
      if (starts_with(p, "interval '"))
      {
         const char *q = p + 10;
         int n = 0;
         while (*q && isdigit((unsigned char)*q))
         {
            n = n * 10 + (*q - '0');
            q++;
         }
         if (starts_with(q, " seconds'"))
         {
            /* Look back: is the immediately-prior char (after trimming
             * spaces) a '-'? If so, rewrite by removing the trailing
             * "datetime('now') - " we already wrote and emitting
             * "datetime('now', '-<n> seconds')". */
            size_t back = o;
            while (back > 0 && out[back - 1] == ' ')
               back--;
            if (back > 0 && out[back - 1] == '-')
            {
               /* Find "datetime('now')" preceding the '-'. */
               size_t end_of_dt = back - 1;
               while (end_of_dt > 0 && out[end_of_dt - 1] == ' ')
                  end_of_dt--;
               size_t dt_len = strlen("datetime('now')");
               if (end_of_dt >= dt_len &&
                   memcmp(out + end_of_dt - dt_len, "datetime('now')", dt_len) == 0)
               {
                  o = end_of_dt - dt_len;
                  char buf[64];
                  snprintf(buf, sizeof(buf), "datetime('now', '-%d seconds')", n);
                  size_t blen = strlen(buf);
                  memcpy(out + o, buf, blen);
                  o += blen;
                  p = q + 9;
                  continue;
               }
            }
         }
      }
      /* to_tsquery('<lang>', 'X') and to_tsvector('<lang>', col) — the
       * shim doesn't faithfully emulate full-text search. Replace the
       * whole '<col_or_expr> @@ to_tsquery(...)' predicate with '0'
       * (no FTS hits). Tests that rely on real FTS routing must be
       * covered by db2 contract tests against real Postgres. */
      if (starts_with(p, "to_tsvector("))
      {
         /* Skip until matching ')'. */
         int depth = 1;
         p += 12;
         while (*p && depth > 0)
         {
            if (*p == '(')
               depth++;
            else if (*p == ')')
               depth--;
            p++;
         }
         /* Skip whitespace and '@@'. */
         while (*p && isspace((unsigned char)*p))
            p++;
         if (starts_with(p, "@@"))
         {
            p += 2;
            while (*p && isspace((unsigned char)*p))
               p++;
            if (starts_with(p, "to_tsquery("))
            {
               depth = 1;
               p += 11;
               while (*p && depth > 0)
               {
                  if (*p == '(')
                     depth++;
                  else if (*p == ')')
                     depth--;
                  p++;
               }
            }
         }
         /* Replace the whole predicate with a falsy expression. */
         memcpy(out + o, "0=1", 3);
         o += 3;
         continue;
      }
      out[o++] = *p++;
   }
   out[o] = '\0';
   return out;
}

/* --- aimee_pg_open / close / exec --- */

void *aimee_pg_open(const char *conninfo, char *errbuf, size_t errlen)
{
   (void)conninfo;
   /* Tests register a sqlite handle as the shared one before db2_init
    * runs. Reuse it as the "postgres" connection. */
   sqlite3 *db = db2_shared_sqlite();
   if (!db)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen,
                  "no shared sqlite handle registered (call "
                  "db2_register_shared_sqlite first in test setup)");
      return NULL;
   }
   pgvec_register_functions(db);
   return db;
}

void aimee_pg_close(void *pg_conn)
{
   /* Test owns the sqlite lifecycle. */
   (void)pg_conn;
}

int aimee_pg_is_shim(void)
{
   return 1;
}

int aimee_pg_ping(void *pg_conn, char *errbuf, size_t errlen)
{
   sqlite3 *db = (sqlite3 *)pg_conn;
   if (!db)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "no connection");
      return -1;
   }
   return 0;
}

int aimee_pg_in_transaction(void *pg_conn)
{
   sqlite3 *db = (sqlite3 *)pg_conn;
   /* sqlite3_get_autocommit returns 0 while a transaction is open. */
   return (db && sqlite3_get_autocommit(db) == 0) ? 1 : 0;
}

int aimee_pg_exec(void *pg_conn, const char *sql, char *errbuf, size_t errlen)
{
   sqlite3 *db = (sqlite3 *)pg_conn;
   if (!db)
      return -1;
   /* The schema-apply path runs the full postgres schema text through
    * here. Tests are expected to apply db2_apply_schema_sqlite_shim to
    * their handle BEFORE registering it; ignore the postgres schema text. */
   if (sql && (strstr(sql, "CREATE OR REPLACE FUNCTION") != NULL ||
               strstr(sql, "AT TIME ZONE 'UTC'") != NULL ||
               strstr(sql, "GENERATED ALWAYS AS") != NULL || strstr(sql, "to_tsvector") != NULL ||
               strstr(sql, "USING hnsw") != NULL || strstr(sql, "DROP INDEX") != NULL))
      return 0;
   char *sql_t = translate_sql(sql);
   if (!sql_t)
      return -1;
   char *errmsg = NULL;
   int rc = sqlite3_exec(db, sql_t, NULL, NULL, &errmsg);
   free(sql_t);
   if (rc != SQLITE_OK)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "%s", errmsg ? errmsg : sqlite3_errmsg(db));
      sqlite3_free(errmsg);
      return -1;
   }
   sqlite3_free(errmsg);
   return 0;
}

int aimee_pg_exec_with_changes(void *pg_conn, const char *sql, char *errbuf, size_t errlen,
                               int *affected_out)
{
   sqlite3 *db = (sqlite3 *)pg_conn;
   if (affected_out)
      *affected_out = 0;
   int rc = aimee_pg_exec(pg_conn, sql, errbuf, errlen);
   if (rc == 0 && affected_out && db)
      *affected_out = sqlite3_changes(db);
   return rc;
}

/* --- Statements --- */

aimee_pg_stmt_t *aimee_pg_prepare(void *pg_conn, const char *sql, char *errbuf, size_t errlen)
{
   sqlite3 *db = (sqlite3 *)pg_conn;
   if (!db)
      return NULL;
   char *sql_t = translate_sql(sql);
   if (!sql_t)
      return NULL;
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(db, sql_t, -1, &st, NULL);
   if (rc != SQLITE_OK)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "prepare: %s (sql=%s)", sqlite3_errmsg(db), sql_t);
      free(sql_t);
      return NULL;
   }
   free(sql_t);
   aimee_pg_stmt_t *out = calloc(1, sizeof(*out));
   if (!out)
   {
      sqlite3_finalize(st);
      return NULL;
   }
   out->st = st;
   out->db = db;
   return out;
}

void aimee_pg_finalize(aimee_pg_stmt_t *stmt)
{
   if (!stmt)
      return;
   sqlite3_finalize(stmt->st);
   free(stmt);
}

aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *stmt, char *errbuf, size_t errlen)
{
   if (!stmt)
      return AIMEE_PG_ERR;
   int rc = sqlite3_step(stmt->st);
   if (rc == SQLITE_ROW)
      return AIMEE_PG_ROW;
   if (rc == SQLITE_DONE)
   {
      stmt->rows_changed = sqlite3_changes(stmt->db);
      return AIMEE_PG_DONE;
   }
   if (errbuf && errlen)
      snprintf(errbuf, errlen, "step: %s", sqlite3_errmsg(stmt->db));
   return AIMEE_PG_ERR;
}

int aimee_pg_reset(aimee_pg_stmt_t *stmt)
{
   if (!stmt)
      return -1;
   sqlite3_reset(stmt->st);
   sqlite3_clear_bindings(stmt->st);
   return 0;
}

int aimee_pg_stmt_changes(aimee_pg_stmt_t *stmt)
{
   if (!stmt)
      return 0;
   return stmt->rows_changed;
}

/* --- Bind helpers ---
 *
 * The migrated db2 modules call aimee_pg_bind_int(stmt, "?N", value).
 * sqlite supports numbered parameters natively (?1, ?2, ...), so we
 * pass the name through to sqlite3_bind_parameter_index. */

static int param_index(aimee_pg_stmt_t *stmt, const char *name)
{
   if (!stmt || !name)
      return 0;
   /* Try the name verbatim first ("?1", "?2"). */
   int idx = sqlite3_bind_parameter_index(stmt->st, name);
   if (idx > 0)
      return idx;
   /* Fall back: ":<name>" form. */
   if (name[0] != ':')
   {
      char buf[64];
      snprintf(buf, sizeof(buf), ":%s", name);
      idx = sqlite3_bind_parameter_index(stmt->st, buf);
      if (idx > 0)
         return idx;
   }
   /* Last resort: parse "?N" as positional. */
   if (name[0] == '?')
   {
      int n = atoi(name + 1);
      if (n > 0)
         return n;
   }
   return 0;
}

int aimee_pg_bind_int(aimee_pg_stmt_t *stmt, const char *name, int value)
{
   int idx = param_index(stmt, name);
   if (idx <= 0)
      return -1;
   return sqlite3_bind_int(stmt->st, idx, value) == SQLITE_OK ? 0 : -1;
}

int aimee_pg_bind_int64(aimee_pg_stmt_t *stmt, const char *name, int64_t value)
{
   int idx = param_index(stmt, name);
   if (idx <= 0)
      return -1;
   return sqlite3_bind_int64(stmt->st, idx, value) == SQLITE_OK ? 0 : -1;
}

int aimee_pg_bind_double(aimee_pg_stmt_t *stmt, const char *name, double value)
{
   int idx = param_index(stmt, name);
   if (idx <= 0)
      return -1;
   return sqlite3_bind_double(stmt->st, idx, value) == SQLITE_OK ? 0 : -1;
}

int aimee_pg_bind_text(aimee_pg_stmt_t *stmt, const char *name, const char *value)
{
   int idx = param_index(stmt, name);
   if (idx <= 0)
      return -1;
   return sqlite3_bind_text(stmt->st, idx, value, -1, SQLITE_TRANSIENT) == SQLITE_OK ? 0 : -1;
}

int aimee_pg_bind_blob(aimee_pg_stmt_t *stmt, const char *name, const void *value, int len)
{
   int idx = param_index(stmt, name);
   if (idx <= 0)
      return -1;
   return sqlite3_bind_blob(stmt->st, idx, value, len, SQLITE_TRANSIENT) == SQLITE_OK ? 0 : -1;
}

int aimee_pg_bind_null(aimee_pg_stmt_t *stmt, const char *name)
{
   int idx = param_index(stmt, name);
   if (idx <= 0)
      return -1;
   return sqlite3_bind_null(stmt->st, idx) == SQLITE_OK ? 0 : -1;
}

/* --- Column accessors --- */

int aimee_pg_column_count(aimee_pg_stmt_t *stmt)
{
   return stmt ? sqlite3_column_count(stmt->st) : 0;
}

const char *aimee_pg_column_name(aimee_pg_stmt_t *stmt, int col)
{
   return stmt ? sqlite3_column_name(stmt->st, col) : NULL;
}

int aimee_pg_column_type(aimee_pg_stmt_t *stmt, int col)
{
   if (!stmt)
      return AIMEE_PG_VALUE_NULL;
   int t = sqlite3_column_type(stmt->st, col);
   switch (t)
   {
   case SQLITE_INTEGER:
      return AIMEE_PG_VALUE_INTEGER;
   case SQLITE_FLOAT:
      return AIMEE_PG_VALUE_FLOAT;
   case SQLITE_TEXT:
      return AIMEE_PG_VALUE_TEXT;
   case SQLITE_BLOB:
      return AIMEE_PG_VALUE_BLOB;
   default:
      return AIMEE_PG_VALUE_NULL;
   }
}

const void *aimee_pg_column_blob(aimee_pg_stmt_t *stmt, int col)
{
   return stmt ? sqlite3_column_blob(stmt->st, col) : NULL;
}

int aimee_pg_column_bytes(aimee_pg_stmt_t *stmt, int col)
{
   return stmt ? sqlite3_column_bytes(stmt->st, col) : 0;
}

int aimee_pg_column_is_null(aimee_pg_stmt_t *stmt, int col)
{
   return stmt && sqlite3_column_type(stmt->st, col) == SQLITE_NULL;
}

int aimee_pg_column_int(aimee_pg_stmt_t *stmt, int col)
{
   return stmt ? sqlite3_column_int(stmt->st, col) : 0;
}

int64_t aimee_pg_column_int64(aimee_pg_stmt_t *stmt, int col)
{
   return stmt ? sqlite3_column_int64(stmt->st, col) : 0;
}

double aimee_pg_column_double(aimee_pg_stmt_t *stmt, int col)
{
   return stmt ? sqlite3_column_double(stmt->st, col) : 0.0;
}

const char *aimee_pg_column_text(aimee_pg_stmt_t *stmt, int col)
{
   if (!stmt)
      return NULL;
   return (const char *)sqlite3_column_text(stmt->st, col);
}
