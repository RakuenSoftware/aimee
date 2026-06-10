/* DB2 (Postgres) idempotent schema bootstrap.
 * See docs/proposals/accepted/three-db-split-user-shared-vectors.md. */

#include "db_schema.h"
#include "db_postgres.h"
#include "aimee.h" /* EMBED_MAX_DIM */
#include "../schema_data.h"

#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
typedef struct sqlite3 sqlite3;
#else
#include <sqlite3.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AIMEE_DISABLE_DB2_SQLITE_SHIM) && (defined(__GNUC__) || defined(__clang__))
#pragma weak sqlite3_errmsg
#pragma weak sqlite3_exec
#pragma weak sqlite3_free
#endif

static void copy_sqlite_err(char *errbuf, size_t errlen, const char *src)
{
   if (!errbuf || errlen == 0)
      return;
   snprintf(errbuf, errlen, "%s", src ? src : "");
}

/* The DB2 SQLite shim is test/support infrastructure for DB2's Postgres
 * domain APIs. The SQLite-flavoured DB2 schema lives in
 * src/db2/schema_sqlite.sql and is embedded as AIMEE_DB2_SCHEMA_SQLITE_SQL. */

#ifndef AIMEE_DISABLE_DB2_SQLITE_SHIM
static void db2_run_sqlite_migrations(sqlite3 *db)
{
   /* Each statement is independent; duplicate-column / missing-table errors are
    * ignored so legacy and fresh DBs both continue to the canonical schema. */
   static const char *migrations[] = {
       "ALTER TABLE code_embeddings ADD COLUMN body_hash TEXT NOT NULL DEFAULT ''",
       NULL,
   };
   for (int i = 0; migrations[i]; i++)
      sqlite3_exec(db, migrations[i], NULL, NULL, NULL);
}
#endif

/* Replace every occurrence of |token| in |src| with |repl|, returning a
 * heap-allocated copy (caller frees) or NULL on allocation failure. |token|
 * must be non-empty. */
static char *schema_subst(const char *src, const char *token, const char *repl)
{
   size_t tok_len = strlen(token);
   size_t repl_len = strlen(repl);
   if (tok_len == 0)
      return NULL;

   size_t count = 0;
   for (const char *p = src; (p = strstr(p, token)) != NULL; p += tok_len)
      count++;

   /* strlen(src) >= count*tok_len always (matches lie within src), so the
    * subtraction stays non-negative before adding the replacement bytes. */
   size_t out_len = strlen(src) - count * tok_len + count * repl_len;
   char *out = malloc(out_len + 1);
   if (!out)
      return NULL;

   char *w = out;
   for (const char *p = src; *p;)
   {
      if (strncmp(p, token, tok_len) == 0)
      {
         memcpy(w, repl, repl_len);
         w += repl_len;
         p += tok_len;
      }
      else
         *w++ = *p++;
   }
   *w = '\0';
   return out;
}

int db_apply_schema_postgres(void *pg_conn, int embed_dim, char *errbuf, size_t errlen)
{
   if (!pg_conn)
      return -1;

   /* The DB2 schema declares its halfvec embedding columns with the
    * __EMBED_DIM__ placeholder so a deployment can run either embedder
    * (0.6b=1024 or 4b=2560). Substitute the configured dimension here — the one
    * place the schema is applied to Postgres. An out-of-range value falls back
    * to the default 0.6b dimension rather than emitting invalid DDL. */
   if (embed_dim <= 0 || embed_dim > EMBED_MAX_DIM)
      embed_dim = 1024;
   char dimbuf[16];
   snprintf(dimbuf, sizeof(dimbuf), "%d", embed_dim);

   char *sql = schema_subst(AIMEE_DB2_SCHEMA_SQL, "__EMBED_DIM__", dimbuf);
   if (!sql)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "schema dimension substitution failed (out of memory)");
      return -1;
   }
   int rc = aimee_pg_exec(pg_conn, sql, errbuf, errlen);
   free(sql);
   return rc;
}

int db2_apply_schema_sqlite_shim(sqlite3 *db, char *errbuf, size_t errlen)
{
#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
   (void)db;
   copy_sqlite_err(errbuf, errlen, "sqlite shim unavailable");
   return -1;
#else
   if (!db)
      return -1;
   if (!sqlite3_exec || !sqlite3_errmsg || !sqlite3_free)
   {
      copy_sqlite_err(errbuf, errlen, "sqlite shim unavailable");
      return -1;
   }
   db2_run_sqlite_migrations(db);
   char *err = NULL;
   int rc = sqlite3_exec(db, AIMEE_DB2_SCHEMA_SQLITE_SQL, NULL, NULL, &err);
   if (rc != SQLITE_OK)
   {
      copy_sqlite_err(errbuf, errlen, err ? err : sqlite3_errmsg(db));
      sqlite3_free(err);
      return -1;
   }
   return 0;
#endif
}
