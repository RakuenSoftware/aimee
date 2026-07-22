#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/kb_payload.h"
#include "db2/db_postgres.h"
#include "db2/kb_service_backend.h"
#include "db2/memory_query.h"
#include "db2/vector_index_ops.h"
#include "db2/kb_runtime_state.h" /* db2_kb_purge_fence_active: ingest fence checks */
#include "db2/sketch.h"
#include "kb_vectors.h"
#include "kb_curator_notify.h"
#include "kb_features.h"
#include "kb_ranker.h"
#include "kb_detect.h"
#include "kb_bandit.h"
#include "kb_bandit_registry.h"
#include "db2/bandit.h"
#endif
#include "headers/sketch.h"
#include "kb.h"
#include "kb_fusion.h"
#include "kb_neardup.h"
#include "memory.h"
#include "kb_doc_hash.h"
#include "log.h"
#include "dstr.h"
#include "lifecycle.h"
#include <dirent.h>
#include <errno.h>
#ifndef AIMEE_WINDOWS
#include <fnmatch.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define KB_ERRBUF 256

#if defined(AIMEE_DB2_DISABLED)
int kb_build(const char *root_path, const char *project, const char *embedding_cmd,
             int force_rebuild, kb_stats_t *stats_out)
{
   (void)root_path;
   (void)project;
   (void)embedding_cmd;
   (void)force_rebuild;
   if (stats_out)
      memset(stats_out, 0, sizeof(*stats_out));
   return -1;
}

int kb_update(const char *root_path, const char *project, const char *embedding_cmd,
              kb_stats_t *stats_out)
{
   (void)root_path;
   (void)project;
   (void)embedding_cmd;
   if (stats_out)
      memset(stats_out, 0, sizeof(*stats_out));
   return -1;
}

char *kb_search(const char *project, const char *query, const char *embedding_cmd, int max_results)
{
   (void)project;
   (void)query;
   (void)embedding_cmd;
   (void)max_results;
   return safe_strdup("error: knowledge base storage is owned by aimee-kb");
}

char *kb_search_json(const char *project, const char *query, const char *embedding_cmd,
                     int max_results)
{
   (void)project;
   (void)query;
   (void)embedding_cmd;
   (void)max_results;
   return safe_strdup("{\"error\":\"knowledge base storage is owned by aimee-kb\"}");
}

void kb_resolve_project(const char *project, const char *root_path, char *out, size_t out_len)
{
   if (project && project[0])
   {
      snprintf(out, out_len, "%s", project);
      return;
   }

   char path[MAX_PATH_LEN];
   if (root_path && root_path[0])
      snprintf(path, sizeof(path), "%s", root_path);
   else if (!getcwd(path, sizeof(path)))
      path[0] = '\0';

   const char *base = strrchr(path, '/');
   snprintf(out, out_len, "%s", base ? base + 1 : path);
}

int kb_async_enabled(void)
{
   return 0;
}

int kb_extract_convention_candidates(void)
{
   return 0;
}

#else

static int estimate_tokens(const char *text, size_t len)
{
   return (int)((len + 3) / 4);
}

/* Compute SHA-256-like hex digest of a file (using simple FNV-64 as a fast hash).
 * Not cryptographic — purely for change detection. */
static void file_hash(const char *path, char *out, size_t out_len)
{
   FILE *f = fopen(path, "rb");
   if (!f)
   {
      snprintf(out, out_len, "0000000000000000");
      return;
   }
   uint64_t h = 14695981039346656037ULL;
   unsigned char buf[4096];
   size_t nr;
   while ((nr = fread(buf, 1, sizeof(buf), f)) > 0)
   {
      for (size_t i = 0; i < nr; i++)
      {
         h ^= (uint64_t)buf[i];
         h *= 1099511628211ULL;
      }
   }
   fclose(f);
   snprintf(out, out_len, "%016llx", (unsigned long long)h);
}

/* ------------------------------------------------------------------ */
/* Glob matching                                                        */
/* ------------------------------------------------------------------ */

#ifdef AIMEE_WINDOWS
#define KB_FNM_PATHNAME 1

static int kb_glob_match_impl(const char *pattern, const char *text, int flags)
{
   while (*pattern)
   {
      if (*pattern == '*')
      {
         while (pattern[1] == '*')
            pattern++;
         pattern++;
         if (!*pattern)
            return !(flags & KB_FNM_PATHNAME) || strchr(text, '/') == NULL;
         for (const char *t = text;; t++)
         {
            if (kb_glob_match_impl(pattern, t, flags))
               return 1;
            if (!*t || ((flags & KB_FNM_PATHNAME) && *t == '/'))
               break;
         }
         return 0;
      }
      if (*pattern == '?')
      {
         if (!*text || ((flags & KB_FNM_PATHNAME) && *text == '/'))
            return 0;
         pattern++;
         text++;
         continue;
      }
      if (*pattern != *text)
         return 0;
      pattern++;
      text++;
   }
   return *text == '\0';
}

static int kb_fnmatch(const char *pattern, const char *text, int flags)
{
   return kb_glob_match_impl(pattern, text, flags) ? 0 : 1;
}
#else
#define KB_FNM_PATHNAME FNM_PATHNAME

static int kb_fnmatch(const char *pattern, const char *text, int flags)
{
   return fnmatch(pattern, text, flags);
}
#endif

/* Split a comma-separated glob list into individual patterns.
 * Returns the number of patterns written to out[] (max max_patterns). */
static int split_glob_list(const char *list, char out[][KB_GLOB_MAX], int max_patterns)
{
   if (!list || !list[0])
      return 0;
   int n = 0;
   const char *p = list;
   while (*p && n < max_patterns)
   {
      const char *comma = strchr(p, ',');
      size_t len = comma ? (size_t)(comma - p) : strlen(p);
      if (len > 0 && len < KB_GLOB_MAX)
      {
         memcpy(out[n], p, len);
         out[n][len] = '\0';
         /* Trim whitespace */
         while (out[n][0] == ' ')
            memmove(out[n], out[n] + 1, strlen(out[n]));
         size_t l2 = strlen(out[n]);
         while (l2 > 0 && out[n][l2 - 1] == ' ')
            out[n][--l2] = '\0';
         if (out[n][0])
            n++;
      }
      if (!comma)
         break;
      p = comma + 1;
   }
   return n;
}

/* Match path (relative to root) against a glob pattern.
 * Supports ** for path components. */
static int glob_match(const char *pattern, const char *rel_path)
{
   /* First try direct fnmatch */
   if (kb_fnmatch(pattern, rel_path, KB_FNM_PATHNAME) == 0)
      return 1;
   /* Try matching just the filename portion */
   const char *slash = strrchr(rel_path, '/');
   if (slash && kb_fnmatch(pattern, slash + 1, 0) == 0)
      return 1;
   /* Handle ** by stripping it and retrying */
   if (strncmp(pattern, "**/", 3) == 0)
   {
      if (kb_fnmatch(pattern + 3, rel_path, KB_FNM_PATHNAME) == 0)
         return 1;
      /* Also try matching against any path suffix */
      const char *rp = rel_path;
      while (rp)
      {
         if (kb_fnmatch(pattern + 3, rp, KB_FNM_PATHNAME) == 0)
            return 1;
         const char *next = strchr(rp, '/');
         rp = next ? next + 1 : NULL;
      }
   }
   return 0;
}

/* Return 1 if rel_path matches any of the include patterns and none of the excludes. */
static int should_index_file(const char *rel_path, char includes[][KB_GLOB_MAX], int n_inc,
                             char excludes[][KB_GLOB_MAX], int n_exc)
{
   for (int i = 0; i < n_exc; i++)
   {
      if (glob_match(excludes[i], rel_path))
         return 0;
   }
   if (n_inc == 0)
      return 1;
   for (int i = 0; i < n_inc; i++)
   {
      if (glob_match(includes[i], rel_path))
         return 1;
   }
   return 0;
}

/* ------------------------------------------------------------------ */
/* File walking                                                         */
/* ------------------------------------------------------------------ */

#define MAX_FILES        4096

typedef struct
{
   char path[MAX_PATH_LEN];     /* absolute path */
   char rel_path[MAX_PATH_LEN]; /* relative to root */
} file_entry_t;

/* Recursively collect files matching include/exclude patterns.
 * Returns count of files found (up to max). */
static int collect_files(const char *root, const char *rel, char includes[][KB_GLOB_MAX], int n_inc,
                         char excludes[][KB_GLOB_MAX], int n_exc, file_entry_t *out, int max)
{
   char dir_path[MAX_PATH_LEN];
   if (rel[0])
      snprintf(dir_path, sizeof(dir_path), "%s/%s", root, rel);
   else
      snprintf(dir_path, sizeof(dir_path), "%s", root);

   DIR *d = opendir(dir_path);
   if (!d)
      return 0;

   int count = 0;
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL && count < max)
   {
      if (ent->d_name[0] == '.')
         continue;

      char abs_path[MAX_PATH_LEN];
      snprintf(abs_path, sizeof(abs_path), "%s/%s", dir_path, ent->d_name);

      char rel_path[MAX_PATH_LEN];
      if (rel[0])
         snprintf(rel_path, sizeof(rel_path), "%s/%s", rel, ent->d_name);
      else
         snprintf(rel_path, sizeof(rel_path), "%s", ent->d_name);

      struct stat st;
      if (stat(abs_path, &st) != 0)
         continue;

      if (S_ISDIR(st.st_mode))
      {
         /* Check if the directory itself is excluded */
         char dir_rel[MAX_PATH_LEN];
         snprintf(dir_rel, sizeof(dir_rel), "%s/", rel_path);
         int excluded = 0;
         for (int i = 0; i < n_exc; i++)
         {
            if (glob_match(excludes[i], rel_path) || glob_match(excludes[i], dir_rel))
            {
               excluded = 1;
               break;
            }
         }
         if (!excluded)
            count += collect_files(root, rel_path, includes, n_inc, excludes, n_exc, out + count,
                                   max - count);
      }
      else if (S_ISREG(st.st_mode))
      {
         if (should_index_file(rel_path, includes, n_inc, excludes, n_exc))
         {
            snprintf(out[count].path, sizeof(out[count].path), "%s", abs_path);
            snprintf(out[count].rel_path, sizeof(out[count].rel_path), "%s", rel_path);
            count++;
         }
      }
   }
   closedir(d);
   return count;
}

