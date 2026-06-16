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

/* embedder-runtime-fetch-autodim §2: record the embedding dim the schema was
 * sized at, and REFUSE a later mismatch. kb_meta.schema_embedding_dim is written
 * once (the first apply) and is then authoritative: if a subsequent apply is asked
 * for a different dim, the existing halfvec columns are still at the recorded dim,
 * so proceeding would silently embed queries at one dim against a corpus at
 * another (search returns nothing). Refuse instead, with a remediation message.
 * Returns 0 (recorded or matches), -1 (mismatch / DB error -> errbuf set).
 * Uses aimee_pg_* so it works against both Postgres and the sqlite test shim. */
int db2_embedding_dim_record_or_check(void *conn, int embed_dim, char *errbuf, size_t errlen)
{
   if (!conn)
      return -1;
   /* A non-positive dim must never be recorded as authoritative: once written it
    * would "match" forever and lock the guard into an unrecoverable state. */
   if (embed_dim <= 0)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "invalid embedding dim: %d (must be > 0)", embed_dim);
      return -1;
   }

   /* Atomic record-or-read in a single statement — no SELECT/INSERT TOCTOU window.
    * On a fresh row this inserts embed_dim; on an existing row the DO UPDATE writes
    * the value back unchanged so RETURNING still yields the authoritative recorded
    * dim. Two racing bootstraps with different dims therefore both observe the one
    * committed value: the loser sees a mismatch and is refused (the old DO NOTHING
    * form silently swallowed the loser's write and returned success). */
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO kb_meta (key, value) VALUES ('schema_embedding_dim', ?1)"
                        " ON CONFLICT (key) DO UPDATE SET value = kb_meta.value"
                        " RETURNING value",
                        err, sizeof(err));
   if (!st)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "kb_meta upsert prepare failed: %s", err);
      return -1;
   }
   char dimtxt[16];
   snprintf(dimtxt, sizeof(dimtxt), "%d", embed_dim);
   if (aimee_pg_bind_text(st, "?1", dimtxt) != 0)
   {
      aimee_pg_finalize(st);
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "kb_meta bind failed");
      return -1;
   }
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step != AIMEE_PG_ROW)
   {
      /* DONE (no row) or ERR — either way we did not learn the authoritative dim,
       * so refuse rather than fall through and assume "first apply". */
      aimee_pg_finalize(st);
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "kb_meta upsert failed: %s", err[0] ? err : "no row returned");
      return -1;
   }
   const char *valtxt = aimee_pg_column_text(st, 0);
   char *endp = NULL;
   long recorded = (valtxt && *valtxt) ? strtol(valtxt, &endp, 10) : 0;
   int corrupt = (!valtxt || !*valtxt || (endp && *endp != '\0') || recorded <= 0);
   aimee_pg_finalize(st);

   if (corrupt)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen,
                  "kb_meta.schema_embedding_dim is corrupt or non-numeric; repair the row "
                  "manually to the schema's true dimension (expected a positive integer).");
      return -1;
   }
   if (recorded != embed_dim)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen,
                  "embedding dim mismatch: schema recorded %ld but configured %d. The halfvec "
                  "columns are sized at %ld; serving at %d would make vector search silently "
                  "return nothing. Restore embedding_dim=%ld, or re-embed at the new dim "
                  "(aimee kb reembed --confirm).",
                  recorded, embed_dim, recorded, embed_dim, recorded);
      return -1;
   }
   return 0; /* recorded == embed_dim — fresh insert or matching existing row */
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
   if (rc != 0)
      return rc;
   /* §2: record the dim on first apply / refuse a mismatch (kb_meta now exists). */
   return db2_embedding_dim_record_or_check(pg_conn, embed_dim, errbuf, errlen);
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
