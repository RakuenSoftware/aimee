/* DB2 (Postgres) idempotent schema bootstrap.
 * See docs/STORAGE_TIERS.md. */

#include "db_schema.h"
#include "db_postgres.h"
#include "aimee.h" /* EMBED_MAX_DIM */
#include "schema_data.h"

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
 * src/modules/db2/c/schema_sqlite.sql and is embedded as AIMEE_DB2_SCHEMA_SQLITE_SQL. */

#ifndef AIMEE_DISABLE_DB2_SQLITE_SHIM
static int db2_sqlite_name_seen(void *ctx, int argc, char **argv, char **columns)
{
   (void)columns;
   if (ctx && argc > 0 && argv[0])
      *(int *)ctx = 1;
   return 0;
}

static int db2_sqlite_column_seen(void *ctx, int argc, char **argv, char **columns)
{
   (void)columns;
   if (ctx && argc > 1 && argv[1] && strcmp(argv[1], "generation") == 0)
      *(int *)ctx = 1;
   return 0;
}

/* SQLite cannot drop the legacy UNIQUE(project_id,path) constraint in place.
 * Rebuild the shim table once so retained generations can contain the same path.
 * legacy_alter_table keeps child FKs aimed at the replacement `files` table;
 * copying ids preserves every file_id relationship. */
static int db2_sqlite_migrate_file_generations(sqlite3 *db)
{
   int files_exists = 0;
   int has_generation = 0;
   sqlite3_exec(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='files'",
                db2_sqlite_name_seen, &files_exists, NULL);
   if (!files_exists)
      return 0;
   sqlite3_exec(db, "PRAGMA table_info(files)", db2_sqlite_column_seen, &has_generation, NULL);
   if (has_generation)
      return 0;

   const char *sql =
       "PRAGMA foreign_keys=OFF;"
       "PRAGMA legacy_alter_table=ON;"
       "BEGIN IMMEDIATE;"
       "ALTER TABLE files RENAME TO files_generation_legacy;"
       "CREATE TABLE files ("
       " id INTEGER PRIMARY KEY AUTOINCREMENT,"
       " project_id INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,"
       " generation INTEGER NOT NULL DEFAULT 1 CHECK (generation > 0),"
       " path TEXT NOT NULL, purpose TEXT NOT NULL DEFAULT '', hash TEXT NOT NULL DEFAULT '',"
       " scanned_at TEXT NOT NULL, language TEXT NOT NULL DEFAULT '',"
       " vendored INTEGER NOT NULL DEFAULT 0, UNIQUE(project_id,generation,path));"
       "INSERT INTO files(id,project_id,generation,path,purpose,hash,scanned_at,language,vendored)"
       " SELECT f.id,f.project_id,p.current_generation,f.path,f.purpose,f.hash,f.scanned_at,"
       " f.language,f.vendored FROM files_generation_legacy f"
       " JOIN projects p ON p.id=f.project_id;"
       "DROP TABLE files_generation_legacy;"
       "COMMIT;"
       "PRAGMA legacy_alter_table=OFF;"
       "PRAGMA foreign_keys=ON;";
   char *err = NULL;
   int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
   if (rc != SQLITE_OK)
   {
      sqlite3_exec(db, "ROLLBACK; PRAGMA legacy_alter_table=OFF; PRAGMA foreign_keys=ON", NULL,
                   NULL, NULL);
      sqlite3_free(err);
      return -1;
   }
   return 0;
}

static int db2_sqlite_migrate_code_embedding_generations(sqlite3 *db)
{
   int table_exists = 0;
   int projects_exists = 0;
   int has_generation = 0;
   sqlite3_exec(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='code_embeddings'",
                db2_sqlite_name_seen, &table_exists, NULL);
   if (!table_exists)
      return 0;
   sqlite3_exec(db, "PRAGMA table_info(code_embeddings)", db2_sqlite_column_seen, &has_generation,
                NULL);
   if (has_generation)
      return 0;

   char *err = NULL;
   int rc = sqlite3_exec(db,
                         "ALTER TABLE code_embeddings ADD COLUMN generation INTEGER NOT NULL"
                         " DEFAULT 1 CHECK (generation > 0);"
                         "DROP INDEX IF EXISTS idx_code_embeddings_node;"
                         "DROP INDEX IF EXISTS idx_code_embeddings_hash;",
                         NULL, NULL, &err);
   if (rc != SQLITE_OK)
   {
      sqlite3_free(err);
      return -1;
   }
   sqlite3_exec(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='projects'",
                db2_sqlite_name_seen, &projects_exists, NULL);
   if (projects_exists)
      sqlite3_exec(db,
                   "UPDATE code_embeddings SET generation=COALESCE((SELECT current_generation"
                   " FROM projects WHERE name=code_embeddings.project),1)",
                   NULL, NULL, NULL);
   return 0;
}

