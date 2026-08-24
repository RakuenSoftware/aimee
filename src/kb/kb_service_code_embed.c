/* src/kb/kb_service_code_embed.c: KB-side code embedding refresh.
 *
 * All writes to code_embeddings run through this KB-side module; server and
 * CLI code must request refreshes via KB RPC and must not access DB2/pgvector
 * directly. */

#include "kb_service_code_embed.h"
#include "aimee.h" /* EMBED_MAX_DIM */
#include "modules/db2/c/code_index_ops.h"
#include "modules/db2/c/kb_runtime_state.h" /* db2_kb_runtime_state_get/set — change short-circuit */
#include "../modules/db2/c/db2_internal.h"
#include "../modules/db2/c/db_postgres.h"
#include "../modules/db2/c/entity_nodes.h"
#include "../modules/db2/c/pgvec_kb_service.h"
#include "../modules/db2/c/lifecycle.h"
#include "config.h"
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CE_ERRBUF   256
#define CE_TEXT_CAP 4096
/* Upper bound on the embedding dimension we will buffer. Tied to the global
 * EMBED_MAX_DIM (4000 — the pgvector vector index ceiling) so the code-embed
 * buffer + cap track the embed ladder (0.6B=1024 / 4B=2560 / 8B-trunc=4000) and
 * never silently reject or overflow a larger-dim model. */
#define CE_EMBED_MAX_DIM        EMBED_MAX_DIM
#define CE_FALLBACK_MAX_CALLS   5
#define CE_FALLBACK_MAX_IMPORTS 5

/* Ceiling on rows held in memory per embedder round trip. The batch buffers are
 * dominated by the per-row text (4 KiB) and vector (dim * 4 B), so 256 rows is a
 * few MB at the widest model — bounded regardless of what an operator configures
 * as the batch size. Throughput is flat well below this: measured on a 4-core
 * deployment, batches of 8 already reach the model's ceiling (~33 emb/s vs 13.4
 * unbatched) and larger batches do not improve on it. */
#define CE_EMBED_BATCH_MAX 256

/* One row that needs embedding, held until its batch is flushed. `row_idx` indexes
 * the caller's rows[] array, which outlives every batch, so path/hash are read
 * from there rather than copied again. */
typedef struct
{
   int row_idx;
   int64_t point_id;
   char node_key[GRAPH_ENDPOINT_MAX];
   char body_hash[32];
   char payload[2048];
   char text[CE_TEXT_CAP];
} ce_pending_t;

typedef struct
{
   int64_t id;
   char path[512];
   char hash[128];
} ce_file_row_t;

/* Embed one batch and upsert every vector it produced. Returns the number of rows
 * actually written, and advances *batch_num by the same amount.
 *
 * The batch is an OPTIMIZATION, never a correctness boundary: if the batched call
 * does not return exactly n vectors at the right width it is discarded whole and
 * every row is re-embedded individually. That path is the pre-existing one — it
 * owns the dependency breaker and the per-row error record — so a batch failure
 * degrades to the old behaviour rather than losing rows. */