/* ------------------------------------------------------------------ */
/* Markdown chunking                                                    */
/* ------------------------------------------------------------------ */

/* MAX_CHUNKS_PER_FILE / MAX_HEADING_LEN / CHUNK_BUF_SIZE and text_chunk_t now
 * live in kb.h (shared with kb_ingest_workers.c's document-ingest entry points). */
#define KB_MINHASH_LIMIT 64

/* Detect markdown heading level: returns 1-6 or 0 if not a heading. */
static int heading_level(const char *line)
{
   int n = 0;
   while (line[n] == '#')
      n++;
   if (n > 0 && n <= 6 && (line[n] == ' ' || line[n] == '\t'))
      return n;
   return 0;
}

/* Extract heading text after the leading '#' characters. */
static void heading_text(const char *line, int level, char *out, size_t out_len)
{
   const char *p = line + level;
   while (*p == ' ' || *p == '\t')
      p++;
   size_t len = strlen(p);
   while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' '))
      len--;
   if (len >= out_len)
      len = out_len - 1;
   memcpy(out, p, len);
   out[len] = '\0';
}

/* Build heading path string by tracking h1/h2/h3 stack.
 * heading_stack[0]=h1, [1]=h2, [2]=h3 */
static void update_heading_path(char heading_stack[][MAX_HEADING_LEN], const char *heading_text_str,
                                int level, char *path_out, size_t path_len)
{
   /* Store at index level-1 */
   int idx = level - 1;
   if (idx < 0 || idx >= 3)
      return;
   snprintf(heading_stack[idx], MAX_HEADING_LEN, "%s", heading_text_str);
   /* Clear deeper levels */
   for (int i = idx + 1; i < 3; i++)
      heading_stack[i][0] = '\0';

   /* Build path from non-empty levels */
   path_out[0] = '\0';
   size_t pos = 0;
   for (int i = 0; i < 3; i++)
   {
      if (!heading_stack[i][0])
         continue;
      if (pos > 0 && pos < path_len - 4)
      {
         memcpy(path_out + pos, " > ", 3);
         pos += 3;
      }
      size_t len = strlen(heading_stack[i]);
      if (pos + len < path_len - 1)
      {
         memcpy(path_out + pos, heading_stack[i], len);
         pos += len;
      }
   }
   path_out[pos] = '\0';
}

/* Flush the current chunk buffer into chunks[] if non-empty. */
static int flush_chunk(text_chunk_t *chunks, int n_chunks, int max_chunks, const char *heading_path,
                       char *buf, int buf_tokens, int line_start, int line_end)
{
   if (n_chunks >= max_chunks)
      return n_chunks;
   /* Trim trailing whitespace from buf */
   size_t blen = strlen(buf);
   while (blen > 0 && (buf[blen - 1] == '\n' || buf[blen - 1] == '\r' || buf[blen - 1] == ' '))
      blen--;
   if (blen == 0)
      return n_chunks;
   buf[blen] = '\0';

   snprintf(chunks[n_chunks].heading_path, sizeof(chunks[n_chunks].heading_path), "%s",
            heading_path);
   snprintf(chunks[n_chunks].content, sizeof(chunks[n_chunks].content), "%s", buf);
   chunks[n_chunks].line_start = line_start;
   chunks[n_chunks].line_end = line_end;
   chunks[n_chunks].token_count = buf_tokens;
   return n_chunks + 1;
}

/* Read a file and split it into chunks. Returns number of chunks produced. */
/* Chunk a heading-structured text stream (markdown/prose) into `chunks`. The
 * caller owns `f` (so the same logic serves both a file on disk and an
 * in-memory document). */
int chunk_stream(FILE *f, text_chunk_t *chunks, int max_chunks)
{
   char heading_stack[3][MAX_HEADING_LEN];
   memset(heading_stack, 0, sizeof(heading_stack));
   char heading_path[MAX_HEADING_LEN] = "";

   char buf[CHUNK_BUF_SIZE] = "";
   size_t buf_pos = 0;
   int buf_tokens = 0;
   int chunk_line_start = 1;
   int line_num = 0;
   int n_chunks = 0;

   char line[4096];
   while (fgets(line, sizeof(line), f) != NULL)
   {
      line_num++;
      size_t line_len = strlen(line);
      int lvl = heading_level(line);

      if (lvl > 0 && lvl <= 3)
      {
         /* Flush current buffer at heading boundary */
         if (buf_tokens > 0)
         {
            n_chunks = flush_chunk(chunks, n_chunks, max_chunks, heading_path, buf, buf_tokens,
                                   chunk_line_start, line_num - 1);
            buf[0] = '\0';
            buf_pos = 0;
            buf_tokens = 0;
         }
         /* Update heading context */
         char htxt[MAX_HEADING_LEN];
         heading_text(line, lvl, htxt, sizeof(htxt));
         update_heading_path(heading_stack, htxt, lvl, heading_path, sizeof(heading_path));
         chunk_line_start = line_num;
         /* Include the heading line in the new chunk */
         if (buf_pos + line_len < CHUNK_BUF_SIZE - 1)
         {
            memcpy(buf + buf_pos, line, line_len);
            buf_pos += line_len;
            buf[buf_pos] = '\0';
            buf_tokens += estimate_tokens(line, line_len);
         }
      }
      else
      {
         /* Append line to current buffer */
         if (buf_pos + line_len < CHUNK_BUF_SIZE - 1)
         {
            memcpy(buf + buf_pos, line, line_len);
            buf_pos += line_len;
            buf[buf_pos] = '\0';
            buf_tokens += estimate_tokens(line, line_len);
         }

         /* Split on chunk size limit */
         if (buf_tokens >= KB_DEFAULT_CHUNK_SIZE)
         {
            n_chunks = flush_chunk(chunks, n_chunks, max_chunks, heading_path, buf, buf_tokens,
                                   chunk_line_start, line_num);
            /* Keep last CHUNK_OVERLAP tokens as overlap for next chunk */
            int keep_chars = KB_DEFAULT_CHUNK_OVERLAP * 4;
            if ((int)buf_pos > keep_chars)
            {
               memmove(buf, buf + buf_pos - (size_t)keep_chars, (size_t)keep_chars);
               buf_pos = (size_t)keep_chars;
               buf[buf_pos] = '\0';
               buf_tokens = KB_DEFAULT_CHUNK_OVERLAP;
            }
            else
            {
               buf[0] = '\0';
               buf_pos = 0;
               buf_tokens = 0;
            }
            chunk_line_start = line_num;
         }
      }
   }

   /* Flush remaining buffer */
   if (buf_tokens > 0)
      n_chunks = flush_chunk(chunks, n_chunks, max_chunks, heading_path, buf, buf_tokens,
                             chunk_line_start, line_num);

   return n_chunks < max_chunks ? n_chunks : max_chunks;
}

static int chunk_file(const char *path, text_chunk_t *chunks, int max_chunks)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   int n = chunk_stream(f, chunks, max_chunks);
   fclose(f);
   return n;
}

/* ------------------------------------------------------------------ */
/* Database helpers                                                     */
/* ------------------------------------------------------------------ */

/* Insert a new chunk row.  kb_documents has no UNIQUE constraint on
 * (project, file_path, chunk_index), so this is a plain INSERT
 * (DELETE-then-INSERT semantics: callers invoke delete_file_chunks
 * for the file before re-inserting any chunks).  RETURNING id keeps
 * backend-specific last-insert state inside DB2.  Returns the new
 * document id, or -1. */

/* Forward an embedding to pgvector for runtime dense search. The
 * conn parameter is unused — kept gone from the signature to mirror the
 * kb_service_backend equivalent. */
int sync_vector_embedding(int64_t doc_id, const float *vec, int dim)
{
   if (!vec || dim <= 0)
      return 0;
   char *payload_json = db2_kb_build_document_payload(doc_id);
   if (!payload_json)
   {
      db2_vector_index_op_record(doc_id, pgvec_kb_vector_collection_name(), 0, 0,
                                 "payload build failed");
      return -1;
   }
   int rc = pgvec_kb_vector_upsert_document(doc_id, vec, dim, payload_json);
   free(payload_json);
   db2_vector_index_op_record(doc_id, pgvec_kb_vector_collection_name(), 0, rc == 0,
                              rc ? "upsert failed" : NULL);
   return rc;
}

const char *kb_effective_embedding_cmd(const char *embedding_cmd)
{
   return config_embedding_command(NULL, embedding_cmd);
}

/* pgvector owns vector bytes. This hook exists to keep the sync/async call
 * shape explicit while the actual pgvector upsert is batched below. */
int accept_generated_embedding(int64_t doc_id, const float *vec, int dim)
{
   (void)doc_id;
   (void)vec;
   (void)dim;
   return 0;
}

/* ── Purge-fence write discipline (webchat-project-lifecycle slice 2) ──
 * EVERY project-scoped ingest write runs inside an explicit transaction that
 * takes the project advisory guard and re-checks the generation fence before
 * the write(s): an ingest that began pre-fence cannot commit stale rows once
 * the fence lands (the guard serializes the check+commit against the fence
 * publish). Guard failure is fail closed. Returns 1 proceed (transaction
 * open), 0 fenced / guard failed (nothing open), -1 transaction error. */
int kb_purge_fenced_txn_begin(const char *project)
{
   if (db2_kb_txn_begin() != 0)
      return -1;
   if (project && project[0] &&
       (db2_kb_purge_txn_guard(project) != 0 || db2_kb_purge_fence_active(project)))
   {
      db2_kb_txn_rollback();
      LOG_WARN("kb_build", "purge fence active for project '%s': dropping ingest write", project);
      return 0;
   }
   return 1;
}