/* Derived KB rows are retained across detach/re-add just like source index rows.
 * Stamp a legacy table exactly once; unconditional backfills would relabel an
 * intentionally retained generation on every later schema apply. */
static int db2_sqlite_migrate_named_generation(sqlite3 *db, const char *table)
{
   int table_exists = 0;
   int has_generation = 0;
   char sql[512];
   snprintf(sql, sizeof(sql), "SELECT name FROM sqlite_master WHERE type='table' AND name='%s'",
            table);
   sqlite3_exec(db, sql, db2_sqlite_name_seen, &table_exists, NULL);
   if (!table_exists)
      return 0;
   snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
   sqlite3_exec(db, sql, db2_sqlite_column_seen, &has_generation, NULL);
   if (has_generation)
      return 0;

   snprintf(sql, sizeof(sql),
            "ALTER TABLE %s ADD COLUMN generation INTEGER NOT NULL DEFAULT 1"
            " CHECK (generation > 0);"
            "UPDATE %s SET generation=COALESCE((SELECT current_generation FROM projects"
            " WHERE name=%s.project),1)",
            table, table, table);
   char *err = NULL;
   int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
   if (rc != SQLITE_OK)
   {
      sqlite3_free(err);
      return -1;
   }
   return 0;
}

/* These legacy tables encoded generation-independent uniqueness in their
 * table definitions. Adding a generation column is insufficient: a re-added
 * checkout would still update the detached row. Rebuild once so generation is
 * part of the key and preserve ids/state while backfilling the active generation. */
