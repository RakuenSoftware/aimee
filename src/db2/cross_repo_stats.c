/* cross_repo_stats.c: DB-backed stats / data-gathering for the cross-repo
 * resolver (S3). Portable SQL over db2 (Postgres in prod; sqlite test shim).
 * Feeds the pure S2a/S2b core. See cross_repo_stats.h and
 * docs/proposals/pending/cross-repo-dependency-graph.md §3.3/§4.1. */

#include "cross_repo_stats.h"

#include "aimee.h"
#include "db2.h"
#include "db_postgres.h"
#include "log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CR_LOG_TAG "cross_repo"
#define CR_ERRBUF  256

static void *cr_conn(void)
{
   return db2_conn();
}

/* ---- FNV-1a 64-bit ------------------------------------------------------- */

static uint64_t fnv1a_init(void)
{
   return 1469598103934665603ULL;
}

static uint64_t fnv1a_add(uint64_t h, const char *s)
{
   if (!s)
      return h;
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
   {
      h ^= (uint64_t)*p;
      h *= 1099511628211ULL;
   }
   /* a field separator so ("ab","c") and ("a","bc") differ */
   h ^= 0x1fULL;
   h *= 1099511628211ULL;
   return h;
}

/* ---- small query helpers ------------------------------------------------- */

/* Run a single-column COUNT/scalar query with up to two text binds; returns the
 * int64 in *out. Pass NULL for an unused bind. Returns 0 on success, -1 error. */
static int cr_scalar(void *conn, const char *sql, const char *b1, const char *b2, int64_t *out)
{
   char err[CR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   if (b1)
      aimee_pg_bind_text(st, "?1", b1);
   if (b2)
      aimee_pg_bind_text(st, "?2", b2);
   int64_t v = 0;
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      v = aimee_pg_column_int64(st, 0);
      rc = 0;
   }
   aimee_pg_finalize(st);
   if (rc == 0 && out)
      *out = v;
   return rc;
}

/* ---- distinctiveness stats (§3.3) ---------------------------------------- */