/* Commit a kb_purge_fenced_txn_begin transaction. 0 on success, -1 (rolled
 * back) on commit failure. */
int kb_purge_fenced_txn_commit(void)
{
   if (db2_kb_txn_commit() != 0)
   {
      db2_kb_txn_rollback();
      return -1;
   }
   return 0;
}

/* File-index upsert behind the fenced-transaction discipline (the file index
 * is a purge-matrix store). Returns the upsert rc, -1 when dropped. */
int kb_file_index_upsert_fenced(const char *project, const char *file_path, const char *hash,
                                const char *content)
{
   if (kb_purge_fenced_txn_begin(project) != 1)
      return -1;
   int rc = db2_kb_file_index_upsert(project, file_path, hash, content);
   if (kb_purge_fenced_txn_commit() != 0)
      rc = -1;
   return rc;
}

/* Single-point vector upsert (the dimension-mismatch fallback and the doc
 * ingest worker) behind the fenced-transaction discipline. */
int kb_sync_vector_embedding_fenced(const char *project, int64_t doc_id, const float *vec, int dim)
{
   if (kb_purge_fenced_txn_begin(project) != 1)
      return -1;
   int rc = sync_vector_embedding(doc_id, vec, dim);
   if (kb_purge_fenced_txn_commit() != 0)
      rc = -1;
   return rc;
}

/* Delete all chunks for a given file path in a project.
 *
 * Caps at 1024 ids per call; a single source file producing >1024
 * chunks is far past the chunker's MAX_CHUNKS_PER_FILE (256), so the
 * cap is comfortably above any realistic value.
 *
 * The chunk/vector/minhash removals run in one guarded, fenced transaction;
 * returns 0 committed, -1 dropped (purge fence active / txn failure) —
 * callers must then stop processing that file. */
int delete_file_chunks(const char *project, const char *file_path)
{
   /* Invalidate curator artifacts derived from this file before its chunks go
    * away; the post-ingest queue then re-extracts the (changed) source, and a
    * best-effort push notifies a subscribed server. Runs OUTSIDE the guarded
    * transaction: artifacts are retained by purge, and the broadcast is a
    * network call that must not hold the advisory lock. */
   int curator_stale = db2_curator_invalidate_doc(project, file_path);

   if (kb_purge_fenced_txn_begin(project) != 1)
      return -1;
   int64_t ids[1024];
   int n_ids = db2_kb_documents_list_chunk_ids_for_file(project, file_path, ids,
                                                        (int)(sizeof(ids) / sizeof(ids[0])));
   for (int i = 0; i < n_ids; i++)
   {
      pgvec_kb_vector_delete_point(ids[i]);
      db2_vector_index_op_remove(ids[i]);
   }
   db2_kb_documents_delete_for_file(project, file_path);
   db2_sketch_minhash_signature_delete(project, file_path);
   int rc = kb_purge_fenced_txn_commit();

   kb_curator_invalidation_broadcast(project, file_path, curator_stale);
   return rc;
}

/* ------------------------------------------------------------------ */
/* Async embedding queue                                                */
/* ------------------------------------------------------------------ */

void kb_async_make_embed_text(const char *heading_path, const char *content, char *out,
                              size_t out_len)
{
   if (heading_path && heading_path[0])
      snprintf(out, out_len, "%s\n%s", heading_path, content ? content : "");
   else
      snprintf(out, out_len, "%s", content ? content : "");
}

int kb_async_enabled(void)
{
   const char *env = getenv("AIMEE_MEMORY_COGNIFY_ASYNC_ENABLED");
   if (env && env[0])
   {
      if (strcmp(env, "1") == 0 || strcmp(env, "true") == 0 || strcmp(env, "yes") == 0 ||
          strcmp(env, "on") == 0)
         return 1;
      if (strcmp(env, "0") == 0 || strcmp(env, "false") == 0 || strcmp(env, "no") == 0 ||
          strcmp(env, "off") == 0)
         return 0;
   }

   config_t cfg;
   if (config_load(&cfg) == 0)
      return cfg.memory_cognify_async_enabled ? 1 : 0;
   return 0;
}

/* ------------------------------------------------------------------ */
/* Build / Update                                                       */
/* ------------------------------------------------------------------ */

static void kb_flush_batch(const char *project, int64_t *ids, float *vecs, int dim, char **payloads,
                           int *count)
{
   if (*count <= 0)
      return;
   /* Guarded transaction around the pgvector batch write with the
    * generation-fence check inside it (webchat-project-lifecycle slice 2):
    * the advisory guard serializes check+commit against the fence publish,
    * so a batch mid-flight past its enqueue check still cannot land vectors
    * for a project being purged. */
   int t = kb_purge_fenced_txn_begin(project);
   int fenced = (t == 0);
   int rc = -1;
   if (t == 1)
   {
      rc = pgvec_kb_vector_upsert_document_batch(ids, vecs, dim, (const char *const *)payloads,
                                                 *count);
      if (kb_purge_fenced_txn_commit() != 0)
         rc = -1;
   }
   for (int i = 0; i < *count; i++)
   {
      int ok = (rc == 0);
      const char *err = rc ? (fenced ? "purge fence active" : "batch upsert failed") : NULL;
      db2_vector_index_op_record(ids[i], pgvec_kb_vector_collection_name(), 0, ok, err);
      free(payloads[i]);
      payloads[i] = NULL;
   }
   *count = 0;
}

/* Minhash signature upsert behind the shared fenced-transaction discipline:
 * the near-dup store is part of the purge matrix. Returns the upsert's rc,
 * or -1 when fenced / guard-failed (write dropped). */
static int kb_minhash_upsert_fenced(const char *project, const char *rel_path, const char *hash,
                                    const sketch_minhash_t *sig)
{
   if (kb_purge_fenced_txn_begin(project) != 1)
      return -1;
   int rc = db2_sketch_minhash_signature_upsert(project, rel_path, hash, sig);
   if (kb_purge_fenced_txn_commit() != 0)
      rc = -1;
   return rc;
}

static void kb_load_build_sketches(const char *project, int force_rebuild, sketch_bloom_t *bloom,
                                   sketch_count_min_t *count_min, sketch_hll_t *hll)
{
   if (force_rebuild)
   {
      sketch_bloom_init(bloom);
      sketch_count_min_init(count_min);
      sketch_hll_init(hll);
      return;
   }
   int bloom_rc = db2_sketch_bloom_load(bloom, "kb_project", project ? project : "", "file_hash");
   int count_min_rc =
       db2_sketch_count_min_load(count_min, "kb_project", project ? project : "", "file_hash");
   int hll_rc = db2_sketch_hll_load(hll, "kb_project", project ? project : "", "file_path");
   if (bloom_rc < 0 || count_min_rc < 0 || hll_rc < 0)
      LOG_WARN("kb_build",
               "project=%s: sketch load failed; continuing with empty advisory sketches",
               project ? project : "?");
}

/* Shared state threaded through the per-file indexing step. kb_build_or_update
 * owns the underlying locals/buffers (alloc + teardown); this context borrows
 * them by pointer so kb_process_one_file can read and mutate them across the
 * file loop without behavior change. */
typedef struct
{
   const char *project;
   const char *effective_cmd;
   int force_rebuild;
   int async_enabled;
   int n_files;
   int kb_progress_every;
   int kb_expected_dim;
   int kb_batch_size;
   file_entry_t *files;
   text_chunk_t *chunks;
   sketch_bloom_t *bloom;
   sketch_count_min_t *count_min;
   sketch_hll_t *hll;
   kb_stats_t *stats;
   int64_t *kb_batch_ids;
   float *kb_batch_vecs;
   char **kb_batch_payloads;
   int *kb_batch_count;
} kb_build_file_ctx_t;

/* Index a single file: skip checks (mtime / content-hash / bloom-dedupe),
 * LSH near-duplicate handling, chunk insertion, and synchronous-or-async
 * embedding with batched pgvector upserts. Mutates the shared sketches, batch
 * buffer, and stats counters through `c`. */
