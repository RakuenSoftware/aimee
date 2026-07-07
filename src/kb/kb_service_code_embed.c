/* src/kb/kb_service_code_embed.c: KB-side code embedding refresh.
 *
 * All writes to code_embeddings run through this KB-side module; server and
 * CLI code must request refreshes via KB RPC and must not access DB2/pgvector
 * directly. */

#include "kb_service_code_embed.h"
#include "aimee.h" /* EMBED_MAX_DIM */
#include "db2/code_index_ops.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "../db2/entity_nodes.h"
#include "../db2/pgvec_kb_service.h"
#include "../db2/lifecycle.h"
#include "config.h"
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CE_ERRBUF   256
#define CE_TEXT_CAP 4096
/* Upper bound on the embedding dimension we will buffer. Tied to the global
 * EMBED_MAX_DIM (4000 — the pgvector halfvec index ceiling) so the code-embed
 * buffer + cap track the embed ladder (0.6B=1024 / 4B=2560 / 8B-trunc=4000) and
 * never silently reject or overflow a larger-dim model. */
#define CE_EMBED_MAX_DIM        EMBED_MAX_DIM
#define CE_FALLBACK_MAX_CALLS   5
#define CE_FALLBACK_MAX_IMPORTS 5

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
   static const char *proj_sql = "SELECT id, root FROM projects WHERE name = ?1 LIMIT 1";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, proj_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   int64_t proj_id = -1;
   char project_root[512] = "";
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      proj_id = aimee_pg_column_int64(st, 0);
      const char *root = aimee_pg_column_text(st, 1);
      snprintf(project_root, sizeof(project_root), "%s", root ? root : "");
   }
   aimee_pg_finalize(st);
   if (proj_id < 0)
   {
      /* Project not indexed yet — accept job immediately (no rows to embed). */
      out->accepted = 1;
      snprintf(out->job_id, sizeof(out->job_id), "code-embeddings:%s:0", project);
      return 0;
   }

   /* Collect files to embed. */
   static const char *files_sql = "SELECT id, path, hash FROM files WHERE project_id = ?1";
   st = aimee_pg_prepare(conn, files_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", proj_id);

   typedef struct
   {
      int64_t id;
      char path[512];
      char hash[128];
   } file_row_t;
   file_row_t *rows = NULL;
   int row_count = 0;
   int row_cap = 256;
   rows = (file_row_t *)malloc((size_t)row_cap * sizeof(file_row_t));
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
         file_row_t *tmp = (file_row_t *)realloc(rows, (size_t)row_cap * sizeof(file_row_t));
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
    * dimension as the halfvec code_embeddings column. The earlier hardcoded
    * 384-dim deterministic hash never matched the 1024-d halfvec column, so every
    * upsert failed and no code vectors were ever stored. */
   config_t ce_cfg;
   config_load(&ce_cfg);
   const char *embed_command = config_embedding_command(&ce_cfg, NULL);
   int embed_dim = db2_embedding_dim();
   if (embed_dim <= 0 || embed_dim > CE_EMBED_MAX_DIM)
      embed_dim = 1024;

   pgvec_kb_service_ensure_code_collection(embed_dim);

   int embedded = 0;
   int skipped = 0;
   int batch_num = 0;
   (void)effective_batch; /* batch processing implemented in caller for now */

   for (int i = 0; i < row_count && embedded < effective_max; i++)
   {
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

      /* Return the pool lease before the (slow) embedder round-trip. Each embed
       * is a network call to the embedder; holding a DB2 lease across the whole
       * embed loop — thousands of files on a fresh ingest — trips the pool's 300s
       * stuck-lease ceiling, which is NOT reclaimed and permanently shrinks the
       * pool until it wedges the drain (observed: a large ~/gow ingest froze the
       * curator drain, leaving most projects scanned-but-unembedded). This is the
       * same hazard kb_curator_extract_code already guards before its sidecar/LLM
       * call. The DB ops bracketing the embed (op_record below; node_key/exists on
       * the next iteration) re-acquire lazily via db2_conn(); the loop never
       * touches the initial `conn` after the file rows are read. */
      db2_lease_release_idle();

      /* Embed the deterministic fallback text with the configured embedder
       * (embed_command runs the 0.6B embedder; "builtin" falls back to a stable
       * deterministic vector when no embedder is configured, e.g. in tests). */
      float vec[CE_EMBED_MAX_DIM];
      int dim = memory_embed_text(text, embed_command, vec, embed_dim);
      if (dim != embed_dim)
      {
         db2_code_index_op_record(point_id, project, node_key, rows[i].path, 0,
                                  "code embedding failed (embedder unavailable or dim mismatch)");
         continue;
      }

      /* content_hash and source_hash are intentionally the SAME value here —
       * rows[i].hash, the source file's hash (files.hash) at embed time. They serve
       * different roles: content_hash backs the (project,node_key,content_hash)
       * dedup index, while source_hash is the D7 drift contract (files.hash <>
       * source_hash ⇒ content drift). Code embeds are file-granular today, so the
       * two coincide; node-level hashing later would split them. */
      int up = pgvec_kb_service_code_upsert(point_id, vec, dim, project, node_key, rows[i].path, "",
                                            rows[i].hash, body_hash, rows[i].hash, payload);
      /* Per-chunk replay bookkeeping so a failed embed is retried by
       * `memory repair --reset-stuck`, not orphaned. */
      db2_code_index_op_record(point_id, project, node_key, rows[i].path, up == 0,
                               up == 0 ? NULL : "code vector upsert failed");
      if (up == 0)
      {
         embedded++;
         batch_num++;
      }
   }

   free(rows);
   out->embedded = embedded;
   out->skipped_unchanged = skipped;
   out->accepted = 1;
   snprintf(out->job_id, sizeof(out->job_id), "code-embeddings:%s:%d", project, embedded);
   return 0;
}
