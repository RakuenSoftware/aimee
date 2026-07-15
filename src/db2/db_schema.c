/* DB2 (Postgres) idempotent schema bootstrap.
 * See docs/STORAGE_TIERS.md. */

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
       /* Retry backoff: sqlite has no ADD COLUMN IF NOT EXISTS, so the duplicate
        * on an already-migrated shim DB is swallowed by the errors-ignored loop
        * (the postgres side uses IF NOT EXISTS in schema.sql). */
       "ALTER TABLE kb_async_jobs ADD COLUMN next_attempt_at TEXT NOT NULL DEFAULT ''",
       "ALTER TABLE kb_code_unit_jobs ADD COLUMN next_attempt_at TEXT NOT NULL DEFAULT ''",
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
                  "embedding dim mismatch: schema sized %ld but configured %d; serving the new "
                  "dim against the old corpus makes vector search silently return nothing. "
                  "Restore embedding_dim=%ld or run `aimee kb reembed --confirm` (see "
                  "docs/retrieval-stack.md).",
                  recorded, embed_dim, recorded);
      return -1;
   }
   return 0; /* recorded == embed_dim — fresh insert or matching existing row */
}

/* §2a: quiet, read-only companion to record_or_check. Returns the recorded dim
 * only when it parses cleanly into 1..EMBED_MAX_DIM; any other state (no row,
 * empty, non-numeric, trailing junk, non-positive, or out of range) returns 0 so
 * the caller falls through to its configured default. Never sets an error. */
int db2_embedding_dim_get(void *conn)
{
   if (!conn)
      return 0;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT value FROM kb_meta WHERE key = 'schema_embedding_dim'", err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int dim = 0;
   if (step == AIMEE_PG_ROW)
   {
      const char *valtxt = aimee_pg_column_text(st, 0);
      char *endp = NULL;
      long v = (valtxt && *valtxt) ? strtol(valtxt, &endp, 10) : 0;
      if (valtxt && *valtxt && endp && *endp == '\0' && v >= 1 && v <= EMBED_MAX_DIM)
         dim = (int)v;
   }
   aimee_pg_finalize(st);
   return dim;
}