static int db2_sqlite_migrate_generation_keys(sqlite3 *db)
{
   static const struct
   {
      const char *table;
      const char *sql;
   } migrations[] = {
       {"kb_file_index",
        "PRAGMA foreign_keys=OFF;PRAGMA legacy_alter_table=ON;BEGIN IMMEDIATE;"
        "ALTER TABLE kb_file_index RENAME TO kb_file_index_generation_legacy;"
        "CREATE TABLE kb_file_index (id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " project TEXT NOT NULL,generation INTEGER NOT NULL DEFAULT 1 CHECK(generation>0),"
        " file_path TEXT NOT NULL,file_hash TEXT NOT NULL,"
        " ingested_at TEXT NOT NULL DEFAULT(datetime('now')),content TEXT DEFAULT NULL,"
        " UNIQUE(project,generation,file_path));"
        "INSERT INTO kb_file_index(id,project,generation,file_path,file_hash,ingested_at,content)"
        " SELECT k.id,k.project,COALESCE(p.current_generation,1),k.file_path,k.file_hash,"
        " k.ingested_at,k.content FROM kb_file_index_generation_legacy k"
        " LEFT JOIN projects p ON p.name=k.project;"
        "DROP TABLE kb_file_index_generation_legacy;"
        "CREATE INDEX IF NOT EXISTS idx_kb_file_index_project ON kb_file_index(project);COMMIT;"
        "PRAGMA legacy_alter_table=OFF;PRAGMA foreign_keys=ON;"},
       {"kb_code_unit_jobs",
        "PRAGMA foreign_keys=OFF;PRAGMA legacy_alter_table=ON;BEGIN IMMEDIATE;"
        "ALTER TABLE kb_code_unit_jobs RENAME TO kb_code_unit_jobs_generation_legacy;"
        "CREATE TABLE kb_code_unit_jobs (id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " project TEXT NOT NULL DEFAULT '',generation INTEGER NOT NULL DEFAULT 1"
        " CHECK(generation>0),file_path TEXT NOT NULL DEFAULT '',"
        " symbol TEXT NOT NULL DEFAULT '',kind TEXT NOT NULL DEFAULT 'function',"
        " line INTEGER NOT NULL DEFAULT 0,status TEXT NOT NULL DEFAULT 'pending',"
        " attempts INTEGER NOT NULL DEFAULT 0,last_error TEXT NOT NULL DEFAULT '',"
        " claimed_by TEXT NOT NULL DEFAULT '',claimed_at TEXT NOT NULL DEFAULT '',"
        " created_at TEXT NOT NULL DEFAULT(datetime('now')),"
        " updated_at TEXT NOT NULL DEFAULT(datetime('now')),"
        " next_attempt_at TEXT NOT NULL DEFAULT '',"
        " UNIQUE(project,generation,file_path,symbol));"
        "INSERT INTO kb_code_unit_jobs(id,project,generation,file_path,symbol,kind,line,status,"
        " attempts,last_error,claimed_by,claimed_at,created_at,updated_at,next_attempt_at)"
        " SELECT j.id,j.project,COALESCE(p.current_generation,1),j.file_path,j.symbol,j.kind,"
        " j.line,j.status,j.attempts,j.last_error,j.claimed_by,j.claimed_at,j.created_at,"
        " j.updated_at,j.next_attempt_at FROM kb_code_unit_jobs_generation_legacy j"
        " LEFT JOIN projects p ON p.name=j.project;"
        "DROP TABLE kb_code_unit_jobs_generation_legacy;"
        "CREATE INDEX IF NOT EXISTS idx_kb_code_unit_jobs_status"
        " ON kb_code_unit_jobs(status,id);COMMIT;"
        "PRAGMA legacy_alter_table=OFF;PRAGMA foreign_keys=ON;"},
       {"kb_minhash_signatures",
        "PRAGMA foreign_keys=OFF;PRAGMA legacy_alter_table=ON;BEGIN IMMEDIATE;"
        "ALTER TABLE kb_minhash_signatures RENAME TO kb_minhash_generation_legacy;"
        "CREATE TABLE kb_minhash_signatures (project TEXT NOT NULL,"
        " generation INTEGER NOT NULL DEFAULT 1 CHECK(generation>0),file_path TEXT NOT NULL,"
        " file_hash TEXT NOT NULL DEFAULT '',signature_bytes BLOB NOT NULL,"
        " updated_at TEXT NOT NULL DEFAULT(datetime('now')),"
        " PRIMARY KEY(project,generation,file_path));"
        "INSERT INTO kb_minhash_signatures(project,generation,file_path,file_hash,"
        " signature_bytes,updated_at) SELECT s.project,COALESCE(p.current_generation,1),"
        " s.file_path,s.file_hash,s.signature_bytes,s.updated_at"
        " FROM kb_minhash_generation_legacy s LEFT JOIN projects p ON p.name=s.project;"
        "DROP TABLE kb_minhash_generation_legacy;"
        "CREATE INDEX IF NOT EXISTS idx_kb_minhash_signatures_project"
        " ON kb_minhash_signatures(project);COMMIT;"
        "PRAGMA legacy_alter_table=OFF;PRAGMA foreign_keys=ON;"},
       {"kb_lsh_buckets",
        "PRAGMA foreign_keys=OFF;PRAGMA legacy_alter_table=ON;BEGIN IMMEDIATE;"
        "ALTER TABLE kb_lsh_buckets RENAME TO kb_lsh_generation_legacy;"
        "CREATE TABLE kb_lsh_buckets (project TEXT NOT NULL,"
        " generation INTEGER NOT NULL DEFAULT 1 CHECK(generation>0),band INTEGER NOT NULL,"
        " band_hash TEXT NOT NULL,file_path TEXT NOT NULL,"
        " updated_at TEXT NOT NULL DEFAULT(datetime('now')),"
        " PRIMARY KEY(project,generation,band,band_hash,file_path));"
        "INSERT INTO kb_lsh_buckets(project,generation,band,band_hash,file_path,updated_at)"
        " SELECT b.project,COALESCE(p.current_generation,1),b.band,b.band_hash,b.file_path,"
        " b.updated_at FROM kb_lsh_generation_legacy b"
        " LEFT JOIN projects p ON p.name=b.project;"
        "DROP TABLE kb_lsh_generation_legacy;"
        "CREATE INDEX IF NOT EXISTS idx_kb_lsh_buckets_lookup"
        " ON kb_lsh_buckets(project,band,band_hash);COMMIT;"
        "PRAGMA legacy_alter_table=OFF;PRAGMA foreign_keys=ON;"},
       {"css_migration_units",
        "PRAGMA foreign_keys=OFF;PRAGMA legacy_alter_table=ON;BEGIN IMMEDIATE;"
        "ALTER TABLE css_migration_units RENAME TO css_migration_units_generation_legacy;"
        "CREATE TABLE css_migration_units (id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " project TEXT NOT NULL DEFAULT '',generation INTEGER NOT NULL DEFAULT 1"
        " CHECK(generation>0),unit_path TEXT NOT NULL DEFAULT '',"
        " state TEXT NOT NULL DEFAULT 'pending',total_tokens INTEGER NOT NULL DEFAULT 0,"
        " resolved_tokens INTEGER NOT NULL DEFAULT 0,oracle_equivalent INTEGER NOT NULL DEFAULT -1,"
        " note TEXT NOT NULL DEFAULT '',updated_at TEXT NOT NULL DEFAULT '',"
        " UNIQUE(project,generation,unit_path));"
        "INSERT INTO css_migration_units(id,project,generation,unit_path,state,total_tokens,"
        " resolved_tokens,oracle_equivalent,note,updated_at)"
        " SELECT u.id,u.project,COALESCE(p.current_generation,1),u.unit_path,u.state,"
        " u.total_tokens,u.resolved_tokens,u.oracle_equivalent,u.note,u.updated_at"
        " FROM css_migration_units_generation_legacy u LEFT JOIN projects p ON p.name=u.project;"
        "DROP TABLE css_migration_units_generation_legacy;"
        "CREATE INDEX IF NOT EXISTS idx_css_migration_project"
        " ON css_migration_units(project,state);COMMIT;"
        "PRAGMA legacy_alter_table=OFF;PRAGMA foreign_keys=ON;"},
       {"css_render_snapshots",
        "PRAGMA foreign_keys=OFF;PRAGMA legacy_alter_table=ON;BEGIN IMMEDIATE;"
        "ALTER TABLE css_render_snapshots RENAME TO css_render_snapshots_generation_legacy;"
        "CREATE TABLE css_render_snapshots (id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " project TEXT NOT NULL DEFAULT '',generation INTEGER NOT NULL DEFAULT 1"
        " CHECK(generation>0),unit_path TEXT NOT NULL DEFAULT '',phase TEXT NOT NULL DEFAULT '',"
        " snapshot TEXT NOT NULL DEFAULT '',content_hash TEXT NOT NULL DEFAULT '',"
        " captured_at TEXT NOT NULL DEFAULT '',UNIQUE(project,generation,unit_path,phase));"
        "INSERT INTO css_render_snapshots(id,project,generation,unit_path,phase,snapshot,"
        " content_hash,captured_at) SELECT s.id,s.project,COALESCE(p.current_generation,1),"
        " s.unit_path,s.phase,s.snapshot,s.content_hash,s.captured_at"
        " FROM css_render_snapshots_generation_legacy s"
        " LEFT JOIN projects p ON p.name=s.project;"
        "DROP TABLE css_render_snapshots_generation_legacy;"
        "CREATE INDEX IF NOT EXISTS idx_css_render_snapshots_unit"
        " ON css_render_snapshots(project,unit_path);COMMIT;"
        "PRAGMA legacy_alter_table=OFF;PRAGMA foreign_keys=ON;"},
   };

   for (size_t i = 0; i < sizeof(migrations) / sizeof(migrations[0]); i++)
   {
      int table_exists = 0;
      int has_generation = 0;
      char probe[256];
      snprintf(probe, sizeof(probe),
               "SELECT name FROM sqlite_master WHERE type='table' AND name='%s'",
               migrations[i].table);
      sqlite3_exec(db, probe, db2_sqlite_name_seen, &table_exists, NULL);
      if (!table_exists)
         continue;
      snprintf(probe, sizeof(probe), "PRAGMA table_info(%s)", migrations[i].table);
      sqlite3_exec(db, probe, db2_sqlite_column_seen, &has_generation, NULL);
      if (has_generation)
         continue;
      char *err = NULL;
      if (sqlite3_exec(db, migrations[i].sql, NULL, NULL, &err) != SQLITE_OK)
      {
         sqlite3_exec(db, "ROLLBACK;PRAGMA legacy_alter_table=OFF;PRAGMA foreign_keys=ON", NULL,
                      NULL, NULL);
         sqlite3_free(err);
         return -1;
      }
   }
   return 0;
}