static int ce_flush_batch(ce_pending_t *pend, int n, const char **texts, float *vecs,
                          const ce_file_row_t *rows, const char *project, const char *embed_command,
                          int embed_dim, int *batch_num)
{
   if (n <= 0)
      return 0;

   for (int i = 0; i < n; i++)
      texts[i] = pend[i].text;

   int wrote = 0;
   if (memory_embed_texts(texts, n, embed_command, EMBED_INPUT_DOCUMENT, vecs, embed_dim) == n)
   {
      wrote = n;
   }
   else
   {
      /* Per-row fallback. A row that fails here is recorded and skipped, exactly as
       * it was before batching existed. */
      for (int i = 0; i < n; i++)
      {
         const ce_file_row_t *r = &rows[pend[i].row_idx];
         float *slot = vecs + (size_t)i * (size_t)embed_dim;
         int dim =
             memory_embed_text(pend[i].text, embed_command, EMBED_INPUT_DOCUMENT, slot, embed_dim);
         if (dim != embed_dim)
         {
            db2_code_index_op_record(pend[i].point_id, project, pend[i].node_key, r->path, 0,
                                     "code embedding failed (embedder unavailable or dim "
                                     "mismatch)");
            pend[i].point_id = -1; /* marks "no vector"; skipped by the upsert below */
            continue;
         }
         wrote++;
      }
   }
   if (wrote == 0)
      return 0;

   int ok = 0;
   for (int i = 0; i < n; i++)
   {
      if (pend[i].point_id < 0)
         continue;
      const ce_file_row_t *r = &rows[pend[i].row_idx];
      /* content_hash and source_hash are intentionally the SAME value here —
       * r->hash, the source file's hash (files.hash) at embed time. They serve
       * different roles: content_hash backs the (project,node_key,content_hash)
       * dedup index, while source_hash is the D7 drift contract (files.hash <>
       * source_hash ⇒ content drift). Code embeds are file-granular today, so the
       * two coincide; node-level hashing later would split them. */
      int up = pgvec_kb_service_code_upsert(pend[i].point_id, vecs + (size_t)i * (size_t)embed_dim,
                                            embed_dim, project, pend[i].node_key, r->path, "",
                                            r->hash, pend[i].body_hash, r->hash, pend[i].payload);
      /* Per-chunk replay bookkeeping so a failed embed is retried by
       * `memory repair --reset-stuck`, not orphaned. */
      db2_code_index_op_record(pend[i].point_id, project, pend[i].node_key, r->path, up == 0,
                               up == 0 ? NULL : "code vector upsert failed");
      if (up == 0)
      {
         ok++;
         (*batch_num)++;
      }
   }
   return ok;
}

/* The project's content signature at |generation|: file count, the ACTIVE embedding
 * dimension, and a digest over every file id:hash. Recorded by a pass that embedded
 * the whole file set, and compared on the next pass to skip an unchanged project.
 *
 * The dimension is part of it on purpose: the same files at a different width are
 * different vectors, so a dim change must invalidate the signature rather than read
 * as "already embedded". */
static void ce_project_signature(void *conn, int64_t proj_id, int64_t generation, char *out,
                                 size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!conn)
      return;
   int sc_dim = db2_embedding_dim();
   if (sc_dim <= 0 || sc_dim > CE_EMBED_MAX_DIM)
      sc_dim = 1024;
   char err[CE_ERRBUF] = "";
   static const char *sig_sql = "SELECT count(*), coalesce(md5(string_agg(id::text || ':' || "
                                "hash, ',' ORDER BY id)), '') "
                                "FROM files WHERE project_id = ?1 AND generation = ?2";
   aimee_pg_stmt_t *sst = aimee_pg_prepare(conn, sig_sql, err, sizeof(err));
   if (!sst)
      return;
   aimee_pg_bind_int64(sst, "?1", proj_id);
   aimee_pg_bind_int64(sst, "?2", generation);
   if (aimee_pg_step(sst, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      int64_t fcount = aimee_pg_column_int64(sst, 0);
      const char *fhash = aimee_pg_column_text(sst, 1);
      snprintf(out, out_len, "%lld:%d:%s", (long long)fcount, sc_dim, fhash ? fhash : "");
   }
   aimee_pg_finalize(sst);
}