int db2_cross_repo_distinct_stats(const char *symbol, const char *caller_repo,
                                  xrepo_distinct_stats_t *out)
{
   void *conn = cr_conn();
   if (!conn || !symbol || !caller_repo || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   int64_t v = 0;
   /* callee in >= ? trusted repos */
   if (cr_scalar(conn,
                 "SELECT COUNT(DISTINCT p.id) FROM code_calls cc "
                 "JOIN files f ON f.id = cc.file_id JOIN projects p ON p.id = f.project_id "
                 "WHERE cc.callee = ?1 AND p.trust = 'trusted'",
                 symbol, NULL, &v) != 0)
      return -1;
   out->callee_repo_count = (int)v;

   /* defined in >= ? trusted repos. Positive match on kind='definition' (the
    * convention db2_code_index_term_find uses) rather than a negative kind<>'route'
    * exclusion, so a future terms.kind value can't silently inflate the count. */
   if (cr_scalar(conn,
                 "SELECT COUNT(DISTINCT p.id) FROM terms t "
                 "JOIN files f ON f.id = t.file_id JOIN projects p ON p.id = f.project_id "
                 "WHERE t.name = ?1 AND t.kind = 'definition' AND p.trust = 'trusted'",
                 symbol, NULL, &v) != 0)
      return -1;
   out->definer_repo_count = (int)v;

   /* caller-file percentage: files of A using S as a callee / total files of A */
   int64_t num = 0, denom = 0;
   if (cr_scalar(conn,
                 "SELECT COUNT(DISTINCT f.id) FROM code_calls cc "
                 "JOIN files f ON f.id = cc.file_id JOIN projects p ON p.id = f.project_id "
                 "WHERE p.name = ?1 AND cc.callee = ?2",
                 caller_repo, symbol, &num) != 0)
      return -1;
   if (cr_scalar(conn,
                 "SELECT COUNT(*) FROM files f JOIN projects p ON p.id = f.project_id "
                 "WHERE p.name = ?1",
                 caller_repo, NULL, &denom) != 0)
      return -1;
   out->caller_file_pct = denom > 0 ? (int)((num * 100) / denom) : 0;
   return 0;
}

/* ---- blocked_symbols (§3.3) ---------------------------------------------- */

int db2_cross_repo_symbol_blocked(const char *symbol, const char *lang)
{
   void *conn = cr_conn();
   if (!conn || !symbol)
      return -1;
   char err[CR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT 1 FROM blocked_symbols WHERE word = ?1 AND (lang = ?2 OR lang = '') LIMIT 1",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", symbol);
   aimee_pg_bind_text(st, "?2", lang ? lang : "");
   int blocked = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW ? 1 : 0;
   aimee_pg_finalize(st);
   return blocked;
}

int db2_cross_repo_recompute_blocked_symbols(int k, int m, int len_min)
{
   void *conn = cr_conn();
   if (!conn || k <= 0 || m <= 0 || len_min <= 0)
      return -1;
   char err[CR_ERRBUF] = "";

   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   int ok = 1;
   /* Bump the version INSIDE the transaction (UPDATE then read), not a
    * read-before-BEGIN of prev+1: the UPDATE takes the row lock so two concurrent
    * recomputes serialize and can't both commit the same version (no PK collision
    * / backward step). UPDATE+SELECT is portable (avoids UPDATE ... RETURNING,
    * which older sqlite shims lack). */
   int64_t ver = 0;
   if (aimee_pg_exec(conn,
                     "UPDATE cross_repo_meta SET blocked_symbols_version = "
                     "blocked_symbols_version + 1 WHERE id = 1",
                     err, sizeof(err)) != 0)
      ok = 0;
   if (ok && cr_scalar(conn, "SELECT blocked_symbols_version FROM cross_repo_meta WHERE id = 1",
                       NULL, NULL, &ver) != 0)
      ok = 0;
   if (ok && aimee_pg_exec(conn, "DELETE FROM blocked_symbols", err, sizeof(err)) != 0)
      ok = 0;

   /* Frequency-derived blocks over TRUSTED repos: callee in >= k repos, or
    * defined in >= m repos. length >= len_min so we don't store short names
    * (those are excluded by the distinctiveness len gate at query time). */
   if (ok)
   {
      /* DELETE above clears the table, and the UNION selects only distinct names
       * (so the two arms can't produce duplicate (word,'') rows) -- no ON CONFLICT
       * needed, which also keeps the statement portable to the sqlite shim. */
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn,
          "INSERT INTO blocked_symbols (word, lang, reason, version) "
          "SELECT name, '', 'frequency', ?3 FROM ("
          "  SELECT cc.callee AS name FROM code_calls cc "
          "    JOIN files f ON f.id = cc.file_id JOIN projects p ON p.id = f.project_id "
          "    WHERE p.trust = 'trusted' AND length(cc.callee) >= ?4 "
          "    GROUP BY cc.callee HAVING COUNT(DISTINCT p.id) >= ?1 "
          "  UNION "
          "  SELECT t.name AS name FROM terms t "
          "    JOIN files f ON f.id = t.file_id JOIN projects p ON p.id = f.project_id "
          "    WHERE p.trust = 'trusted' AND t.kind = 'definition' AND length(t.name) >= ?4 "
          "    GROUP BY t.name HAVING COUNT(DISTINCT p.id) >= ?2"
          ") q",
          err, sizeof(err));
      if (!st)
         ok = 0;
      else
      {
         aimee_pg_bind_int(st, "?1", k);
         aimee_pg_bind_int(st, "?2", m);
         aimee_pg_bind_int64(st, "?3", ver);
         aimee_pg_bind_int(st, "?4", len_min);
         if (aimee_pg_step(st, err, sizeof(err)) < 0)
            ok = 0;
         aimee_pg_finalize(st);
      }
   }

   if (!ok)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      LOG_ERROR(CR_LOG_TAG, "blocked_symbols recompute failed: %s", err);
      return -1;
   }
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      return -1;

   int64_t n = 0;
   (void)cr_scalar(conn, "SELECT COUNT(*) FROM blocked_symbols", NULL, NULL, &n);
   return (int)n;
}