static void kb_process_one_file(kb_build_file_ctx_t *c, int fi)
{
   if (fi > 0 && fi % c->kb_progress_every == 0)
      LOG_INFO("kb_build", "project=%s: %d/%d files processed (indexed=%d, skipped=%d, chunks=%d)",
               c->project ? c->project : "?", fi, c->n_files, c->stats->files_indexed,
               c->stats->files_skipped, c->stats->chunks_added);

   /* Mtime pre-check: skip expensive read+hash for files unchanged since last ingest */
   if (!c->force_rebuild)
   {
      struct stat fst;
      char findex_hash[64] = "";
      char findex_ingested[64] = "";
      if (stat(c->files[fi].path, &fst) == 0 &&
          db2_kb_file_index_get(c->project, c->files[fi].rel_path, findex_hash, sizeof(findex_hash),
                                findex_ingested, sizeof(findex_ingested)) == 1 &&
          findex_ingested[0])
      {
         struct tm tm_ingest;
         memset(&tm_ingest, 0, sizeof(tm_ingest));
         strptime(findex_ingested, "%Y-%m-%d %H:%M:%S", &tm_ingest);
         time_t ingested_t = timegm(&tm_ingest);
         if (fst.st_mtime < ingested_t)
         {
            c->stats->files_skipped++;
            return;
         }
      }
   }

   char hash[32];
   file_hash(c->files[fi].path, hash, sizeof(hash));
   uint64_t h64 = (uint64_t)strtoull(hash, NULL, 16);

   /* Check if file needs re-indexing */
   if (!c->force_rebuild)
   {
      char stored[32];
      if (db2_kb_documents_get_stored_hash(c->project, c->files[fi].rel_path, stored,
                                           sizeof(stored)) == 0)
      {
         if (strcmp(stored, hash) == 0)
         {
            kb_file_index_upsert_fenced(c->project, c->files[fi].rel_path, hash, NULL);
            c->stats->files_skipped++;
            return;
         }
      }
   }

   if (!c->force_rebuild && sketch_bloom_test_hash(c->bloom, h64))
   {
      char dup_path[MAX_PATH_LEN] = "";
      int dup_exists = db2_kb_documents_hash_exists(c->project, hash, dup_path, sizeof(dup_path));
      if (dup_exists == 1)
      {
         if (delete_file_chunks(c->project, c->files[fi].rel_path) != 0)
            return; /* purge fence: drop this file */
         db2_sketch_minhash_row_t dup_sig;
         if (dup_path[0] && db2_sketch_minhash_signature_get(c->project, dup_path, &dup_sig) == 1)
            kb_minhash_upsert_fenced(c->project, c->files[fi].rel_path, hash, &dup_sig.signature);
         kb_file_index_upsert_fenced(c->project, c->files[fi].rel_path, hash, NULL);
         LOG_INFO("kb_build",
                  "bloom-dedupe: project=%s path=%s hash=%s matched=%s; skipping duplicate",
                  c->project ? c->project : "?", c->files[fi].rel_path, hash,
                  dup_path[0] ? dup_path : "?");
         c->stats->files_skipped++;
         return;
      }
      if (dup_exists < 0)
         LOG_WARN("kb_build", "project=%s path=%s: bloom cross-check failed; indexing normally",
                  c->project ? c->project : "?", c->files[fi].rel_path);
   }

   /* Remove old chunks for this file */
   if (delete_file_chunks(c->project, c->files[fi].rel_path) != 0)
      return; /* purge fence: drop this file */

   /* Chunk the file */
   int n_chunks = chunk_file(c->files[fi].path, c->chunks, MAX_CHUNKS_PER_FILE);
   if (n_chunks == 0)
      return;

   sketch_minhash_t minhash_sig;
   sketch_minhash_init(&minhash_sig);
   for (int ci = 0; ci < n_chunks; ci++)
      sketch_minhash_add_text(&minhash_sig, c->chunks[ci].content, 5);
   db2_sketch_minhash_row_t lsh_rows[KB_MINHASH_LIMIT];
   int n_lsh =
       db2_sketch_minhash_candidate_list(c->project, &minhash_sig, lsh_rows, KB_MINHASH_LIMIT);
   double near_jaccard = 0.0;
   char near_path[MAX_PATH_LEN] = "";
   for (int mi = 0; mi < n_lsh; mi++)
   {
      if (strcmp(lsh_rows[mi].file_path, c->files[fi].rel_path) == 0)
         continue;
      int band_hit = 0;
      for (int band = 0; band < SKETCH_LSH_BANDS && !band_hit; band++)
         band_hit = sketch_lsh_band_hash(&minhash_sig, band) ==
                    sketch_lsh_band_hash(&lsh_rows[mi].signature, band);
      double jaccard =
          band_hit ? sketch_minhash_jaccard(&minhash_sig, &lsh_rows[mi].signature) : 0.0;
      if (jaccard >= 0.75)
      {
         LOG_INFO("kb_build", "lsh-dedupe: project=%s path=%s near_duplicate=%s jaccard=%.3f",
                  c->project ? c->project : "?", c->files[fi].rel_path, lsh_rows[mi].file_path,
                  jaccard);
         if (jaccard > near_jaccard)
         {
            near_jaccard = jaccard;
            snprintf(near_path, sizeof(near_path), "%s", lsh_rows[mi].file_path);
         }
      }
   }
   if (near_jaccard >= 0.75)
   {
      if (kb_minhash_upsert_fenced(c->project, c->files[fi].rel_path, hash, &minhash_sig) != 0)
      {
         LOG_WARN("kb_build", "project=%s path=%s: minhash signature save failed",
                  c->project ? c->project : "?", c->files[fi].rel_path);
      }
      else
      {
         sketch_bloom_add_hash(c->bloom, h64);
         sketch_count_min_add_hash(c->count_min, h64, 1);
         sketch_hll_add_hash(c->hll,
                             sketch_fnv1a(c->files[fi].rel_path, strlen(c->files[fi].rel_path)));
         if (kb_file_index_upsert_fenced(c->project, c->files[fi].rel_path, hash, NULL) == 0)
         {
            kb_neardup_propose(c->project, c->files[fi].rel_path, near_path, near_jaccard);
            c->stats->files_skipped++;
            return;
         }
         LOG_WARN("kb_build", "project=%s path=%s: file index save failed",
                  c->project ? c->project : "?", c->files[fi].rel_path);
      }
   }

   /* Insert chunks and link neighbours — ALL rows for this file in ONE
    * guarded, fenced transaction (phase 1). Embeddings run afterwards
    * (phase 2): the advisory guard must never be held across the slow
    * external embedder calls. */
   int64_t *doc_ids = malloc((size_t)n_chunks * sizeof(int64_t));
   if (!doc_ids)
      return;
   if (kb_purge_fenced_txn_begin(c->project) != 1)
   {
      free(doc_ids); /* purge fence: drop this file */
      return;
   }
   int64_t prev_doc_id = 0;
   for (int ci = 0; ci < n_chunks; ci++)
   {
      doc_ids[ci] = db2_kb_documents_insert_chunk(c->project, c->files[fi].rel_path, hash, ci,
                                                  c->chunks[ci].heading_path,
                                                  c->chunks[ci].line_start, c->chunks[ci].line_end,
                                                  c->chunks[ci].content, c->chunks[ci].token_count);
      if (doc_ids[ci] < 0)
         continue;
      db2_kb_documents_link_neighbours(doc_ids[ci], prev_doc_id);
      prev_doc_id = doc_ids[ci];
      c->stats->chunks_added++;
   }
   if (kb_purge_fenced_txn_commit() != 0)
   {
      free(doc_ids);
      return;
   }

   /* Phase 2: embed each committed chunk (sync or async). */
   for (int ci = 0; ci < n_chunks; ci++)
   {
      int64_t doc_id = doc_ids[ci];
      if (doc_id < 0)
         continue;

      /* Generate embedding synchronously or enqueue it for async draining. */
      if (c->async_enabled)
      {
         if (db2_kb_async_enqueue("embed_raw", doc_id, c->project) == 0)
            continue;
      }
      if (c->effective_cmd[0])
      {
         /* Embed heading_path + content */
         char embed_text[4096];
         kb_async_make_embed_text(c->chunks[ci].heading_path, c->chunks[ci].content, embed_text,
                                  sizeof(embed_text));

         float vec[EMBED_MAX_DIM];
         int dim = memory_embed_text(embed_text, c->effective_cmd, vec, EMBED_MAX_DIM);
         if (dim > 0)
         {
            accept_generated_embedding(doc_id, vec, dim);
            c->stats->embeddings_added++;

            /* Add to pgvector batch buffer. Dim mismatches fall back to
             * single-point upsert which fails cleanly against the
             * 384-d collection instead of corrupting the batch. */
            if (dim != c->kb_expected_dim)
            {
               kb_flush_batch(c->project, c->kb_batch_ids, c->kb_batch_vecs, c->kb_expected_dim,
                              c->kb_batch_payloads, c->kb_batch_count);
               kb_sync_vector_embedding_fenced(c->project, doc_id, vec, dim);
            }
            else
            {
               char *payload_json = db2_kb_build_document_payload(doc_id);
               if (!payload_json)
               {
                  db2_vector_index_op_record(doc_id, pgvec_kb_vector_collection_name(), 0, 0,
                                             "payload build failed");
                  continue;
               }
               c->kb_batch_ids[*c->kb_batch_count] = doc_id;
               memcpy(c->kb_batch_vecs + (size_t)*c->kb_batch_count * (size_t)c->kb_expected_dim,
                      vec, (size_t)c->kb_expected_dim * sizeof(float));
               c->kb_batch_payloads[*c->kb_batch_count] = payload_json;
               (*c->kb_batch_count)++;
               if (*c->kb_batch_count >= c->kb_batch_size)
                  kb_flush_batch(c->project, c->kb_batch_ids, c->kb_batch_vecs, c->kb_expected_dim,
                                 c->kb_batch_payloads, c->kb_batch_count);
            }
         }
      }
   }
   free(doc_ids);

   sketch_bloom_add_hash(c->bloom, h64);
   sketch_count_min_add_hash(c->count_min, h64, 1);
   sketch_hll_add_hash(c->hll, sketch_fnv1a(c->files[fi].rel_path, strlen(c->files[fi].rel_path)));
   if (kb_minhash_upsert_fenced(c->project, c->files[fi].rel_path, hash, &minhash_sig) != 0)
      LOG_WARN("kb_build", "project=%s path=%s: minhash signature save failed",
               c->project ? c->project : "?", c->files[fi].rel_path);
   kb_file_index_store_from_path(c->project, c->files[fi].rel_path, hash, c->files[fi].path);
   c->stats->files_indexed++;
}