static int db2_run_sqlite_migrations(sqlite3 *db)
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
       "ALTER TABLE kb_vault_rewrap_operation ADD COLUMN failure_from_state TEXT",
       "ALTER TABLE projects ADD COLUMN lifecycle_state TEXT NOT NULL DEFAULT 'current'",
       "ALTER TABLE projects ADD COLUMN current_generation INTEGER NOT NULL DEFAULT 1",
       "CREATE TABLE IF NOT EXISTS code_project_aliases ("
       " id INTEGER PRIMARY KEY AUTOINCREMENT,"
       " project_id INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,"
       " alias TEXT NOT NULL UNIQUE, alias_kind TEXT NOT NULL DEFAULT 'checkout',"
       " is_current INTEGER NOT NULL DEFAULT 1 CHECK (is_current IN (0,1)),"
       " first_seen_at TEXT NOT NULL, last_seen_at TEXT NOT NULL)",
       "CREATE INDEX IF NOT EXISTS idx_code_project_alias_project"
       " ON code_project_aliases(project_id, is_current)",
       "CREATE TABLE IF NOT EXISTS code_project_generations ("
       " id INTEGER PRIMARY KEY AUTOINCREMENT,"
       " project_id INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,"
       " generation INTEGER NOT NULL CHECK (generation > 0), root TEXT NOT NULL,"
       " state TEXT NOT NULL DEFAULT 'current'"
       " CHECK (state IN ('current','superseded','detached')),"
       " created_at TEXT NOT NULL, detached_at TEXT NOT NULL DEFAULT '',"
       " UNIQUE(project_id, generation))",
       "CREATE INDEX IF NOT EXISTS idx_code_project_generation_state"
       " ON code_project_generations(project_id, state, generation)",
       "INSERT OR IGNORE INTO code_project_generations"
       " (project_id,generation,root,state,created_at,detached_at)"
       " SELECT id,current_generation,root,lifecycle_state,scanned_at,'' FROM projects",
       "INSERT OR IGNORE INTO code_project_aliases"
       " (project_id,alias,alias_kind,is_current,first_seen_at,last_seen_at)"
       " SELECT id,root,'checkout',CASE WHEN lifecycle_state='current' THEN 1 ELSE 0 END,"
       " scanned_at,scanned_at FROM projects WHERE substr(root,1,1)='/'",
       "ALTER TABLE kb_doc_assets ADD COLUMN project TEXT NOT NULL DEFAULT ''",
       "UPDATE kb_doc_assets SET project=(SELECT MIN(d.project) FROM kb_documents d"
       " WHERE d.file_path=kb_doc_assets.document_key) WHERE project='' AND"
       " (SELECT COUNT(DISTINCT d.project) FROM kb_documents d"
       " WHERE d.file_path=kb_doc_assets.document_key)=1",
       NULL,
   };
   for (int i = 0; migrations[i]; i++)
      sqlite3_exec(db, migrations[i], NULL, NULL, NULL);
   if (db2_sqlite_migrate_file_generations(db) != 0)
      return -1;
   if (db2_sqlite_migrate_code_embedding_generations(db) != 0)
      return -1;
   if (db2_sqlite_migrate_generation_keys(db) != 0)
      return -1;
   if (db2_sqlite_migrate_named_generation(db, "kb_documents") != 0)
      return -1;
   if (db2_sqlite_migrate_named_generation(db, "kb_doc_assets") != 0)
      return -1;
   return 0;
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
 * for a different dim, the existing vector columns are still at the recorded dim,
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