/* ---- meta / hashes (§4.1) ------------------------------------------------ */

int db2_cross_repo_meta_read(int64_t *trust_epoch, int64_t *blocked_symbols_version,
                             char *repo_set_hash, size_t cap)
{
   void *conn = cr_conn();
   if (!conn)
      return -1;
   char err[CR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT trust_epoch, blocked_symbols_version, repo_set_hash FROM cross_repo_meta "
       "WHERE id = 1",
       err, sizeof(err));
   if (!st)
      return -1;
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (trust_epoch)
         *trust_epoch = aimee_pg_column_int64(st, 0);
      if (blocked_symbols_version)
         *blocked_symbols_version = aimee_pg_column_int64(st, 1);
      if (repo_set_hash && cap)
         snprintf(repo_set_hash, cap, "%s", aimee_pg_column_text(st, 2));
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

/* Hash one ordered single-column query's rows into the running FNV state. */
static int cr_hash_query(void *conn, const char *sql, const char *bind, uint64_t *h)
{
   char err[CR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   if (bind)
      aimee_pg_bind_text(st, "?1", bind);
   int step;
   while ((step = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
      *h = fnv1a_add(*h, aimee_pg_column_text(st, 0));
   aimee_pg_finalize(st);
   return step < 0 ? -1 : 0;
}

int db2_cross_repo_repo_symbol_hash(const char *project, char *out, size_t cap)
{
   void *conn = cr_conn();
   if (!conn || !project || !out || cap < 17)
      return -1;
   uint64_t h = fnv1a_init();
   /* terms (name|kind), exports (name), imports (name) -- ordered for determinism. */
   if (cr_hash_query(conn,
                     "SELECT t.name || '|' || t.kind FROM terms t "
                     "JOIN files f ON f.id = t.file_id JOIN projects p ON p.id = f.project_id "
                     "WHERE p.name = ?1 ORDER BY 1",
                     project, &h) != 0)
      return -1;
   if (cr_hash_query(conn,
                     "SELECT 'E:' || e.name FROM file_exports e "
                     "JOIN files f ON f.id = e.file_id JOIN projects p ON p.id = f.project_id "
                     "WHERE p.name = ?1 ORDER BY 1",
                     project, &h) != 0)
      return -1;
   if (cr_hash_query(conn,
                     "SELECT 'I:' || i.name FROM file_imports i "
                     "JOIN files f ON f.id = i.file_id JOIN projects p ON p.id = f.project_id "
                     "WHERE p.name = ?1 ORDER BY 1",
                     project, &h) != 0)
      return -1;
   snprintf(out, cap, "%016llx", (unsigned long long)h);
   return 0;
}

int db2_cross_repo_repo_set_hash(char *out, size_t cap)
{
   void *conn = cr_conn();
   if (!conn || !out || cap < 17)
      return -1;
   char err[CR_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT name, trust FROM projects ORDER BY name", err, sizeof(err));
   if (!st)
      return -1;
   uint64_t h = fnv1a_init();
   int step;
   int rc = 0;
   while ((step = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      const char *name = aimee_pg_column_text(st, 0);
      const char *trust = aimee_pg_column_text(st, 1);
      char sym[17] = "";
      if (db2_cross_repo_repo_symbol_hash(name, sym, sizeof(sym)) != 0)
      {
         rc = -1;
         break;
      }
      h = fnv1a_add(h, name);
      h = fnv1a_add(h, trust);
      h = fnv1a_add(h, sym);
   }
   aimee_pg_finalize(st);
   if (rc != 0 || step < 0)
      return -1;
   snprintf(out, cap, "%016llx", (unsigned long long)h);

   /* persist into cross_repo_meta */
   aimee_pg_stmt_t *up = aimee_pg_prepare(
       conn, "UPDATE cross_repo_meta SET repo_set_hash = ?1 WHERE id = 1", err, sizeof(err));
   if (up)
   {
      aimee_pg_bind_text(up, "?1", out);
      (void)aimee_pg_step(up, err, sizeof(err));
      aimee_pg_finalize(up);
   }
   return 0;
}