db2_dim_read_t db2_embedding_dim_read(void *conn, int *out)
{
   if (out)
      *out = 0;
   if (!conn)
      return DB2_DIM_ERROR;
   char err[256] = "";
   /* A FRESH DB has no kb_meta yet — it is created by db_apply_schema_postgres,
    * which runs AFTER this read. On real Postgres a SELECT against a missing table
    * errors (at step, not prepare), so a naive read would misreport ERROR and
    * fail-fast the §2b cold path before the schema is ever applied — deadlocking
    * the kb_main retry loop. Probe the catalog first with to_regclass (returns NULL
    * rather than erroring when the table is absent) and report ABSENT, never ERROR.
    * The sqlite test shim always has kb_meta (it pre-applies the schema) and lacks
    * to_regclass, so the guard is skipped there. */
   if (!aimee_pg_is_shim())
   {
      aimee_pg_stmt_t *chk = aimee_pg_prepare(
          conn, "SELECT (to_regclass('kb_meta') IS NOT NULL)::int", err, sizeof(err));
      if (!chk)
         return DB2_DIM_ERROR;
      int exists = 0, ok = 0;
      if (aimee_pg_step(chk, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         ok = 1;
         exists = aimee_pg_column_int(chk, 0);
      }
      aimee_pg_finalize(chk);
      if (!ok)
         return DB2_DIM_ERROR; /* a real query/connection error */
      if (!exists)
         return DB2_DIM_ABSENT; /* fresh DB: kb_meta not created yet */
   }
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT value FROM kb_meta WHERE key = 'schema_embedding_dim'", err, sizeof(err));
   if (!st)
      return DB2_DIM_ERROR; /* kb_meta exists (or shim): a prepare failure is real */
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   db2_dim_read_t rc;
   if (step == AIMEE_PG_ROW)
   {
      const char *valtxt = aimee_pg_column_text(st, 0);
      char *endp = NULL;
      long v = (valtxt && *valtxt) ? strtol(valtxt, &endp, 10) : 0;
      if (valtxt && *valtxt && endp && *endp == '\0' && v >= 1 && v <= EMBED_MAX_DIM)
      {
         if (out)
            *out = (int)v;
         rc = DB2_DIM_FOUND;
      }
      else
         rc = DB2_DIM_ABSENT; /* garbage / out-of-range row: quiet, as §2a */
   }
   else if (step == AIMEE_PG_DONE)
   {
      rc = DB2_DIM_ABSENT; /* no row: the expected fresh-DB signal */
   }
   else
   {
      rc = DB2_DIM_ERROR; /* step error (lost conn etc.): do not misread as absent */
   }
   aimee_pg_finalize(st);
   return rc;
}

/* True if compat_csv (a comma-separated list of "old_id->new_id" transitions)
 * admits the transition old_id -> new_id. Whitespace around tokens is trimmed.
 * unified-llm-container §2: a compat-list entry only EXISTS once an operator has
 * validated the upgrade (cosine >= 0.99 on a fixed probe set vs the prior model,
 * or a same-repo retrain); this function checks membership, the validation is the
 * admission criterion for adding the entry. */
static int compat_admits(const char *compat_csv, const char *old_id, const char *new_id)
{
   if (!compat_csv || !*compat_csv || !old_id || !new_id)
      return 0;
   const char *p = compat_csv;
   while (*p)
   {
      while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
         p++;
      const char *entry = p;
      while (*p && *p != ',' && *p != '\n' && *p != '\r')
         p++;
      const char *entry_end = p; /* [entry, entry_end) is one CSV token */
      const char *arrow = entry;
      while (arrow + 1 < entry_end && !(arrow[0] == '-' && arrow[1] == '>'))
         arrow++;
      if (arrow + 1 < entry_end && arrow[0] == '-' && arrow[1] == '>')
      {
         /* trim spaces around each side */
         const char *ls = entry, *le = arrow;
         while (ls < le && (*ls == ' ' || *ls == '\t'))
            ls++;
         while (le > ls && (le[-1] == ' ' || le[-1] == '\t'))
            le--;
         const char *rs = arrow + 2, *re = entry_end;
         while (rs < re && (*rs == ' ' || *rs == '\t'))
            rs++;
         while (re > rs && (re[-1] == ' ' || re[-1] == '\t'))
            re--;
         if ((size_t)(le - ls) == strlen(old_id) && strncmp(ls, old_id, (size_t)(le - ls)) == 0 &&
             (size_t)(re - rs) == strlen(new_id) && strncmp(rs, new_id, (size_t)(re - rs)) == 0)
            return 1;
      }
   }
   return 0;
}

/* Read kb_meta[key] into out[outlen] (out[0]='\0' if absent). Returns 0 on
 * success (incl. absent), -1 on DB/prepare error. */
static int kb_meta_get(void *conn, const char *key, char *out, size_t outlen)
{
   if (out && outlen)
      out[0] = '\0';
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT value FROM kb_meta WHERE key = ?1", err, sizeof(err));
   if (!st)
      return -1;
   if (aimee_pg_bind_text(st, "?1", key) != 0)
   {
      aimee_pg_finalize(st);
      return -1;
   }
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      if (v && out && outlen)
         snprintf(out, outlen, "%s", v);
   }
   aimee_pg_finalize(st);
   return 0;
}

/* Upsert kb_meta[key] = value (overwriting). Returns 0 / -1. */
static int kb_meta_set(void *conn, const char *key, const char *value)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "INSERT INTO kb_meta (key, value) VALUES (?1, ?2)"
                                          " ON CONFLICT (key) DO UPDATE SET value = ?2",
                                          err, sizeof(err));
   if (!st)
      return -1;
   if (aimee_pg_bind_text(st, "?1", key) != 0 || aimee_pg_bind_text(st, "?2", value) != 0)
   {
      aimee_pg_finalize(st);
      return -1;
   }
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (step == AIMEE_PG_DONE || step == AIMEE_PG_ROW) ? 0 : -1;
}