/* The identity the retired builtin lexical embedder recorded. Nothing serves it any
 * more — a kb with no embedder now refuses to start — but corpora created before it
 * was removed still carry it in kb_meta, so the value has to outlive the code. */
#define RETIRED_LEXICAL_SERVING_ID "builtin/lexical-v1"

/* The tables that hold derived vectors. Every one is rebuildable from source kept
 * elsewhere, which is why db2_reembed.c may drop them; here the same set answers a
 * different question — has anything actually been embedded yet.
 *
 * Keep in sync with schema.sql's vector(__EMBED_DIM__) tables (and their
 * schema_sqlite.sql counterparts). A table missing from this list would answer the
 * emptiness question wrongly in the unsafe direction, so adding one is not optional. */
const char *const DB2_DERIVED_VECTOR_TABLES[] = {"kb_embeddings",
                                                 "kb_pdf_embeddings",
                                                 "memory_embeddings",
                                                 "curator_entity_vectors",
                                                 "curator_narrative_vectors",
                                                 "curator_claim_vectors",
                                                 "curator_code_unit_vectors",
                                                 "exemplar_vectors",
                                                 "evidence_vectors",
                                                 "code_embeddings",
                                                 NULL};

/* Does any vector table hold a row?
 *
 * Returns 1 (has vectors), 0 (provably empty), or -1 (could not tell). Callers must
 * treat -1 as "has vectors": an unprovable corpus is not an empty one. */