static int kb_build_or_update(const char *root_path, const char *project, const char *embedding_cmd,
                              int force_rebuild, kb_stats_t *stats_out)
{
   if (!root_path || !db2_is_initialized())
      return -1;
   /* Generation fence (webchat-project-lifecycle slice 2): refuse to start an
    * ingest for a project whose purge is in flight. Per-batch commit-point
    * checks in kb_flush_batch cover a fence raised mid-ingest. */
   if (project && project[0] && db2_kb_purge_fence_active(project))
   {
      LOG_WARN("kb_build", "purge fence active for project '%s': refusing ingest", project);
      return -1;
   }
   const char *effective_cmd = kb_effective_embedding_cmd(embedding_cmd);

   kb_stats_t stats;
   memset(&stats, 0, sizeof(stats));
   int async_enabled = kb_async_enabled();

   if (force_rebuild && project && project[0])
   {
      LOG_INFO("kb_build", "force rebuild: clearing project '%s' from DB2", project);
      db2_kb_service_clear_project(project);
      db2_sketch_minhash_signature_delete_project(project);
      pgvec_kb_vector_delete_project(project);
   }

   /* Resolve include/exclude patterns */
   char includes[16][KB_GLOB_MAX];
   char excludes[16][KB_GLOB_MAX];
   int n_inc = split_glob_list(KB_DEFAULT_INCLUDE, includes, 16);
   int n_exc = split_glob_list(KB_DEFAULT_EXCLUDE, excludes, 16);

   /* Collect files */
   file_entry_t *files = malloc(MAX_FILES * sizeof(file_entry_t));
   if (!files)
      return -1;
   int n_files = collect_files(root_path, "", includes, n_inc, excludes, n_exc, files, MAX_FILES);
   stats.files_scanned = n_files;

   /* Process each file */
   text_chunk_t *chunks = malloc(MAX_CHUNKS_PER_FILE * sizeof(text_chunk_t));
   if (!chunks)
   {
      free(files);
      return -1;
   }

   /* Shadow-mode sketches: Bloom tracks which file content hashes have been
    * indexed before. Count-Min estimates duplicate frequency. The project
    * HLL tracks distinct file paths as a calibration/sample-size signal;
    * candidate-level HLL features are computed against each candidate hash.
    * Force rebuild starts sketches from empty state because
    * db2_kb_service_clear_project() has removed their source-of-truth rows. */
   sketch_bloom_t bloom;
   sketch_count_min_t count_min;
   sketch_hll_t hll;
   kb_load_build_sketches(project, force_rebuild, &bloom, &count_min, &hll);

   const int kb_progress_every = 50;

   const int kb_batch_size_default = 64;
   const char *kb_batch_env = getenv("AIMEE_VECTOR_KB_BATCH_SIZE");
   int kb_batch_size = kb_batch_env ? atoi(kb_batch_env) : kb_batch_size_default;
   if (kb_batch_size < 1)
      kb_batch_size = 1;
   if (kb_batch_size > 512)
      kb_batch_size = 512;
   const int kb_expected_dim = 384;
   int64_t *kb_batch_ids = malloc((size_t)kb_batch_size * sizeof(int64_t));
   float *kb_batch_vecs = malloc((size_t)kb_batch_size * (size_t)kb_expected_dim * sizeof(float));
   char **kb_batch_payloads = calloc((size_t)kb_batch_size, sizeof(char *));
   int kb_batch_count = 0;
   if (!kb_batch_ids || !kb_batch_vecs || !kb_batch_payloads)
   {
      free(kb_batch_ids);
      free(kb_batch_vecs);
      free(kb_batch_payloads);
      free(chunks);
      free(files);
      return -1;
   }

   kb_build_file_ctx_t fctx = {
       .project = project,
       .effective_cmd = effective_cmd,
       .force_rebuild = force_rebuild,
       .async_enabled = async_enabled,
       .n_files = n_files,
       .kb_progress_every = kb_progress_every,
       .kb_expected_dim = kb_expected_dim,
       .kb_batch_size = kb_batch_size,
       .files = files,
       .chunks = chunks,
       .bloom = &bloom,
       .count_min = &count_min,
       .hll = &hll,
       .stats = &stats,
       .kb_batch_ids = kb_batch_ids,
       .kb_batch_vecs = kb_batch_vecs,
       .kb_batch_payloads = kb_batch_payloads,
       .kb_batch_count = &kb_batch_count,
   };
   for (int fi = 0; fi < n_files; fi++)
      kb_process_one_file(&fctx, fi);

   kb_flush_batch(project, kb_batch_ids, kb_batch_vecs, kb_expected_dim, kb_batch_payloads,
                  &kb_batch_count);
   free(kb_batch_ids);
   free(kb_batch_vecs);
   free(kb_batch_payloads);

   if (db2_sketch_bloom_save(&bloom, "kb_project", project ? project : "", "file_hash") != 0 ||
       db2_sketch_count_min_save(&count_min, "kb_project", project ? project : "", "file_hash") !=
           0 ||
       db2_sketch_hll_save(&hll, "kb_project", project ? project : "", "file_path") != 0)
      LOG_WARN("kb_build", "project=%s: sketch save failed; ingest data remains committed",
               project ? project : "?");

   LOG_INFO(
       "kb_build",
       "project=%s complete: %d files scanned, %d indexed, %d skipped, %d chunks, %d embeddings",
       project ? project : "?", stats.files_scanned, stats.files_indexed, stats.files_skipped,
       stats.chunks_added, stats.embeddings_added);

   free(chunks);
   free(files);

   if (stats_out)
      *stats_out = stats;
   return 0;
}

int kb_build(const char *root_path, const char *project, const char *embedding_cmd,
             int force_rebuild, kb_stats_t *stats_out)
{
   if (!db2_is_initialized())
      return -1;
   return kb_build_or_update(root_path, project, embedding_cmd, force_rebuild, stats_out);
}

int kb_update(const char *root_path, const char *project, const char *embedding_cmd,
              kb_stats_t *stats_out)
{
   if (!db2_is_initialized())
      return -1;
   return kb_build_or_update(root_path, project, embedding_cmd, 0, stats_out);
}

/* ------------------------------------------------------------------ */
/* Search: pgvector lexical-style + vector retrieval with RRF          */
/* ------------------------------------------------------------------ */

#define RRF_K               60
#define MAX_LEXICAL_RESULTS 50
#define MAX_VEC_RESULTS     50

typedef struct
{
   int64_t doc_id;
   char file_path[MAX_PATH_LEN];
   char file_hash[32];
   char heading_path[MAX_HEADING_LEN];
   int line_start;
   int line_end;
   char content[CHUNK_BUF_SIZE];
   double score;       /* combined score (RRF or ranker) */
   double lex_score;   /* raw lexical-leg cosine score */
   double dense_score; /* raw dense-leg cosine score */
} kb_result_t;

/* Fetch a kb_documents row by (id, project) into out, returning 1 on
 * hit / 0 on miss. Routes through db2_kb_document_fetch so the SQL
 * stays inside src/db2/. */
static int kb_fetch_doc_row(int64_t id, const char *project, kb_result_t *out)
{
   db2_kb_document_row_t row;
   if (!db2_kb_document_fetch(id, project, &row))
      return 0;
   /* structured-PDF Phase 2 safety chokepoint: PDF chunks are excluded from the general
    * search legs entirely. Once embedded they appear in the vector index, but PDF content
    * is evidence reachable ONLY through the access-controlled search_chunks tool (which
    * gates on quarantine_state + scope), never through plain /v1/search. This is the single
    * point both legs materialise rows, so excluding here covers lexical + dense. */
   if (strcmp(row.doc_kind, "pdf") == 0)
      return 0;
   out->doc_id = row.id;
   snprintf(out->file_path, sizeof(out->file_path), "%s", row.file_path);
   snprintf(out->file_hash, sizeof(out->file_hash), "%s", row.file_hash);
   snprintf(out->heading_path, sizeof(out->heading_path), "%s", row.heading_path);
   out->line_start = row.line_start;
   out->line_end = row.line_end;
   snprintf(out->content, sizeof(out->content), "%s", row.content);
   out->score = 0.0;
   return 1;
}

/* Lexical-style search routed through pgvector. Embeds the query, runs the
 * same project-scoped dense search as vec_search, and materialises rows
 * via DB2. Today this returns the same id set as vec_search, so the
 * RRF merge in kb_search_gather degenerates to a re-rank of one input
 * list — still a valid combined score.  Future work: add a sparse / BM25
 * vector index and route this leg through it for true hybrid retrieval. */
/* Lexical (FTS/term-match) retrieval leg for kb_search_fused. This is a genuine
 * term search over kb_documents.kb_fts_tsv, NOT another dense search: previously
 * it embedded the query and called the same pgvector search as vec_search, so the
 * two legs were byte-for-byte identical and alpha_merge collapsed to the identity
 * (rrf/static_alpha/dynamic_alpha all produced the same ranking). With a real
 * lexical leg the two signals diverge and the fusion modes become meaningful. */
static int lexical_search_fts(const char *project, const char *query, kb_result_t *out, int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return 0;

   int64_t ids[MAX_LEXICAL_RESULTS * 2];
   double scores[MAX_LEXICAL_RESULTS * 2];
   int cap = (int)(sizeof(ids) / sizeof(ids[0]));
   if (max * 2 < cap)
      cap = max * 2;
   int n_hits = db2_kb_documents_fts_search(project, query, ids, scores, cap);
   if (n_hits <= 0)
      return 0;

   int n = 0;
   for (int i = 0; i < n_hits && n < max; i++)
   {
      if (kb_fetch_doc_row(ids[i], project, &out[n]))
      {
         out[n].lex_score = scores[i];
         out[n].score = scores[i];
         n++;
      }
   }
   return n;
}

/* Vector search uses pgvector as the only dense retrieval path. */
static int vec_search(const char *project, const float *qvec, int qdim, kb_result_t *out, int max)
{
   if (qdim <= 0)
      return 0;

   int64_t ids[MAX_VEC_RESULTS * 2];
   double scores[MAX_VEC_RESULTS * 2];
   int n_hits = pgvec_kb_vector_search_project(project, qvec, qdim, max * 2, ids, scores,
                                               MAX_VEC_RESULTS * 2);
   if (n_hits < 0)
      return -1;
   if (n_hits == 0)
      return 0;

   int n = 0;
   for (int i = 0; i < n_hits && n < max; i++)
   {
      if (kb_fetch_doc_row(ids[i], project, &out[n]))
      {
         out[n].dense_score = scores[i];
         out[n].score = scores[i];
         n++;
      }
   }
   return n;
}

/* Forward declaration: rrf_merge is defined after alpha_merge. */
static int rrf_merge(kb_result_t *lex_res, int n_lex, kb_result_t *vec_res, int n_vec,
                     kb_result_t *out, int max);