int kb_code_embed_project_fully_embedded(const char *project)
{
   if (!project || !project[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[CE_ERRBUF] = "";
   static const char *proj_sql = "SELECT id, current_generation FROM projects"
                                 " WHERE name = ?1 AND lifecycle_state='current' LIMIT 1";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, proj_sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", project);
   int64_t proj_id = -1;
   int64_t generation = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      proj_id = aimee_pg_column_int64(st, 0);
      generation = aimee_pg_column_int64(st, 1);
   }
   aimee_pg_finalize(st);
   /* Not indexed is not embedded. An unknown project answers 0 rather than
    * "vacuously complete", which would let curation start on nothing. */
   if (proj_id < 0)
      return 0;

   char sig_now[160] = "";
   ce_project_signature(conn, proj_id, generation, sig_now, sizeof(sig_now));
   if (!sig_now[0])
      return 0;

   char sig_key[320];
   snprintf(sig_key, sizeof(sig_key), "code_embed_sig:%s", project);
   char stored[160] = "";
   if (db2_kb_runtime_state_get(sig_key, stored, sizeof(stored)) != 0)
      return 0;
   return stored[0] && strcmp(stored, sig_now) == 0;
}

static unsigned long long ce_fnv1a_update(unsigned long long h, unsigned char c)
{
   h ^= (unsigned long long)c;
   h *= 1099511628211ULL;
   return h;
}

static int ce_path_uses_hash_comments(const char *rel_path)
{
   const char *dot = rel_path ? strrchr(rel_path, '.') : NULL;
   if (!dot)
      return 0;
   return strcmp(dot, ".py") == 0 || strcmp(dot, ".rb") == 0 || strcmp(dot, ".sh") == 0 ||
          strcmp(dot, ".bash") == 0 || strcmp(dot, ".zsh") == 0 || strcmp(dot, ".yaml") == 0 ||
          strcmp(dot, ".yml") == 0 || strcmp(dot, ".toml") == 0;
}

static int ce_path_uses_slash_comments(const char *rel_path)
{
   const char *dot = rel_path ? strrchr(rel_path, '.') : NULL;
   if (!dot)
      return 0;
   return strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0 || strcmp(dot, ".cc") == 0 ||
          strcmp(dot, ".cpp") == 0 || strcmp(dot, ".cxx") == 0 || strcmp(dot, ".hpp") == 0 ||
          strcmp(dot, ".hh") == 0 || strcmp(dot, ".java") == 0 || strcmp(dot, ".js") == 0 ||
          strcmp(dot, ".jsx") == 0 || strcmp(dot, ".ts") == 0 || strcmp(dot, ".tsx") == 0 ||
          strcmp(dot, ".go") == 0 || strcmp(dot, ".rs") == 0 || strcmp(dot, ".cs") == 0 ||
          strcmp(dot, ".kt") == 0 || strcmp(dot, ".swift") == 0;
}

void kb_code_embed_normalized_file_body_hash(const char *root, const char *rel_path, char out[32])
{
   if (!out)
      return;
   out[0] = '\0';
   if (!root || !rel_path)
      return;

   char path[1024];
   int n = snprintf(path, sizeof(path), "%s/%s", root, rel_path);
   if (n < 0 || (size_t)n >= sizeof(path))
      return;
   FILE *f = fopen(path, "rb");
   if (!f)
      return;

   /* Lightweight heuristic normalization, not a language parser: strips common
    * comments and collapses whitespace, but does not understand string/char
    * literals. Good enough for advisory clone grouping; symbol-level parsers can
    * replace it when code-unit rows become parser-backed. */
   unsigned long long h = 1469598103934665603ULL;
   int c;
   int in_ws = 0;
   int line_prefix_ws = 1;
   int hash_comments = ce_path_uses_hash_comments(rel_path);
   int slash_comments = ce_path_uses_slash_comments(rel_path);
   int emitted = 0;
   while ((c = fgetc(f)) != EOF)
   {
      if (slash_comments && c == '/')
      {
         int next = fgetc(f);
         if (next == '/')
         {
            while ((c = fgetc(f)) != EOF && c != '\n')
               ;
            in_ws = 1;
            line_prefix_ws = 1;
            continue;
         }
         if (next == '*')
         {
            int prev = 0;
            while ((c = fgetc(f)) != EOF)
            {
               if (prev == '*' && c == '/')
                  break;
               prev = c;
            }
            in_ws = 1;
            continue;
         }
         if (next != EOF)
            ungetc(next, f);
      }
      if (hash_comments && c == '#' && line_prefix_ws)
      {
         while ((c = fgetc(f)) != EOF && c != '\n')
            ;
         in_ws = 1;
         line_prefix_ws = 1;
         continue;
      }
      if (isspace((unsigned char)c))
      {
         in_ws = 1;
         if (c == '\n' || c == '\r')
            line_prefix_ws = 1;
         continue;
      }
      if (in_ws)
      {
         h = ce_fnv1a_update(h, ' ');
         in_ws = 0;
      }
      h = ce_fnv1a_update(h, (unsigned char)c);
      emitted = 1;
      line_prefix_ws = 0;
   }
   fclose(f);
   if (emitted)
      snprintf(out, 32, "%016llx", h);
}

static int ce_file_line_count(const char *root, const char *rel_path)
{
   if (!root || !rel_path)
      return 0;
   char path[1024];
   int n = snprintf(path, sizeof(path), "%s/%s", root, rel_path);
   if (n < 0 || (size_t)n >= sizeof(path))
      return 0;
   FILE *f = fopen(path, "rb");
   if (!f)
      return 0;
   int lines = 0;
   int saw_any = 0;
   int last = '\n';
   int c;
   while ((c = fgetc(f)) != EOF)
   {
      saw_any = 1;
      if (c == '\n')
         lines++;
      last = c;
   }
   fclose(f);
   if (saw_any && last != '\n')
      lines++;
   return lines;
}

int kb_code_embed_build_fallback_text(const char *project, const char *file_path, int64_t file_id,
                                      char *out, size_t cap)
{
   if (!project || !file_path || !out || cap == 0)
      return -1;
   memset(out, 0, cap);

   /* Start with file path. */
   int written = snprintf(out, cap, "file:%s:%s", project, file_path);
   if (written < 0 || (size_t)written >= cap)
      return written;

   if (file_id <= 0)
      return written;

   void *conn = db2_conn();
   if (!conn)
      return written;
   char err[CE_ERRBUF] = "";

   /* Append up to 5 definitions. */
   {
      static const char *sql = "SELECT name FROM terms WHERE file_id = ?1 AND kind IN"
                               " ('definition','function','method','class','struct') LIMIT 5";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", file_id);
         while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            const char *name = aimee_pg_column_text(st, 0);
            if (name && (size_t)written < cap - 2)
            {
               int r = snprintf(out + written, cap - (size_t)written, " %s", name);
               if (r > 0)
                  written += r;
            }
         }
         aimee_pg_finalize(st);
      }
   }

   /* Append up to CE_FALLBACK_MAX_CALLS outgoing call names. */
   {
      static const char *sql = "SELECT callee FROM code_calls WHERE file_id = ?1"
                               " AND callee != '' ORDER BY line LIMIT 5";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", file_id);
         int calls = 0;
         while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW &&
                calls < CE_FALLBACK_MAX_CALLS)
         {
            const char *name = aimee_pg_column_text(st, 0);
            if (name && (size_t)written < cap - 2)
            {
               int r = snprintf(out + written, cap - (size_t)written, " calls:%s", name);
               if (r > 0)
                  written += r;
               calls++;
            }
         }
         aimee_pg_finalize(st);
      }
   }

   /* Append up to CE_FALLBACK_MAX_IMPORTS import names. */
   {
      static const char *sql = "SELECT name FROM file_imports WHERE file_id = ?1 LIMIT 5";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", file_id);
         int imports = 0;
         while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW &&
                imports < CE_FALLBACK_MAX_IMPORTS)
         {
            const char *name = aimee_pg_column_text(st, 0);
            if (name && (size_t)written < cap - 2)
            {
               int r = snprintf(out + written, cap - (size_t)written, " import:%s", name);
               if (r > 0)
                  written += r;
               imports++;
            }
         }
         aimee_pg_finalize(st);
      }
   }

   return written;
}