/* unified-llm-container §2: record/check the EMBEDDER model identity (repo@sha)
 * alongside the dim, so a same-dim different-model swap (pplx-embed 1024 ↔
 * Qwen3-0.6B 1024) is refused rather than silently mixing vector spaces.
 *   - model_id NULL/empty   -> no-op (return 0): a deployment whose embedder
 *     reports no identity (the legacy torch embedder) is unaffected.
 *   - kb_meta.schema_embedder_model_id absent or == model_id -> record / match.
 *   - recorded != model_id, transition admitted by compat_csv -> update + 0.
 *   - recorded != model_id, not admitted -> refuse (-1, remediation set).
 * Keyed re-embed triggers on a model_id change (the migration, §Migration). */
int db2_embedding_model_record_or_check(void *conn, const char *model_id, const char *compat_csv,
                                        char *errbuf, size_t errlen)
{
   if (!conn)
      return -1;
   if (!model_id || !*model_id)
      return 0; /* identity unknown -> guard is a no-op (back-compat) */

   char recorded[160] = "";
   if (kb_meta_get(conn, "schema_embedder_model_id", recorded, sizeof(recorded)) != 0)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "kb_meta read failed for schema_embedder_model_id");
      return -1;
   }
   if (!recorded[0])
   {
      if (kb_meta_set(conn, "schema_embedder_model_id", model_id) != 0)
      {
         if (errbuf && errlen)
            snprintf(errbuf, errlen, "kb_meta write failed for schema_embedder_model_id");
         return -1;
      }
      return 0;
   }
   if (strcmp(recorded, model_id) == 0)
      return 0; /* match */

   if (compat_admits(compat_csv, recorded, model_id))
   {
      if (kb_meta_set(conn, "schema_embedder_model_id", model_id) != 0)
      {
         if (errbuf && errlen)
            snprintf(errbuf, errlen, "kb_meta write failed promoting admitted embedder upgrade");
         return -1;
      }
      return 0; /* admitted same-dim upgrade */
   }

   /* Kept concise so it survives the caller's conventional 256-byte errbuf after
    * both ids are substituted (the full remediation lives in retrieval-stack.md). */
   if (errbuf && errlen)
      snprintf(
          errbuf, errlen,
          "embedder model mismatch: corpus '%s' vs configured '%s' (same dim != same vector "
          "space). Re-embed the corpus or compat-list the transition (docs/retrieval-stack.md).",
          recorded, model_id);
   return -1;
}

/* unified-llm-container §2: record the RERANKER identity + scoring contract. The
 * reranker writes no corpus vectors and there is no persisted score cache, so a
 * swap is safe — this is record-only (drift observability), never a refusal. On a
 * change it refreshes the recorded value (a future score cache would key its
 * invalidation off this). model_id NULL/empty -> no-op. Returns 0 / -1 (DB err). */
int db2_reranker_model_record(void *conn, const char *model_id, const char *contract, char *errbuf,
                              size_t errlen)
{
   if (!conn)
      return -1;
   if (!model_id || !*model_id)
      return 0;
   const char *want_contract = contract ? contract : "";
   /* Skip the writes when the recorded identity already matches, so a steady-state
    * restart incurs no redundant kb_meta writes (mirrors the embedder short-circuit). */
   char rec_id[160] = "", rec_contract[96] = "";
   if (kb_meta_get(conn, "schema_reranker_model_id", rec_id, sizeof(rec_id)) == 0 &&
       kb_meta_get(conn, "schema_reranker_contract", rec_contract, sizeof(rec_contract)) == 0 &&
       strcmp(rec_id, model_id) == 0 && strcmp(rec_contract, want_contract) == 0)
      return 0;
   if (kb_meta_set(conn, "schema_reranker_model_id", model_id) != 0 ||
       kb_meta_set(conn, "schema_reranker_contract", want_contract) != 0)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "kb_meta write failed for reranker identity");
      return -1;
   }
   return 0;
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
   /* §2: record the dim on first apply / refuse a mismatch (kb_meta now exists).
    * The unified-llm-container §2 model-identity guards (embedder + reranker) run
    * in db2_init right after this, where the configured identity globals live —
    * keeping this lower schema layer free of an upward dependency on them. */
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