/* final(d) = (1-alpha)*dense_norm(d) + alpha*lex_norm(d); falls back to rrf when leg empty. */
static int alpha_merge(kb_result_t *lex_res, int n_lex, kb_result_t *vec_res, int n_vec,
                       kb_result_t *out, int max, double alpha)
{
   if (n_lex <= 0 || n_vec <= 0)
      return rrf_merge(lex_res, n_lex, vec_res, n_vec, out, max);

   /* Build union of doc_ids. */
   typedef struct
   {
      int64_t doc_id;
      double lex_score;
      double dense_score;
      int lex_idx;
      int dense_idx;
   } am_entry_t;

   int total = n_lex + n_vec;
   am_entry_t *entries = malloc((size_t)total * sizeof(am_entry_t));
   if (!entries)
      return rrf_merge(lex_res, n_lex, vec_res, n_vec, out, max);

   int n_entries = 0;
   for (int i = 0; i < n_lex; i++)
   {
      int found = 0;
      for (int j = 0; j < n_entries; j++)
         if (entries[j].doc_id == lex_res[i].doc_id)
         {
            entries[j].lex_score = lex_res[i].lex_score;
            entries[j].lex_idx = i;
            found = 1;
            break;
         }
      if (!found)
      {
         entries[n_entries].doc_id = lex_res[i].doc_id;
         entries[n_entries].lex_score = lex_res[i].lex_score;
         entries[n_entries].dense_score = 0.0;
         entries[n_entries].lex_idx = i;
         entries[n_entries].dense_idx = -1;
         n_entries++;
      }
   }
   for (int i = 0; i < n_vec; i++)
   {
      int found = 0;
      for (int j = 0; j < n_entries; j++)
         if (entries[j].doc_id == vec_res[i].doc_id)
         {
            entries[j].dense_score = vec_res[i].dense_score;
            entries[j].dense_idx = i;
            found = 1;
            break;
         }
      if (!found)
      {
         entries[n_entries].doc_id = vec_res[i].doc_id;
         entries[n_entries].lex_score = 0.0;
         entries[n_entries].dense_score = vec_res[i].dense_score;
         entries[n_entries].lex_idx = -1;
         entries[n_entries].dense_idx = i;
         n_entries++;
      }
   }

   /* Normalize each score channel. */
   double *lex_s = malloc((size_t)n_entries * sizeof(double));
   double *dense_s = malloc((size_t)n_entries * sizeof(double));
   if (!lex_s || !dense_s)
   {
      free(lex_s);
      free(dense_s);
      free(entries);
      return rrf_merge(lex_res, n_lex, vec_res, n_vec, out, max);
   }
   for (int i = 0; i < n_entries; i++)
   {
      lex_s[i] = entries[i].lex_score;
      dense_s[i] = entries[i].dense_score;
   }
   kb_fusion_normalize_scores(lex_s, n_entries);
   kb_fusion_normalize_scores(dense_s, n_entries);

   /* Compute fused scores and insertion-sort into output. */
   double *fused = malloc((size_t)n_entries * sizeof(double));
   int *order = malloc((size_t)n_entries * sizeof(int));
   if (!fused || !order)
   {
      free(lex_s);
      free(dense_s);
      free(fused);
      free(order);
      free(entries);
      return rrf_merge(lex_res, n_lex, vec_res, n_vec, out, max);
   }
   for (int i = 0; i < n_entries; i++)
   {
      fused[i] = (1.0 - alpha) * dense_s[i] + alpha * lex_s[i];
      order[i] = i;
   }
   /* Sort order[] by fused descending (insertion sort — small arrays) */
   for (int i = 1; i < n_entries; i++)
   {
      int key = order[i];
      int j = i - 1;
      while (j >= 0 && fused[order[j]] < fused[key])
      {
         order[j + 1] = order[j];
         j--;
      }
      order[j + 1] = key;
   }

   int n = (n_entries < max) ? n_entries : max;
   for (int i = 0; i < n; i++)
   {
      int idx = order[i];
      kb_result_t *src;
      if (entries[idx].lex_idx >= 0)
         src = &lex_res[entries[idx].lex_idx];
      else
         src = &vec_res[entries[idx].dense_idx];
      out[i] = *src;
      out[i].score = fused[idx];
      out[i].lex_score = entries[idx].lex_score;
      out[i].dense_score = entries[idx].dense_score;
   }

   free(lex_s);
   free(dense_s);
   free(fused);
   free(order);
   free(entries);
   return n;
}

/* Reciprocal Rank Fusion: combine lexical and vector result lists.
 * score(d) = sum_over_lists( 1 / (RRF_K + rank_in_list) )
 * Returns merged list sorted by combined score descending. */
static int rrf_merge(kb_result_t *lex_res, int n_lex, kb_result_t *vec_res, int n_vec,
                     kb_result_t *out, int max)
{
   /* Use a simple map: collect unique doc_ids, accumulate RRF scores and preserve
    * individual leg scores for downstream feature computation. */
   typedef struct
   {
      int64_t doc_id;
      double rrf;
      double lex_score;
      double dense_score;
      int source_idx; /* index in lex_res or vec_res for content */
      int source;     /* 0=lexical, 1=vector */
   } rrf_entry_t;

   int total = n_lex + n_vec;
   rrf_entry_t *entries = malloc((size_t)total * sizeof(rrf_entry_t));
   if (!entries)
      return 0;
   int n_entries = 0;

   /* Helper: find or insert doc_id */
   for (int i = 0; i < n_lex; i++)
   {
      int found = 0;
      for (int j = 0; j < n_entries; j++)
      {
         if (entries[j].doc_id == lex_res[i].doc_id)
         {
            entries[j].rrf += 1.0 / (RRF_K + i + 1);
            entries[j].lex_score = lex_res[i].lex_score;
            found = 1;
            break;
         }
      }
      if (!found)
      {
         entries[n_entries].doc_id = lex_res[i].doc_id;
         entries[n_entries].rrf = 1.0 / (RRF_K + i + 1);
         entries[n_entries].lex_score = lex_res[i].lex_score;
         entries[n_entries].dense_score = 0.0;
         entries[n_entries].source_idx = i;
         entries[n_entries].source = 0;
         n_entries++;
      }
   }
   for (int i = 0; i < n_vec; i++)
   {
      int found = 0;
      for (int j = 0; j < n_entries; j++)
      {
         if (entries[j].doc_id == vec_res[i].doc_id)
         {
            entries[j].rrf += 1.0 / (RRF_K + i + 1);
            entries[j].dense_score = vec_res[i].dense_score;
            found = 1;
            break;
         }
      }
      if (!found)
      {
         entries[n_entries].doc_id = vec_res[i].doc_id;
         entries[n_entries].rrf = 1.0 / (RRF_K + i + 1);
         entries[n_entries].lex_score = 0.0;
         entries[n_entries].dense_score = vec_res[i].dense_score;
         entries[n_entries].source_idx = i;
         entries[n_entries].source = 1;
         n_entries++;
      }
   }

   /* Sort by rrf descending */
   for (int i = 1; i < n_entries; i++)
   {
      rrf_entry_t tmp = entries[i];
      int j = i - 1;
      while (j >= 0 && entries[j].rrf < tmp.rrf)
      {
         entries[j + 1] = entries[j];
         j--;
      }
      entries[j + 1] = tmp;
   }

   int n = (n_entries < max) ? n_entries : max;
   for (int i = 0; i < n; i++)
   {
      kb_result_t *src = entries[i].source == 0 ? &lex_res[entries[i].source_idx]
                                                : &vec_res[entries[i].source_idx];
      out[i] = *src;
      out[i].score = entries[i].rrf;
      out[i].lex_score = entries[i].lex_score;
      out[i].dense_score = entries[i].dense_score;
   }
   free(entries);
   return n;
}

/* Format a snippet: truncate content to ~200 chars */
static void format_snippet(const char *content, char *out, size_t out_len)
{
   size_t clen = strlen(content);
   if (clen <= 200)
   {
      snprintf(out, out_len, "%s", content);
      return;
   }
   /* Find a line break near the 200-char mark */
   size_t cut = 200;
   for (size_t i = 200; i < clen && i < 220; i++)
   {
      if (content[i] == '\n')
      {
         cut = i;
         break;
      }
   }
   memcpy(out, content, cut < out_len - 4 ? cut : out_len - 4);
   size_t pos = cut < out_len - 4 ? cut : out_len - 5;
   out[pos] = '\0';
   if (cut < clen)
      strncat(out, "...", out_len - pos - 1);
}

/* Append `s` to `out` with JSON-string escaping applied (quotes not added;
 * caller wraps). Used by kb_search_json(). */
static void kb_json_append_escaped(dstr_t *out, const char *s)
{
   if (!s)
      return;
   for (const char *p = s; *p; p++)
   {
      unsigned char c = (unsigned char)*p;
      switch (c)
      {
      case '"':
         dstr_append_str(out, "\\\"");
         break;
      case '\\':
         dstr_append_str(out, "\\\\");
         break;
      case '\n':
         dstr_append_str(out, "\\n");
         break;
      case '\r':
         dstr_append_str(out, "\\r");
         break;
      case '\t':
         dstr_append_str(out, "\\t");
         break;
      default:
         if (c < 0x20)
         {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            dstr_append_str(out, buf);
         }
         else
         {
            char single[2] = {(char)c, 0};
            dstr_append_str(out, single);
         }
         break;
      }
   }
}

/* Resolve the per-call result cap.  Honors cfg->kb_search_max_results (default
 * 50 when unset) as an upper bound; requests <= 0 fall back to
 * KB_DEFAULT_MAX_RESULTS. */
static int kb_search_resolve_cap(int requested)
{
   config_t cfg;
   int cap = 50;
   if (config_load(&cfg) == 0 && cfg.kb_search_max_results > 0)
      cap = cfg.kb_search_max_results;
   if (requested <= 0)
      requested = KB_DEFAULT_MAX_RESULTS;
   if (requested > cap)
      requested = cap;
   return requested;
}