/* Does `table` exist? 1 yes, 0 no, -1 could not ask. Asked explicitly rather than
 * inferred from a failed read: "the table is absent" and "the table is there but I
 * cannot read it" are opposite answers to the emptiness question, and a failed prepare
 * cannot tell them apart. Treating both as absence let one unreadable table plus one
 * empty table report a corpus as provably empty. */
static int corpus_table_exists(void *conn, const char *table)
{
   char err[256] = "";
   /* Views count as existing. Postgres's to_regclass resolves them, so the shim has to
    * as well or the two backends disagree about what "absent" means — and the answer
    * that matters is "is there something under this name I would have to read", not
    * "is it specifically a table". */
   const char *sql = aimee_pg_is_shim()
                         ? "SELECT name FROM sqlite_master WHERE type IN ('table','view') "
                           "AND name=?1"
                         : "SELECT to_regclass(?1)";
   aimee_pg_stmt_t *probe = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!probe)
      return -1;
   if (aimee_pg_bind_text(probe, "?1", table) != 0)
   {
      aimee_pg_finalize(probe);
      return -1;
   }
   int exists = 0;
   if (aimee_pg_step(probe, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *resolved = aimee_pg_column_text(probe, 0);
      exists = resolved && resolved[0];
   }
   aimee_pg_finalize(probe);
   return exists;
}

static int corpus_has_vectors(void *conn)
{
   for (int i = 0; DB2_DERIVED_VECTOR_TABLES[i]; i++)
   {
      int exists = corpus_table_exists(conn, DB2_DERIVED_VECTOR_TABLES[i]);
      if (exists < 0)
         return -1; /* could not even ask whether it is there */
      if (!exists)
         continue; /* absent, and absence IS proof that it holds nothing */

      char err[256] = "";
      char sql[160];
      snprintf(sql, sizeof(sql), "SELECT 1 FROM %s LIMIT 1", DB2_DERIVED_VECTOR_TABLES[i]);
      aimee_pg_stmt_t *q = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!q)
         return -1; /* it exists and we cannot read it: never call that empty */
      aimee_pg_step_t step = aimee_pg_step(q, err, sizeof(err));
      aimee_pg_finalize(q);
      if (step == AIMEE_PG_ROW)
         return 1;
      if (step != AIMEE_PG_DONE)
         return -1;
   }
   /* Every table was either provably absent or read and found empty. */
   return 0;
}

/* Record/check the embedder's VECTOR-SPACE identity (the gateway's /health
 * serving_id: model + pooling + prefix pair, digested).
 *
 * The dim guard and the model-id guard both miss the two failures that actually
 * happened here. Flipping pooling from `last` to `mean`, and adopting the card's
 * query/document prefixes, each change every vector while the dim and the model name
 * stay put: no error, right width, right name, different space, collapsed recall. This
 * is the guard for that class.
 *
 *   - serving_id NULL/empty -> no-op (0): an endpoint that reports no identity (a
 *     legacy embedder, or a gateway predating the field) must keep working.
 *   - nothing recorded -> record and accept. A corpus embedded before this guard
 *     existed cannot be distinguished from a fresh one, so it adopts the current id;
 *     the drift it could not have detected is the operator's to resolve (the cutover
 *     runbook says re-embed).
 *   - recorded == serving_id -> match.
 *   - recorded is the RETIRED lexical identity AND nothing has been embedded -> adopt
 *     the new identity. Those corpora exist because an unconfigured kb used to serve a
 *     builtin lexical embedder, which recorded itself as the vector space on first
 *     init whether or not anything was ever embedded. The embedder is gone, but the
 *     kb_meta rows are not, and without this every such deployment would refuse to
 *     start the moment it was given the embedder it now requires. With no vectors
 *     written there is no space to mix. Emptiness must be PROVEN — an unreadable
 *     corpus is treated as non-empty.
 *   - recorded != serving_id -> REFUSE. There is no compat list: unlike a model swap,
 *     where cosine agreement can be measured and admitted, a pooling or prefix change
 *     is definitionally a different space. A lexical corpus that HAS vectors is still
 *     refused: the builtin was 384-dim and so is the bundled model, so the dim guard
 *     cannot see that transition and this is the only thing that can.
 */