int kb_code_embed_refresh(const char *project, const char *scope, const char **paths,
                          int path_count, int batch_size, int max_points, int dry_run,
                          kb_code_embed_result_t *out)
{
   if (!project || !*project || !out)
      return -1;

   memset(out, 0, sizeof(*out));
   snprintf(out->project, sizeof(out->project), "%s", project);
   snprintf(out->scope, sizeof(out->scope), "%s", scope ? scope : "changed_files");
   out->dry_run = dry_run;
   snprintf(out->writer, sizeof(out->writer), "kb_service");

   int effective_batch = (batch_size > 0) ? batch_size : 128;
   int effective_max = (max_points > 0) ? max_points : 5000;

   void *conn = db2_conn();
   if (!conn)
   {
      /* No DB available — accept as a no-op (e.g., test env). */
      out->accepted = 1;
      snprintf(out->job_id, sizeof(out->job_id), "code-embeddings:%s:0", project);
      return 0;
   }
   char err[CE_ERRBUF] = "";

   /* Get project id. */
   static const char *proj_sql = "SELECT id, root, current_generation FROM projects"
                                 " WHERE name = ?1 AND lifecycle_state='current' LIMIT 1";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, proj_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   int64_t proj_id = -1;
   int64_t generation = 0;
   char project_root[512] = "";
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      proj_id = aimee_pg_column_int64(st, 0);
      const char *root = aimee_pg_column_text(st, 1);
      snprintf(project_root, sizeof(project_root), "%s", root ? root : "");
      generation = aimee_pg_column_int64(st, 2);
   }
   aimee_pg_finalize(st);
   if (proj_id < 0)
   {
      /* Project not indexed yet — accept job immediately (no rows to embed). */
      out->accepted = 1;
      snprintf(out->job_id, sizeof(out->job_id), "code-embeddings:%s:0", project);
      return 0;
   }

   /* Per-project change short-circuit for the default "changed_files" pass. The
    * drain calls this for EVERY project EVERY poll; without a cheap early-out it
    * loads all files and checks each against the vector store — thousands of ops
    * per poll, holding a pool connection long enough to trip the stuck-lease
    * reaper (the "flapping" pool member) and starving the deep curator. The
    * git-SHA short-circuit in the scan handler is never recorded for detached
    * (thin-client) workspaces, so nothing else stops the re-scan. Compute a
    * signature over the project's files + the active embed dim and skip the whole
    * pass when it matches the last fully-embedded signature. Only for the implicit
    * changed_files scope (paths==NULL); explicit path lists / "all" always run. */
   char sig_now[160] = "";
   if (!paths && !dry_run && strcmp(out->scope, "changed_files") == 0)
   {
      ce_project_signature(conn, proj_id, generation, sig_now, sizeof(sig_now));
      if (sig_now[0])
      {
         char sig_key[320];
         snprintf(sig_key, sizeof(sig_key), "code_embed_sig:%s", project);
         char stored_sig[160] = "";
         if (db2_kb_runtime_state_get(sig_key, stored_sig, sizeof(stored_sig)) == 0 &&
             strcmp(stored_sig, sig_now) == 0)
         {
            /* Nothing changed since the last fully-embedded pass — skip entirely. */
            out->accepted = 1;
            snprintf(out->job_id, sizeof(out->job_id), "code-embeddings:%s:0", project);
            return 0;
         }
      }
   }

   /* Collect files to embed. */
   static const char *files_sql =
       "SELECT id, path, hash FROM files WHERE project_id = ?1 AND generation = ?2";
   st = aimee_pg_prepare(conn, files_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", proj_id);
   aimee_pg_bind_int64(st, "?2", generation);

   ce_file_row_t *rows = NULL;
   int row_count = 0;
   int row_cap = 256;
   rows = (ce_file_row_t *)malloc((size_t)row_cap * sizeof(ce_file_row_t));
   if (!rows)
   {
      aimee_pg_finalize(st);
      return -1;
   }
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      /* Apply path filter if provided. */
      const char *p = aimee_pg_column_text(st, 1);
      if (path_count > 0 && paths)
      {
         int match = 0;
         for (int i = 0; i < path_count; i++)
            if (paths[i] && p && strcmp(paths[i], p) == 0)
            {
               match = 1;
               break;
            }
         if (!match)
            continue;
      }
      if (row_count >= effective_max)
         break;
      if (row_count >= row_cap)
      {
         row_cap *= 2;
         ce_file_row_t *tmp =
             (ce_file_row_t *)realloc(rows, (size_t)row_cap * sizeof(ce_file_row_t));
         if (!tmp)
            break;
         rows = tmp;
      }
      rows[row_count].id = aimee_pg_column_int64(st, 0);
      const char *path = aimee_pg_column_text(st, 1);
      const char *hash = aimee_pg_column_text(st, 2);
      snprintf(rows[row_count].path, sizeof(rows[row_count].path), "%s", path ? path : "");
      snprintf(rows[row_count].hash, sizeof(rows[row_count].hash), "%s", hash ? hash : "");
      row_count++;
   }
   aimee_pg_finalize(st);
   out->estimated_points = row_count;

   if (dry_run)
   {
      free(rows);
      out->accepted = 1;
      snprintf(out->job_id, sizeof(out->job_id), "code-embeddings:%s:%d", project, row_count);
      return 0;
   }

   /* Embed each code unit with the configured embedder (pipeline stage 2 — the
    * 0.6B model by default) at the deployment's embedding dimension, the same
    * dimension as the vector code_embeddings column. The earlier hardcoded
    * 384-dim deterministic hash never matched the 1024-d vector column, so every
    * upsert failed and no code vectors were ever stored. */
   const char *embed_command = config_embedder_command_current(NULL);
   int embed_dim = db2_embedding_dim();
   if (embed_dim <= 0 || embed_dim > CE_EMBED_MAX_DIM)
      embed_dim = 1024;

   pgvec_kb_service_ensure_code_collection(embed_dim);

   int embedded = 0;
   int skipped = 0;
   int batch_num = 0;

   /* ONE EMBEDDER ROUND TRIP PER BATCH, NOT PER FILE.
    *
    * This loop used to call memory_embed_text() per file and discard
    * effective_batch entirely. The embedder serves ~2000 vectors/min when texts
    * arrive batched and ~800 when they arrive one at a time, and a corpus is tens
    * of thousands of vectors -- so the per-file shape made a ~15 minute ingest
    * take hours, which is what made per-workspace embedding look impractical.
    *
    * Rows are collected into a pending batch, embedded in one call, then upserted.
    * Everything that decides WHETHER a row needs embedding (node_key, hashes, the
    * exists-by-hash skip) still happens per row before the batch is filled, so the
    * skip path is unchanged and unchanged files still cost no embedder work. */
   int cap = effective_batch;
   if (cap < 1)
      cap = 1;
   if (cap > CE_EMBED_BATCH_MAX)
      cap = CE_EMBED_BATCH_MAX;

   ce_pending_t *pend = calloc((size_t)cap, sizeof(*pend));
   float *vecs = calloc((size_t)cap * (size_t)embed_dim, sizeof(float));
   const char **texts = calloc((size_t)cap, sizeof(*texts));
   if (!pend || !vecs || !texts)
   {
      free(pend);
      free(vecs);
      free(texts);
      free(rows);
      return -1;
   }
   int pending = 0;

   for (int i = 0; i < row_count && embedded < effective_max; i++)
   {
      /* Return the pool lease at the top of every iteration so this loop never
       * pins a connection across more than one file's work. Two shapes would
       * otherwise trip the pool's 300s stuck-lease ceiling and wedge the drain:
       *  - the embed round-trip (memory_embed_text below), one slow network call
       *    per file on a fresh ingest; and
       *  - the STEADY STATE, where every file is already embedded so the loop only
       *    computes node_key + checks exists-by-hash and `continue`s — thousands of
       *    checks per poll, every poll, all under one held lease.
       * The per-file DB lookups re-acquire lazily via db2_conn(); no-op inside an
       * explicit lease scope. Mirrors kb_curator_extract_code's guard. */
      db2_lease_release_idle();

      char node_key[GRAPH_ENDPOINT_MAX];
      if (db2_entity_node_key_file(project, rows[i].path, node_key, sizeof(node_key)) != 0)
         continue;

      char body_hash[32];
      kb_code_embed_normalized_file_body_hash(project_root, rows[i].path, body_hash);
      int line_count = ce_file_line_count(project_root, rows[i].path);
      if (!body_hash[0] && line_count > 0)
         snprintf(body_hash, sizeof(body_hash), "%s", rows[i].hash);

      /* Skip unchanged only when both file content and normalized body hash are
       * current. Upgraded stores with blank legacy body_hash intentionally
       * re-embed once so clone grouping is backfilled. */
      if (rows[i].hash[0] &&
          pgvec_kb_service_code_exists_by_hash(project, node_key, rows[i].hash, body_hash))
      {
         skipped++;
         continue;
      }

      /* Build deterministic fallback text (no LLM required). */
      char text[CE_TEXT_CAP];
      kb_code_embed_build_fallback_text(project, rows[i].path, rows[i].id, text, sizeof(text));
      if (!text[0])
      {
         /* An empty text is not embeddable and would fail the batch for every
          * other row in it. Record it and move on, as the per-file path did. */
         db2_code_index_op_record(rows[i].id, project, node_key, rows[i].path, 0,
                                  "code embedding text empty");
         continue;
      }

      /* files.id is a global IDENTITY primary key — already unique across every
       * project — so it is the point id directly. The previous
       * `rows[i].id + proj_id * 1000000` offset could collide once file ids
       * exceeded ~1M (e.g. id 1,500,000 in one project and id 500,000 in
       * another both map to point 2,500,000), silently overwriting another
       * project's code vector and code_index_ops row. */
      int64_t point_id = rows[i].id;

      /* Build payload JSON. node_key (<=512) + file_path (<=512) +
       * content_hash (<=128) + project can exceed a 1 KiB buffer; size it
       * generously and guard truncation so a malformed JSON payload is never
       * upserted into the pgvector point. */
      char payload[2048];
      int plen = snprintf(payload, sizeof(payload),
                          "{\"project\":\"%s\",\"node_key\":\"%s\",\"file_path\":\"%s\","
                          "\"content_hash\":\"%s\",\"body_hash\":\"%s\",\"line_count\":%d}",
                          project, node_key, rows[i].path, rows[i].hash, body_hash, line_count);
      if (plen < 0 || (size_t)plen >= sizeof(payload))
      {
         db2_code_index_op_record(point_id, project, node_key, rows[i].path, 0,
                                  "code payload too large to encode");
         continue;
      }

      /* Queue for the next embedder round trip. */
      pend[pending].row_idx = i;
      pend[pending].point_id = point_id;
      snprintf(pend[pending].node_key, sizeof(pend[pending].node_key), "%s", node_key);
      memcpy(pend[pending].body_hash, body_hash, sizeof(pend[pending].body_hash));
      memcpy(pend[pending].payload, payload, sizeof(pend[pending].payload));
      snprintf(pend[pending].text, sizeof(pend[pending].text), "%s", text);
      pending++;

      if (pending == cap)
      {
         embedded += ce_flush_batch(pend, pending, texts, vecs, rows, project, embed_command,
                                    embed_dim, &batch_num);
         pending = 0;
      }
   }

   if (pending > 0)
   {
      embedded += ce_flush_batch(pend, pending, texts, vecs, rows, project, embed_command,
                                 embed_dim, &batch_num);
      pending = 0;
   }

   free(pend);
   free(vecs);
   free(texts);

   /* Record the fully-embedded signature so the next poll skips this project when
    * nothing changed — but only when the WHOLE file set was loaded this pass. A
    * load capped at effective_max leaves files unprocessed, so it must re-run
    * (and its signature stays unrecorded / stale until it fully catches up). */
   if (sig_now[0] && row_count < effective_max)
   {
      char sig_key[320];
      snprintf(sig_key, sizeof(sig_key), "code_embed_sig:%s", project);
      db2_kb_runtime_state_set(sig_key, sig_now);
   }

   free(rows);
   out->embedded = embedded;
   out->skipped_unchanged = skipped;
   out->accepted = 1;
   snprintf(out->job_id, sizeof(out->job_id), "code-embeddings:%s:%d", project, embedded);
   return 0;
}