/* Hybrid retrieval: lexical + dense, fused by configured mode.
 * fusion_mode_override: overrides config when non-NULL/non-empty.
 * fusion_mode_out (buf>=32) and fusion_alpha_out receive the mode/alpha used. */
static char *kb_search_gather(const char *project, const char *query, const char *embedding_cmd,
                              int max_results, kb_result_t *merged, int *n_out,
                              const char *fusion_mode_override, char *fusion_mode_out,
                              double *fusion_alpha_out)
{
   *n_out = 0;
   if (fusion_mode_out)
      snprintf(fusion_mode_out, 32, "rrf");
   if (fusion_alpha_out)
      *fusion_alpha_out = 0.0;

   if (!query || !query[0])
      return safe_strdup("error: empty query");
   if (!db2_is_initialized())
      return safe_strdup("error: db2 not initialized");
   const char *effective_cmd = kb_effective_embedding_cmd(embedding_cmd);

   /* An explicit project scopes the search; omitting it means whole-corpus (all
    * projects), NOT the server's cwd-basename. kb_resolve_project would default
    * an empty project to getcwd()'s basename, which inside the kb SERVER is
    * always its WORKDIR (/var/lib/aimee -> "aimee") and matches no real corpus —
    * so an API /v1/kb/search that omits `project` silently returned nothing. The
    * CLI resolves its own cwd project before calling, so it always sends an
    * explicit name and is unaffected. Empty proj flows to the legs as "all". */
   char proj[256];
   if (project && project[0])
      kb_resolve_project(project, NULL, proj, sizeof(proj));
   else
      proj[0] = '\0';

   /* Determine fusion mode: explicit override > bandit sample > config > default.
    * When no override is given and live bandit decisions are enabled, sample the
    * fusion strategy from the kb_fusion_mode decision point; the choice is
    * rewarded from this search's recall sufficiency at the success exit below. */
   config_t fusion_cfg;
   int fusion_cfg_ok = (config_load(&fusion_cfg) == 0);
   const char *fusion_mode;
   char fm_decision_id[KB_BANDIT_MAX_DECISION] = {0};
   char fm_arm_id[KB_BANDIT_MAX_ARM_ID] = {0};
   char fm_promo[KB_BANDIT_MAX_ARM_ID] = "";
   const kb_bandit_decision_point_t *fm_dp = NULL;
   if (fusion_mode_override && fusion_mode_override[0])
      fusion_mode = fusion_mode_override;
   else
   {
      if (fusion_cfg_ok && fusion_cfg.bandit_live_decision_enabled)
      {
         fm_dp = kb_bandit_registry_get("kb_fusion_mode");
         if (fm_dp && fm_dp->n_arms > 0)
         {
            char fm_arms[KB_BANDIT_MAX_ARMS][KB_BANDIT_MAX_ARM_ID];
            for (int i = 0; i < fm_dp->n_arms; i++)
               snprintf(fm_arms[i], KB_BANDIT_MAX_ARM_ID, "%s", fm_dp->arms[i]);
            int a = kb_bandit_sample(&fusion_cfg, fm_dp->id, NULL, fm_arms, fm_dp->n_arms,
                                     fm_decision_id);
            if (a >= 0 && a < fm_dp->n_arms)
               snprintf(fm_arm_id, sizeof(fm_arm_id), "%s", fm_arms[a]);
            else
               fm_dp = NULL; /* sample disabled/failed: fall back, no reward */
         }
      }
      if (fm_arm_id[0])
         fusion_mode = fm_arm_id; /* function-scope buffer; valid throughout */
      else if (db2_bandit_promotion_get("kb_fusion_mode", fm_promo, sizeof(fm_promo)) == 0 &&
               fm_promo[0])
         fusion_mode = fm_promo; /* promoted default (operator locked it in) */
      else if (fusion_cfg_ok && fusion_cfg.kb_fusion_mode[0])
         fusion_mode = fusion_cfg.kb_fusion_mode;
      else
         fusion_mode = "rrf";
   }

   kb_result_t *lex_res = malloc(MAX_LEXICAL_RESULTS * sizeof(kb_result_t));
   kb_result_t *vec_res = malloc(MAX_VEC_RESULTS * sizeof(kb_result_t));
   if (!lex_res || !vec_res)
   {
      free(lex_res);
      free(vec_res);
      return safe_strdup("error: out of memory");
   }

   int n_lex = lexical_search_fts(proj, query, lex_res, MAX_LEXICAL_RESULTS);

   /* Dense search is mandatory; the lexical vector leg complements it. */
   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text(query, effective_cmd, qvec, EMBED_MAX_DIM);
   if (qdim <= 0)
   {
      free(lex_res);
      free(vec_res);
      return safe_strdup(
          "error: documentation query embedding failed; server-side embedding configuration is "
          "unavailable");
   }
   int n_vec = vec_search(proj, qvec, qdim, vec_res, MAX_VEC_RESULTS);
   if (n_vec < 0)
   {
      free(lex_res);
      free(vec_res);
      return safe_strdup(
          "error: documentation index unavailable; server-side maintenance is required");
   }

   int n_results = 0;
   double chosen_alpha = 0.0;

   if (n_lex > 0 && n_vec > 0)
   {
      if (strcmp(fusion_mode, "static_alpha") == 0)
      {
         chosen_alpha = fusion_cfg_ok ? fusion_cfg.kb_fusion_static_alpha : 0.5;
         n_results = alpha_merge(lex_res, n_lex, vec_res, n_vec, merged, max_results, chosen_alpha);
      }
      else if (strcmp(fusion_mode, "dynamic_alpha") == 0)
      {
         chosen_alpha = kb_fusion_predict_alpha(query);
         n_results = alpha_merge(lex_res, n_lex, vec_res, n_vec, merged, max_results, chosen_alpha);
      }
      else
      {
         n_results = rrf_merge(lex_res, n_lex, vec_res, n_vec, merged, max_results);
         fusion_mode = "rrf";
      }
   }
   else if (n_lex > 0)
   {
      int copy = (n_lex < max_results) ? n_lex : max_results;
      memcpy(merged, lex_res, (size_t)copy * sizeof(kb_result_t));
      n_results = copy;
      fusion_mode = "rrf";
   }
   else if (n_vec > 0)
   {
      int copy = (n_vec < max_results) ? n_vec : max_results;
      memcpy(merged, vec_res, (size_t)copy * sizeof(kb_result_t));
      n_results = copy;
      fusion_mode = "rrf";
   }

   if (fusion_mode_out)
      snprintf(fusion_mode_out, 32, "%s", fusion_mode);
   if (fusion_alpha_out)
      *fusion_alpha_out = chosen_alpha;

   free(lex_res);
   free(vec_res);

#if !defined(AIMEE_DB2_DISABLED)
   if (n_results > 0)
   {
      config_t kb_cfg;
      int cfg_ok = (config_load(&kb_cfg) == 0);

      sketch_count_min_t count_min;
      int have_count_min =
          db2_sketch_count_min_load(&count_min, "kb_project", proj, "file_hash") == 0;
      if (!have_count_min)
         sketch_count_min_init(&count_min);

      /* Compute feature rows for each candidate. */
      kb_sketch_features_t sketch_features[MAX_LEXICAL_RESULTS + MAX_VEC_RESULTS];
      for (int i = 0; i < n_results; i++)
      {
         memset(&sketch_features[i], 0, sizeof(sketch_features[i]));
         if (merged[i].file_hash[0] && have_count_min)
         {
            uint64_t h64 = (uint64_t)strtoull(merged[i].file_hash, NULL, 16);
            sketch_features[i].frequency_kind_scope =
                (double)sketch_count_min_estimate_hash(&count_min, h64);
         }
         if (merged[i].file_hash[0])
         {
            sketch_hll_t source_hll;
            if (db2_kb_documents_hll_sources_for_hash(proj, merged[i].file_hash, &source_hll) >= 0)
               sketch_features[i].distinct_sources_hll = sketch_hll_estimate(&source_hll);
         }
         kb_features_upsert_with_sketch(merged[i].doc_id, merged[i].lex_score,
                                        merged[i].dense_score, 0.0, &sketch_features[i]);
      }

      /* Apply linear ranker when enabled and a model is loaded. */
      if (cfg_ok && kb_cfg.kb_ranker_enabled)
      {
         int64_t ranked_ids[MAX_LEXICAL_RESULTS + MAX_VEC_RESULTS];
         double ranked_scores[MAX_LEXICAL_RESULTS + MAX_VEC_RESULTS];
         double lex_s[MAX_LEXICAL_RESULTS + MAX_VEC_RESULTS];
         double dense_s[MAX_LEXICAL_RESULTS + MAX_VEC_RESULTS];
         double age_s[MAX_LEXICAL_RESULTS + MAX_VEC_RESULTS];
         for (int i = 0; i < n_results; i++)
         {
            lex_s[i] = merged[i].lex_score;
            dense_s[i] = merged[i].dense_score;
            age_s[i] = 0.0;
         }
         int64_t ids[MAX_LEXICAL_RESULTS + MAX_VEC_RESULTS];
         for (int i = 0; i < n_results; i++)
            ids[i] = merged[i].doc_id;

         int n_ranked =
             kb_ranker_rerank_with_sketch(&kb_cfg, ids, lex_s, dense_s, age_s, sketch_features,
                                          n_results, ranked_ids, ranked_scores);
         if (n_ranked == n_results)
         {
            /* Re-order merged[] to match ranked order. */
            kb_result_t tmp_buf[MAX_LEXICAL_RESULTS + MAX_VEC_RESULTS];
            for (int i = 0; i < n_results; i++)
            {
               int found = 0;
               for (int j = 0; j < n_results; j++)
               {
                  if (merged[j].doc_id == ranked_ids[i])
                  {
                     tmp_buf[i] = merged[j];
                     tmp_buf[i].score = ranked_scores[i];
                     found = 1;
                     break;
                  }
               }
               if (!found)
                  tmp_buf[i] = merged[i];
            }
            memcpy(merged, tmp_buf, (size_t)n_results * sizeof(kb_result_t));
         }
      }

      /* Drift detection in shadow mode. */
      if (cfg_ok && kb_cfg.drift_detect_shadow_enabled && n_results > 0)
      {
         double sum = 0.0;
         for (int i = 0; i < n_results; i++)
            sum += merged[i].dense_score;
         kb_detect_observe(&kb_cfg, sum / n_results, n_results);
      }
   }
#endif

   *n_out = n_results;

   /* Close the kb_fusion_mode bandit loop with this search's recall sufficiency
    * (same proxy as kb_memory_retrieval_limit), so the arm posteriors learn from
    * live traffic. Only runs when an arm was sampled above. */
   if (fm_dp && fm_decision_id[0] && fm_arm_id[0])
   {
      double reward = kb_bandit_recall_sufficiency_reward(n_results, max_results);
      kb_bandit_reward(NULL, fm_dp->id, fm_decision_id, fm_arm_id, reward);
   }
   return NULL;
}