int db2_embedder_serving_record_or_check(void *conn, const char *serving_id, char *errbuf,
                                         size_t errlen)
{
   if (!conn)
      return -1;
   if (!serving_id || !*serving_id)
      return 0; /* identity unknown -> no-op (back-compat) */

   char recorded[160] = "";
   if (kb_meta_get(conn, "schema_embedder_serving_id", recorded, sizeof(recorded)) != 0)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "kb_meta read failed for schema_embedder_serving_id");
      return -1;
   }
   if (!recorded[0] || strcmp(recorded, serving_id) == 0)
   {
      if (!recorded[0] && kb_meta_set(conn, "schema_embedder_serving_id", serving_id) != 0)
      {
         if (errbuf && errlen)
            snprintf(errbuf, errlen, "kb_meta write failed for schema_embedder_serving_id");
         return -1;
      }
      return 0;
   }

   if (strcmp(recorded, RETIRED_LEXICAL_SERVING_ID) == 0 && corpus_has_vectors(conn) == 0)
   {
      if (kb_meta_set(conn, "schema_embedder_serving_id", serving_id) != 0)
      {
         if (errbuf && errlen)
            snprintf(errbuf, errlen, "kb_meta write failed for schema_embedder_serving_id");
         return -1;
      }
      return 0;
   }

   /* Deliberately does not claim WHICH of model, pooling, prefixes or width changed: the
    * identity is a digest, so the only honest statement is that the spaces differ. Saying
    * "even at the same dim" was wrong the first time a width change hit it. */
   if (errbuf && errlen)
      snprintf(errbuf, errlen,
               "embedder serving identity changed: corpus '%s' vs serving '%s'. The model, "
               "its pooling, its query/document prefixes or its width differ, so the stored "
               "vectors and new queries are not in the same space. Re-embed: aimee kb "
               "reembed (docs/retrieval-stack.md).",
               recorded, serving_id);
   return -1;
}

int db_apply_schema_postgres(void *pg_conn, int embed_dim, char *errbuf, size_t errlen)
{
   if (!pg_conn)
      return -1;

   /* The DB2 schema declares its vector embedding columns with the
    * __EMBED_DIM__ placeholder so a deployment can run an embedder of any
    * supported width. Substitute the configured dimension here — the one place
    * the schema is applied to Postgres.
    *
    * An unusable width is an ERROR, not something to paper over: this layer holds
    * no default (the width is declared once, in config, and reaches db2 via
    * db2_set_embedding_dim_default). Silently substituting one here is how a
    * corpus gets columns sized for an embedder that is not the one running. */
   if (embed_dim <= 0 || embed_dim > EMBED_MAX_DIM)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen,
                  "embedding dimension %d is unusable (expected 1..%d); the deployment's "
                  "width was never supplied to the DB2 layer — check that startup calls "
                  "db2_set_embedding_dim_default(config_embedder_dims_default())",
                  embed_dim, EMBED_MAX_DIM);
      return -1;
   }
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
    * The unified-llm-container §2 model-identity guard (the embedder) runs
    * in db2_init right after this, where the configured identity globals live —
    * keeping this lower schema layer free of an upward dependency on them. */
   /* schema_version + schema_embedding_dim are recorded by schema.sql itself (so any
    * applier — the C path here, or a plain `psql -f schema.sql` migrate — records
    * them), which is what a hardened runtime kb reads to verify a complete, current
    * migration. This C layer keeps the authoritative dim record-or-check (drift
    * guard) above. */
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
   if (db2_run_sqlite_migrations(db) != 0)
   {
      copy_sqlite_err(errbuf, errlen, sqlite3_errmsg(db));
      return -1;
   }
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