char *kb_search(const char *project, const char *query, const char *embedding_cmd, int max_results)
{
   if (!db2_is_initialized())
      return safe_strdup("error: db2 not initialized");
   max_results = kb_search_resolve_cap(max_results);

   kb_result_t *merged = malloc((MAX_LEXICAL_RESULTS + MAX_VEC_RESULTS) * sizeof(kb_result_t));
   if (!merged)
      return safe_strdup("error: out of memory");

   int n_results = 0;
   char *err = kb_search_gather(project, query, embedding_cmd, max_results, merged, &n_results,
                                NULL, NULL, NULL);
   if (err)
   {
      free(merged);
      return err;
   }
   if (n_results == 0)
   {
      free(merged);
      return safe_strdup("No results found.");
   }

   dstr_t out;
   dstr_init(&out);
   for (int i = 0; i < n_results; i++)
   {
      char snippet[512];
      format_snippet(merged[i].content, snippet, sizeof(snippet));

      char header[512];
      if (merged[i].heading_path[0])
         snprintf(header, sizeof(header), "[%s:%d-%d] %s", merged[i].file_path,
                  merged[i].line_start, merged[i].line_end, merged[i].heading_path);
      else
         snprintf(header, sizeof(header), "[%s:%d-%d]", merged[i].file_path, merged[i].line_start,
                  merged[i].line_end);

      dstr_append_str(&out, header);
      dstr_append_str(&out, "\n");
      dstr_append_str(&out, snippet);
      if (i < n_results - 1)
         dstr_append_str(&out, "\n\n");
   }

   free(merged);
   char *result = safe_strdup(out.data ? out.data : "");
   dstr_free(&out);
   return result;
}

char *kb_search_json(const char *project, const char *query, const char *embedding_cmd,
                     int max_results)
{
   if (!db2_is_initialized())
      return safe_strdup("{\"error\":\"db2 not initialized\"}");
   max_results = kb_search_resolve_cap(max_results);

   kb_result_t *merged = malloc((MAX_LEXICAL_RESULTS + MAX_VEC_RESULTS) * sizeof(kb_result_t));
   if (!merged)
      return safe_strdup("{\"error\":\"out of memory\"}");

   int n_results = 0;
   char fusion_mode[32];
   double fusion_alpha = 0.0;
   char *err = kb_search_gather(project, query, embedding_cmd, max_results, merged, &n_results,
                                NULL, fusion_mode, &fusion_alpha);
   if (err)
   {
      free(merged);
      dstr_t out;
      dstr_init(&out);
      dstr_append_str(&out, "{\"error\":\"");
      kb_json_append_escaped(&out, err);
      dstr_append_str(&out, "\"}");
      free(err);
      char *result = safe_strdup(out.data ? out.data : "{\"error\":\"unknown\"}");
      dstr_free(&out);
      return result;
   }

   dstr_t out;
   dstr_init(&out);
   char num[32];
   dstr_append_str(&out, "{\"fusion_mode\":\"");
   dstr_append_str(&out, fusion_mode);
   dstr_append_str(&out, "\"");
   if (strcmp(fusion_mode, "rrf") != 0)
   {
      dstr_append_str(&out, ",\"fusion_alpha\":");
      snprintf(num, sizeof(num), "%.4f", fusion_alpha);
      dstr_append_str(&out, num);
   }
   dstr_append_str(&out, ",\"results\":[");
   for (int i = 0; i < n_results; i++)
   {
      if (i > 0)
         dstr_append_str(&out, ",");
      dstr_append_str(&out, "{\"file_path\":\"");
      kb_json_append_escaped(&out, merged[i].file_path);
      dstr_append_str(&out, "\",\"line_start\":");
      snprintf(num, sizeof(num), "%d", merged[i].line_start);
      dstr_append_str(&out, num);
      dstr_append_str(&out, ",\"line_end\":");
      snprintf(num, sizeof(num), "%d", merged[i].line_end);
      dstr_append_str(&out, num);
      dstr_append_str(&out, ",\"heading_path\":\"");
      kb_json_append_escaped(&out, merged[i].heading_path);
      dstr_append_str(&out, "\",\"content\":\"");
      kb_json_append_escaped(&out, merged[i].content);
      dstr_append_str(&out, "\",\"score\":");
      snprintf(num, sizeof(num), "%.6f", merged[i].score);
      dstr_append_str(&out, num);
      /* doc_id keys the row to its feature_rows — needed by the learning-to-rank
       * outcome capture (the file_path-keyed http `hits` shape lacks it). */
      dstr_append_str(&out, ",\"doc_id\":");
      snprintf(num, sizeof(num), "%lld", (long long)merged[i].doc_id);
      dstr_append_str(&out, num);
      dstr_append_str(&out, "}");
   }
   dstr_append_str(&out, "]}");

   free(merged);
   char *result = safe_strdup(out.data ? out.data : "{\"results\":[]}");
   dstr_free(&out);
   return result;
}

/* Variant of kb_search_json with an explicit fusion_mode override.
 * fusion_mode_override: "rrf" | "static_alpha" | "dynamic_alpha"; NULL = use config. */
char *kb_search_json_ex(const char *project, const char *query, const char *embedding_cmd,
                        int max_results, const char *fusion_mode_override)
{
   if (!db2_is_initialized())
      return safe_strdup("{\"error\":\"db2 not initialized\"}");
   max_results = kb_search_resolve_cap(max_results);

   kb_result_t *merged = malloc((MAX_LEXICAL_RESULTS + MAX_VEC_RESULTS) * sizeof(kb_result_t));
   if (!merged)
      return safe_strdup("{\"error\":\"out of memory\"}");

   int n_results = 0;
   char fusion_mode[32];
   double fusion_alpha = 0.0;
   char *err = kb_search_gather(project, query, embedding_cmd, max_results, merged, &n_results,
                                fusion_mode_override, fusion_mode, &fusion_alpha);
   if (err)
   {
      free(merged);
      dstr_t out;
      dstr_init(&out);
      dstr_append_str(&out, "{\"error\":\"");
      kb_json_append_escaped(&out, err);
      dstr_append_str(&out, "\"}");
      free(err);
      char *result = safe_strdup(out.data ? out.data : "{\"error\":\"unknown\"}");
      dstr_free(&out);
      return result;
   }

   dstr_t out;
   dstr_init(&out);
   char num[32];
   dstr_append_str(&out, "{\"fusion_mode\":\"");
   dstr_append_str(&out, fusion_mode);
   dstr_append_str(&out, "\"");
   if (strcmp(fusion_mode, "rrf") != 0)
   {
      dstr_append_str(&out, ",\"fusion_alpha\":");
      snprintf(num, sizeof(num), "%.4f", fusion_alpha);
      dstr_append_str(&out, num);
   }
   dstr_append_str(&out, ",\"results\":[");
   for (int i = 0; i < n_results; i++)
   {
      if (i > 0)
         dstr_append_str(&out, ",");
      dstr_append_str(&out, "{\"file_path\":\"");
      kb_json_append_escaped(&out, merged[i].file_path);
      dstr_append_str(&out, "\",\"line_start\":");
      snprintf(num, sizeof(num), "%d", merged[i].line_start);
      dstr_append_str(&out, num);
      dstr_append_str(&out, ",\"line_end\":");
      snprintf(num, sizeof(num), "%d", merged[i].line_end);
      dstr_append_str(&out, num);
      dstr_append_str(&out, ",\"heading_path\":\"");
      kb_json_append_escaped(&out, merged[i].heading_path);
      dstr_append_str(&out, "\",\"content\":\"");
      kb_json_append_escaped(&out, merged[i].content);
      dstr_append_str(&out, "\",\"score\":");
      snprintf(num, sizeof(num), "%.6f", merged[i].score);
      dstr_append_str(&out, num);
      /* doc_id keys the row to its feature_rows — needed by the learning-to-rank
       * outcome capture (the file_path-keyed http `hits` shape lacks it). */
      dstr_append_str(&out, ",\"doc_id\":");
      snprintf(num, sizeof(num), "%lld", (long long)merged[i].doc_id);
      dstr_append_str(&out, num);
      dstr_append_str(&out, "}");
   }
   dstr_append_str(&out, "]}");

   free(merged);
   char *result = safe_strdup(out.data ? out.data : "{\"results\":[]}");
   dstr_free(&out);
   return result;
}

/* ------------------------------------------------------------------ */
/* Utility                                                              */
/* ------------------------------------------------------------------ */

void kb_resolve_project(const char *project, const char *root_path, char *out, size_t out_len)
{
   if (project && project[0])
   {
      snprintf(out, out_len, "%s", project);
      return;
   }
   char path[MAX_PATH_LEN];
   if (root_path && root_path[0])
      snprintf(path, sizeof(path), "%s", root_path);
   else if (!getcwd(path, sizeof(path)))
      path[0] = '\0';
   const char *base = strrchr(path, '/');
   snprintf(out, out_len, "%s", base ? base + 1 : path);
}

#endif /* !AIMEE_DB2_DISABLED */
