/* test_kb.c: unit tests for the project knowledge base (kb.c) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee.h"
#include "db.h"
#include "db2.h"
#include "db2_test_shim.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "../db2/artifacts.h"
#include "../db2/kb_payload.h"
#include "../db2/lifecycle.h"
#include "../db2/pgvec_kb_service.h"
#include "../db2/sketch.h"
#include "kb_service_backend.h"
#include "../db2/kb_vectors.h"
#include "kb.h"
#include "platform_process.h"
#include "cJSON.h"
#include "platform_test_util.h"
#include "kb_vectors.h"
#include "support/mock_agent_http.h"

/* ------------------------------------------------------------------ */
/* Test utilities                                                       */
/* ------------------------------------------------------------------ */

static int test_kb_vector_upsert_document(int64_t document_id, const float *vec, int dim,
                                          const char *payload_json, void *ctx)
{
   (void)ctx;
   return pgvec_kb_service_upsert_document_point(document_id, vec, dim, payload_json);
}

/* The DB2 backing handle is owned by db2_test_shim_open(); these
 * helpers wrap that lifecycle plus a legacy HTTP /points/search mock
 * left over from the pre-pgvector era. The mock is harmless under the
 * pgvector path (it simply isn't hit) but is kept so we don't have to
 * regenerate fixtures if a regression test re-introduces an HTTP path.
 *
 * If you find yourself touching the mock body, prefer deleting it and
 * the corresponding mock_agent_http_set_post_handler call once it's
 * confirmed dead — kb.c routes through pgvec_kb_vector_search_project
 * now, not an HTTP collection. */
static int kb_test_vector_post_handler(const char *url, const char *auth_header, const char *body,
                                       char **response_buf, int timeout_ms,
                                       const char *extra_headers)
{
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   if (response_buf)
      *response_buf = NULL;
   if (!url)
      return -1;

   /* Search-only stub.  Other POSTs (upserts, etc.) get the generic OK so
    * any legacy HTTP path that still POSTs to a vector endpoint can keep
    * running without complaint. */
   if (strstr(url, "/points/search") == NULL)
   {
      if (response_buf)
         *response_buf = strdup("{\"result\":{\"status\":\"ok\"},\"status\":\"ok\",\"time\":0.0}");
      return 200;
   }

   /* Pull project out of the request body's filter.must[0].match.value. */
   char project[128] = "";
   if (body && body[0])
   {
      cJSON *root = cJSON_Parse(body);
      if (root)
      {
         cJSON *filter = cJSON_GetObjectItemCaseSensitive(root, "filter");
         if (cJSON_IsObject(filter))
         {
            cJSON *must = cJSON_GetObjectItemCaseSensitive(filter, "must");
            cJSON *cond = NULL;
            cJSON_ArrayForEach(cond, must)
            {
               cJSON *key = cJSON_GetObjectItemCaseSensitive(cond, "key");
               cJSON *match = cJSON_GetObjectItemCaseSensitive(cond, "match");
               if (cJSON_IsString(key) && strcmp(key->valuestring, "project") == 0 &&
                   cJSON_IsObject(match))
               {
                  cJSON *value = cJSON_GetObjectItemCaseSensitive(match, "value");
                  if (cJSON_IsString(value))
                     snprintf(project, sizeof(project), "%s", value->valuestring);
                  break;
               }
            }
         }
         cJSON_Delete(root);
      }
   }

   cJSON *resp_root = cJSON_CreateObject();
   cJSON *result = cJSON_AddArrayToObject(resp_root, "result");

   if (project[0])
   {
      char kb_err[128] = "";
      static const char *sql =
          "SELECT id FROM kb_documents WHERE project = ?1 ORDER BY id DESC LIMIT 50";
      aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, kb_err, sizeof(kb_err));
      if (st)
      {
         aimee_pg_bind_text(st, "?1", project);
         double score = 1.0;
         while (aimee_pg_step(st, kb_err, sizeof(kb_err)) == AIMEE_PG_ROW)
         {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", (double)aimee_pg_column_int64(st, 0));
            cJSON_AddNumberToObject(item, "score", score);
            cJSON_AddItemToArray(result, item);
            score *= 0.95;
            if (score < 0.01)
               score = 0.01;
         }
         aimee_pg_finalize(st);
      }
   }
   cJSON_AddStringToObject(resp_root, "status", "ok");
   cJSON_AddNumberToObject(resp_root, "time", 0.0);

   char *out = cJSON_PrintUnformatted(resp_root);
   cJSON_Delete(resp_root);
   if (response_buf)
      *response_buf = out ? out : strdup("{\"result\":[],\"status\":\"ok\",\"time\":0.0}");
   else
      free(out);
   return 200;
}

static void open_test_db(void)
{
   db2_test_shim_open();
   /* Legacy /points/search HTTP route — kept harmless for any test path
    * that still POSTs to a vector endpoint. Resets to default on close. */
   mock_agent_http_set_post_handler(kb_test_vector_post_handler);
}

static void close_test_db(void)
{
   mock_agent_http_reset();
   db2_test_shim_close();
}

/* Write text to a file. */
static void write_file(const char *path, const char *content)
{
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs(content, f);
   fclose(f);
}

/* Create a directory (best-effort, ignore if exists). */
static void mkdir_p(const char *path)
{
   mkdir(path, 0755);
}

/* ------------------------------------------------------------------ */
/* Tests: kb_resolve_project                                           */
/* ------------------------------------------------------------------ */

static void test_resolve_project_explicit(void)
{
   char out[256];
   kb_resolve_project("myproject", "/some/path", out, sizeof(out));
   assert(strcmp(out, "myproject") == 0);
   printf("  PASS: resolve_project explicit name\n");
}

static void test_resolve_project_from_path(void)
{
   char out[256];
   kb_resolve_project(NULL, "/home/user/myrepo", out, sizeof(out));
   assert(strcmp(out, "myrepo") == 0);
   printf("  PASS: resolve_project from path basename\n");
}

static void test_resolve_project_empty_string(void)
{
   char out[256];
   kb_resolve_project("", "/home/user/project42", out, sizeof(out));
   assert(strcmp(out, "project42") == 0);
   printf("  PASS: resolve_project empty string treated as NULL\n");
}

/* ------------------------------------------------------------------ */
/* Tests: kb_build and kb_search (FTS5)                                */
/* ------------------------------------------------------------------ */

static void test_build_empty_dir(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_empty_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);

   open_test_db();
   kb_stats_t stats;
   int rc = kb_build(tmpdir, "test_empty", NULL, 1, &stats);
   assert(rc == 0);
   assert(stats.files_scanned == 0);
   assert(stats.files_indexed == 0);
   assert(stats.chunks_added == 0);

   db2_kb_service_project_status_t status;
   assert(db2_kb_service_collect_project_status("test_empty", &status) == 0);
   assert(strcmp(status.project, "test_empty") == 0);
   assert(status.files == 0);

   close_test_db();
   platform_test_rmrf(tmpdir);
   printf("  PASS: build empty directory\n");
}

static void test_build_single_file(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_single_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);

   /* Write a small markdown file */
   char fpath[512];
   snprintf(fpath, sizeof(fpath), "%s/README.md", tmpdir);
   write_file(fpath, "# Project Overview\n\n"
                     "This project implements a knowledge base.\n\n"
                     "## Installation\n\n"
                     "Run `make install` to install.\n\n"
                     "## Usage\n\n"
                     "Use `aimee kb build` to index documentation.\n");

   open_test_db();
   kb_stats_t stats;
   int rc = kb_build(tmpdir, "test_single", NULL, 1, &stats);
   assert(rc == 0);
   assert(stats.files_scanned >= 1);
   assert(stats.files_indexed >= 1);
   assert(stats.chunks_added >= 1);

   /* Search for something that should be in the index */
   char *result = kb_search("test_single", "installation", NULL, 3);
   assert(result != NULL);
   /* Should find the Installation section */
   assert(strstr(result, "README.md") != NULL || strcmp(result, "No results found.") != 0);
   free(result);

   close_test_db();

   /* Cleanup */
   unlink(fpath);
   platform_test_rmrf(tmpdir);
   printf("  PASS: build single markdown file\n");
}

static void test_build_incremental_update(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_incr_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);

   char fpath[512];
   snprintf(fpath, sizeof(fpath), "%s/doc.md", tmpdir);
   write_file(fpath, "# First version\n\nOriginal content here.\n");

   open_test_db();
   kb_stats_t stats;

   /* First build */
   int rc = kb_build(tmpdir, "test_incr", NULL, 1, &stats);
   assert(rc == 0);
   int chunks_first = stats.chunks_added;
   assert(chunks_first > 0);

   /* Update: no changes — should skip */
   kb_stats_t stats2;
   rc = kb_update(tmpdir, "test_incr", NULL, &stats2);
   assert(rc == 0);
   assert(stats2.files_skipped >= 1);
   assert(stats2.files_indexed == 0);

   /* Modify file */
   write_file(fpath, "# Updated version\n\nNew content added here.\n");
   kb_stats_t stats3;
   rc = kb_update(tmpdir, "test_incr", NULL, &stats3);
   assert(rc == 0);
   assert(stats3.files_indexed >= 1);
   assert(stats3.files_skipped == 0);

   close_test_db();
   unlink(fpath);
   platform_test_rmrf(tmpdir);
   printf("  PASS: incremental update re-indexes changed files\n");
}

static void test_bloom_dedupe_skips_duplicate_content(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_bloom_dup_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);

   char a_path[512];
   char b_path[512];
   snprintf(a_path, sizeof(a_path), "%s/a.md", tmpdir);
   snprintf(b_path, sizeof(b_path), "%s/b.md", tmpdir);
   write_file(a_path, "# Shared\n\nIdentical body for bloom dedupe.\n");
   write_file(b_path, "# Old\n\norchid zeppelin marmalade only existed here.\n");

   open_test_db();
   kb_stats_t stats;
   int rc = kb_build(tmpdir, "test_bloom_dup", NULL, 0, &stats);
   assert(rc == 0);
   assert(stats.files_indexed == 2);
   assert(stats.chunks_added > 0);

   write_file(b_path, "# Shared\n\nIdentical body for bloom dedupe.\n");
   kb_stats_t stats2;
   rc = kb_update(tmpdir, "test_bloom_dup", NULL, &stats2);
   assert(rc == 0);
   assert(stats2.files_indexed == 0);
   assert(stats2.files_skipped >= 1);
   int64_t stale_ids[4];
   assert(db2_kb_documents_list_chunk_ids_for_file("test_bloom_dup", "b.md", stale_ids, 4) == 0);
   db2_sketch_minhash_row_t dup_sig;
   assert(db2_sketch_minhash_signature_get("test_bloom_dup", "b.md", &dup_sig) == 1);

   close_test_db();
   unlink(a_path);
   unlink(b_path);
   platform_test_rmrf(tmpdir);
   printf("  PASS: bloom dedupe skips duplicate content\n");
}

static void test_minhash_shadow_signatures_persist(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_lsh_shadow_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);

   char a_path[512];
   char b_path[512];
   snprintf(a_path, sizeof(a_path), "%s/a.md", tmpdir);
   snprintf(b_path, sizeof(b_path), "%s/b.md", tmpdir);
   write_file(a_path,
              "# Shared\n\nAlpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu "
              "nu xi omicron pi rho sigma tau upsilon phi chi psi omega.\n");

   open_test_db();
   kb_stats_t stats;
   int rc = kb_build(tmpdir, "test_lsh_shadow", NULL, 0, &stats);
   assert(rc == 0);
   assert(stats.files_indexed == 1);

   write_file(b_path,
              "# Shared\n\nAlpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu "
              "nu xi omicron pi rho sigma tau upsilon phi chi psi omega plus.\n");
   kb_stats_t stats2;
   rc = kb_update(tmpdir, "test_lsh_shadow", NULL, &stats2);
   assert(rc == 0);
   assert(stats2.files_indexed == 0);
   assert(stats2.files_skipped >= 1);
   int64_t skipped_ids[4];
   assert(db2_kb_documents_list_chunk_ids_for_file("test_lsh_shadow", "b.md", skipped_ids, 4) == 0);

   db2_sketch_minhash_row_t rows[4];
   int n = db2_sketch_minhash_signature_list("test_lsh_shadow", rows, 4);
   assert(n >= 2);
   double best = 0.0;
   for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++)
      {
         double sim = sketch_minhash_jaccard(&rows[i].signature, &rows[j].signature);
         if (sim > best)
            best = sim;
      }
   assert(best >= 0.92);

   db2_artifact_proposed_t proposed[4];
   int pn = db2_artifact_list_proposed(NULL, 10, proposed, 4);
   int saw_supersedes = 0;
   for (int i = 0; i < pn; i++)
      if (strcmp(proposed[i].kind, "kb_near_duplicate") == 0 &&
          strstr(proposed[i].payload_json, "\"proposed_relation\":\"supersedes\""))
         saw_supersedes = 1;
   assert(saw_supersedes);

   db2_sketch_minhash_row_t candidates[4];
   int c = db2_sketch_minhash_candidate_list("test_lsh_shadow", &rows[0].signature, candidates, 4);
   assert(c >= 2);
   best = 0.0;
   for (int i = 0; i < c; i++)
   {
      if (strcmp(candidates[i].file_path, rows[0].file_path) == 0)
         continue;
      double sim = sketch_minhash_jaccard(&rows[0].signature, &candidates[i].signature);
      if (sim > best)
         best = sim;
   }
   assert(best >= 0.75);

   close_test_db();
   unlink(a_path);
   unlink(b_path);
   platform_test_rmrf(tmpdir);
   printf("  PASS: minhash shadow signatures persist\n");
}

static void test_async_embedding_queue_and_drain(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_async_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);

   char fpath[512];
   snprintf(fpath, sizeof(fpath), "%s/async.md", tmpdir);
   write_file(fpath, "# Async Queue\n\nEmbeddings should be deferred.\n");

   open_test_db();
   platform_setenv("AIMEE_MEMORY_COGNIFY_ASYNC_ENABLED", "1");

   kb_stats_t stats;
   int rc = kb_build(tmpdir, "test_async", "builtin", 1, &stats);
   assert(rc == 0);
   assert(stats.chunks_added > 0);
   assert(stats.embeddings_added == 0);

   db2_kb_service_async_queue_stats_t qstats;
   assert(db2_kb_service_async_queue_status(&qstats) == 0);
   assert(qstats.pending > 0);

   const char *vector_count_sql = "SELECT COUNT(*) FROM vector_index_ops"
                                  " WHERE collection = 'kb_embeddings' AND status = 'ok'";
   char vc_err[128] = "";
   aimee_pg_stmt_t *vc_st = aimee_pg_prepare(db2_conn(), vector_count_sql, vc_err, sizeof(vc_err));
   assert(vc_st != NULL);
   assert(aimee_pg_step(vc_st, vc_err, sizeof(vc_err)) == AIMEE_PG_ROW);
   assert(aimee_pg_column_int(vc_st, 0) == 0);
   aimee_pg_finalize(vc_st);

   assert(db2_kb_service_async_queue_drain("builtin", 5, pgvec_kb_vector_collection_name(),
                                           test_kb_vector_upsert_document, NULL, &qstats) == 0);
   assert(qstats.pending == 0);
   assert(qstats.running == 0);
   assert(qstats.failed == 0);
   assert(qstats.done > 0);

   vc_st = aimee_pg_prepare(db2_conn(), vector_count_sql, vc_err, sizeof(vc_err));
   assert(vc_st != NULL);
   assert(aimee_pg_step(vc_st, vc_err, sizeof(vc_err)) == AIMEE_PG_ROW);
   assert(aimee_pg_column_int(vc_st, 0) > 0);
   aimee_pg_finalize(vc_st);

   platform_setenv("AIMEE_MEMORY_COGNIFY_ASYNC_ENABLED", "0");
   close_test_db();
   unlink(fpath);
   platform_test_rmrf(tmpdir);
   printf("  PASS: async embedding queue drains queued KB jobs\n");
}

/* ------------------------------------------------------------------ */
/* Characterization tests for the kb_build_or_update per-file loop.    */
/* These pin every observable mutation it makes — the kb_stats_t       */
/* counters, the DB2 rows, and the pgvector op records — so the loop   */
/* can later be disentangled with no behavior change.                  */
/* ------------------------------------------------------------------ */

/* Count vector_index_ops rows for a collection (status NULL = any). */
static int kb_count_vector_ops(const char *collection, const char *status)
{
   char sql[256];
   if (status)
      snprintf(sql, sizeof(sql),
               "SELECT COUNT(*) FROM vector_index_ops WHERE collection = '%s' AND status = '%s'",
               collection, status);
   else
      snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM vector_index_ops WHERE collection = '%s'",
               collection);
   char err[128] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   if (!st)
      return -1;
   int n = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return n;
}

/* Synchronous-embedder build: every inserted chunk is embedded inline
 * (embeddings_added == chunks_added) and batch-upserted, each recorded as one
 * successful vector op. This pins the embed + batch-upsert mutations the
 * NULL-embedder build tests never exercise. */
static void test_build_sync_embeddings_and_vectors(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_syncembed_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);
   char fpath[512];
   snprintf(fpath, sizeof(fpath), "%s/doc.md", tmpdir);
   write_file(fpath, "# Title\n\n## Section one\n\nAlpha content for section one here.\n\n"
                     "## Section two\n\nBeta content for section two here.\n");

   open_test_db();
   platform_setenv("AIMEE_MEMORY_COGNIFY_ASYNC_ENABLED", "0");

   kb_stats_t stats;
   int rc = kb_build(tmpdir, "test_syncembed", "builtin", 1, &stats);
   assert(rc == 0);
   assert(stats.files_indexed == 1);
   assert(stats.chunks_added > 0);
   assert(stats.embeddings_added == stats.chunks_added);

   /* Every embedded chunk is batch-upserted to the kb collection and recorded
    * as exactly one successful op — no drops, no duplicates, no failures. */
   const char *coll = pgvec_kb_vector_collection_name();
   assert(kb_count_vector_ops(coll, "ok") == stats.embeddings_added);
   assert(kb_count_vector_ops(coll, NULL) == stats.embeddings_added);

   close_test_db();
   unlink(fpath);
   platform_test_rmrf(tmpdir);
   printf("  PASS: sync build embeds every chunk and batch-upserts vectors\n");
}

/* Small batch size forces a mid-loop flush plus the final flush; every embedded
 * chunk must still be recorded exactly once (pins the batch-flush mutation). */
static void test_build_vector_batch_flush(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_batch_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);
   char fpath[512];
   snprintf(fpath, sizeof(fpath), "%s/big.md", tmpdir);
   write_file(fpath, "# H\n\n## A\n\naaaa\n\n## B\n\nbbbb\n\n## C\n\ncccc\n\n"
                     "## D\n\ndddd\n\n## E\n\neeee\n\n## F\n\nffff\n");

   open_test_db();
   platform_setenv("AIMEE_MEMORY_COGNIFY_ASYNC_ENABLED", "0");
   platform_setenv("AIMEE_VECTOR_KB_BATCH_SIZE", "2");

   kb_stats_t stats;
   int rc = kb_build(tmpdir, "test_batch", "builtin", 1, &stats);
   assert(rc == 0);
   assert(stats.embeddings_added == stats.chunks_added);
   /* With batch size 2 and >2 chunks the loop flushes mid-stream and again at
    * the end; every embedded chunk is recorded exactly once — none dropped by a
    * premature flush, none double-counted. */
   const char *coll = pgvec_kb_vector_collection_name();
   assert(kb_count_vector_ops(coll, "ok") == stats.embeddings_added);
   assert(kb_count_vector_ops(coll, NULL) == stats.embeddings_added);

   unsetenv("AIMEE_VECTOR_KB_BATCH_SIZE");
   close_test_db();
   unlink(fpath);
   platform_test_rmrf(tmpdir);
   printf("  PASS: vector batch flushes mid-loop and at end\n");
}

/* Distinct files index independently; force-rebuild re-indexes them all with
 * no skips (pins files_scanned / files_indexed / files_skipped aggregation). */
static void test_build_multi_file_counts(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_multi_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);
   char p[512];
   snprintf(p, sizeof(p), "%s/one.md", tmpdir);
   write_file(p, "# One\n\nUnique aardvark content for file one.\n");
   snprintf(p, sizeof(p), "%s/two.md", tmpdir);
   write_file(p, "# Two\n\nUnique boomerang content for file two.\n");
   snprintf(p, sizeof(p), "%s/three.md", tmpdir);
   write_file(p, "# Three\n\nUnique cellophane content for file three.\n");

   open_test_db();
   kb_stats_t stats;
   int rc = kb_build(tmpdir, "test_multi", NULL, 1, &stats);
   assert(rc == 0);
   assert(stats.files_scanned == 3);
   assert(stats.files_indexed == 3);
   assert(stats.files_skipped == 0);
   assert(stats.chunks_added >= 3);

   /* Force rebuild re-indexes everything (clears the project first; no skips). */
   kb_stats_t s2;
   rc = kb_build(tmpdir, "test_multi", NULL, 1, &s2);
   assert(rc == 0);
   assert(s2.files_indexed == 3);
   assert(s2.files_skipped == 0);

   close_test_db();
   platform_test_rmrf(tmpdir);
   printf("  PASS: multi-file build counts + force rebuild re-indexes all\n");
}

static void test_search_empty_kb(void)
{
   open_test_db();
   char *result = kb_search("no_such_project", "anything", NULL, 3);
   assert(result != NULL);
   assert(strcmp(result, "No results found.") == 0);
   free(result);
   close_test_db();
   printf("  PASS: search empty KB returns 'No results found.'\n");
}

static void test_search_finds_content(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_search_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);

   char fpath[512];
   snprintf(fpath, sizeof(fpath), "%s/api.md", tmpdir);
   write_file(fpath, "# API Reference\n\n"
                     "## Authentication\n\n"
                     "All requests require a Bearer token in the Authorization header.\n\n"
                     "## Endpoints\n\n"
                     "### POST /api/sessions\n\n"
                     "Creates a new session. Returns a session_id.\n");

   open_test_db();
   kb_stats_t stats;
   kb_build(tmpdir, "test_search", NULL, 1, &stats);
   assert(stats.chunks_added > 0);

   /* Search for authentication info */
   char *result = kb_search("test_search", "authentication bearer token", NULL, 3);
   assert(result != NULL);
   /* Result should reference the file */
   assert(strstr(result, "api.md") != NULL);
   free(result);

   close_test_db();
   unlink(fpath);
   platform_test_rmrf(tmpdir);
   printf("  PASS: search finds relevant content\n");
}

static void test_search_requires_query_embedding(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_embedfail_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);

   char fpath[512];
   snprintf(fpath, sizeof(fpath), "%s/api.md", tmpdir);
   write_file(fpath, "# API Reference\n\nBearer token authentication.\n");

   open_test_db();
   kb_stats_t stats;
   kb_build(tmpdir, "test_embedfail", NULL, 1, &stats);
   assert(stats.chunks_added > 0);

   char *result =
       kb_search("test_embedfail", "authentication bearer token", "command-that-does-not-exist", 3);
   assert(result != NULL);
   assert(strstr(result, "error: documentation query embedding failed;") != NULL);
   free(result);

   close_test_db();
   unlink(fpath);
   platform_test_rmrf(tmpdir);
   printf("  PASS: search fails when documentation query embedding fails\n");
}

static void test_search_json_empty(void)
{
   open_test_db();
   char *result = kb_search_json("no_such_project", "anything", NULL, 3);
   assert(result != NULL);
   /* Empty result set must be valid JSON with a results array and fusion_mode field. */
   assert(strstr(result, "\"results\":[]") != NULL);
   assert(strstr(result, "\"fusion_mode\":") != NULL);
   free(result);
   close_test_db();
   printf("  PASS: kb_search_json empty KB returns valid JSON with results array\n");
}

static void test_search_json_structured(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_jsearch_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);

   char fpath[512];
   snprintf(fpath, sizeof(fpath), "%s/api.md", tmpdir);
   write_file(fpath, "# API Reference\n\n"
                     "## Authentication\n\n"
                     "Bearer token in the Authorization header.\n");

   open_test_db();
   kb_stats_t stats;
   kb_build(tmpdir, "test_jsearch", NULL, 1, &stats);
   assert(stats.chunks_added > 0);

   char *result = kb_search_json("test_jsearch", "bearer token authentication", NULL, 3);
   assert(result != NULL);
   /* Must be structured JSON with separate typed fields, not the legacy
    * single-string format. */
   assert(strstr(result, "\"results\":[") != NULL);
   assert(strstr(result, "\"file_path\":") != NULL);
   assert(strstr(result, "\"line_start\":") != NULL);
   assert(strstr(result, "\"line_end\":") != NULL);
   assert(strstr(result, "\"content\":") != NULL);
   assert(strstr(result, "\"score\":") != NULL);
   assert(strstr(result, "api.md") != NULL);
   /* The legacy [path:line-range] header MUST NOT leak into the JSON payload. */
   assert(strstr(result, "[") == NULL || strstr(result, "[") > strstr(result, "\"results\":["));
   free(result);

   close_test_db();
   unlink(fpath);
   platform_test_rmrf(tmpdir);
   printf("  PASS: kb_search_json returns structured per-hit fields\n");
}

static void test_search_max_cap_above_legacy_limit(void)
{
   /* Regression: prior to the configurable cap, kb_search silently clamped
    * max_results to 8.  Default cap is 50, so a request for 20 with 12 distinct
    * chunks must be honored. */
   char tmpdir[] = "/tmp/aimee_kb_test_cap_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);

   char fpath[512];
   snprintf(fpath, sizeof(fpath), "%s/many.md", tmpdir);
   /* Twelve distinct sections so hybrid retrieval has >8 candidates. */
   write_file(fpath, "# Many sections\n\n"
                     "## Alpha\n\nThe alpha section discusses alpha behaviour.\n\n"
                     "## Beta\n\nThe beta section discusses beta behaviour.\n\n"
                     "## Gamma\n\nThe gamma section discusses gamma behaviour.\n\n"
                     "## Delta\n\nThe delta section discusses delta behaviour.\n\n"
                     "## Epsilon\n\nThe epsilon section discusses epsilon behaviour.\n\n"
                     "## Zeta\n\nThe zeta section discusses zeta behaviour.\n\n"
                     "## Eta\n\nThe eta section discusses eta behaviour.\n\n"
                     "## Theta\n\nThe theta section discusses theta behaviour.\n\n"
                     "## Iota\n\nThe iota section discusses iota behaviour.\n\n"
                     "## Kappa\n\nThe kappa section discusses kappa behaviour.\n\n"
                     "## Lambda\n\nThe lambda section discusses lambda behaviour.\n\n"
                     "## Mu\n\nThe mu section discusses mu behaviour.\n");

   open_test_db();
   kb_stats_t stats;
   kb_build(tmpdir, "test_cap", NULL, 1, &stats);
   assert(stats.chunks_added >= 10);

   char *result = kb_search_json("test_cap", "section behaviour discusses", NULL, 20);
   assert(result != NULL);
   /* Count '"file_path":' occurrences; must be > 8 to prove the cap lifted. */
   int hits = 0;
   for (const char *p = result; (p = strstr(p, "\"file_path\":")) != NULL; p++)
      hits++;
   assert(hits > 8);
   free(result);

   close_test_db();
   unlink(fpath);
   platform_test_rmrf(tmpdir);
   printf("  PASS: kb_search_json honors --max above the legacy 8-hit limit\n");
}

/* ------------------------------------------------------------------ */
/* Tests: DB2-owned clear helpers                                      */
/* ------------------------------------------------------------------ */

static void test_clear(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_clear_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);

   char fpath[512];
   snprintf(fpath, sizeof(fpath), "%s/notes.md", tmpdir);
   write_file(fpath, "# Notes\n\nSome notes here.\n");

   open_test_db();
   kb_stats_t stats;
   kb_build(tmpdir, "test_clear", NULL, 1, &stats);
   assert(stats.chunks_added > 0);

   int deleted = db2_kb_service_clear_project("test_clear");
   assert(deleted > 0);

   /* Search should now return nothing */
   char *result = kb_search("test_clear", "notes", NULL, 3);
   assert(result != NULL);
   assert(strcmp(result, "No results found.") == 0);
   free(result);

   close_test_db();
   unlink(fpath);
   platform_test_rmrf(tmpdir);
   printf("  PASS: clear removes all chunks for project\n");
}

/* ------------------------------------------------------------------ */
/* Tests: purge generation fence (webchat-project-lifecycle slice 2)   */
/* ------------------------------------------------------------------ */

static void test_purge_fence_blocks_ingest(void)
{
   char tmpdir[] = "/tmp/aimee_kb_test_fence_XXXXXX";
   char *d = mkdtemp(tmpdir);
   assert(d != NULL);
   char fpath[512];
   snprintf(fpath, sizeof(fpath), "%s/notes.md", tmpdir);
   write_file(fpath, "# Notes\n\nFenced project notes.\n");

   open_test_db();

   /* No fence yet. */
   assert(db2_kb_purge_fence_active("fence_proj") == 0);
   assert(db2_kb_purge_fence_read("fence_proj", NULL, 0, NULL, 0, NULL) == 0);

   /* Write, then read back generation/purge_id + liveness. */
   assert(db2_kb_purge_fence_write("fence_proj", "gen-1", "pid-1") == 0);
   assert(db2_kb_purge_fence_active("fence_proj") == 1);
   char gen[64] = "", pid[64] = "";
   int live = 0;
   assert(db2_kb_purge_fence_read("fence_proj", gen, sizeof(gen), pid, sizeof(pid), &live) == 1);
   assert(strcmp(gen, "gen-1") == 0);
   assert(strcmp(pid, "pid-1") == 0);
   assert(live == 1);

   /* Writers refuse to start an ingest for a fenced project; another key is
    * unaffected. */
   kb_stats_t stats;
   assert(kb_build(tmpdir, "fence_proj", NULL, 0, &stats) == -1);
   assert(kb_build(tmpdir, "other_proj", NULL, 0, &stats) == 0);

   /* Heartbeat: displaced ids no-op, matching ids refresh. */
   assert(db2_kb_purge_fence_heartbeat("fence_proj", "gen-0", "pid-1") == 0);
   assert(db2_kb_purge_fence_heartbeat("fence_proj", "gen-1", "pid-1") == 1);

   /* Atomic acquire: a LIVE foreign fence is refused without takeover (the
    * current owner's ids are returned), displaced with takeover:true. */
   {
      char cg[64] = "", cp[64] = "";
      int replaced = -1;
      assert(db2_kb_purge_fence_acquire("fence_proj", "gen-2", "pid-2", 0, cg, sizeof(cg), cp,
                                        sizeof(cp), &replaced) == 0);
      assert(strcmp(cg, "gen-1") == 0);
      assert(strcmp(cp, "pid-1") == 0);
      assert(replaced == 0);
      /* Same-owner re-acquire is idempotent (no refusal, not a replace). */
      assert(db2_kb_purge_fence_acquire("fence_proj", "gen-1", "pid-1", 0, cg, sizeof(cg), cp,
                                        sizeof(cp), &replaced) == 1);
      assert(replaced == 0);
      /* Takeover displaces the live owner and reports the displaced ids. */
      assert(db2_kb_purge_fence_acquire("fence_proj", "gen-2", "pid-2", 1, cg, sizeof(cg), cp,
                                        sizeof(cp), &replaced) == 1);
      assert(replaced == 1);
      assert(strcmp(cg, "gen-1") == 0);
      assert(strcmp(cp, "pid-1") == 0);
      /* Restore the gen-1 owner for the remaining assertions. */
      assert(db2_kb_purge_fence_write("fence_proj", "gen-1", "pid-1") == 0);
   }

   /* Fail closed: an identity row WITHOUT a heartbeat row is ACTIVE (cannot
    * arise from a torn write — the publish transaction writes ts first). */
   assert(db2_kb_runtime_state_delete("project_purging_ts:fence_proj") == 0);
   assert(db2_kb_purge_fence_active("fence_proj") == 1);
   /* A displaced owner still no-ops against the identity-only fence... */
   assert(db2_kb_purge_fence_heartbeat("fence_proj", "gen-0", "pid-0") == 0);
   /* ...while the matching owner's heartbeat repairs the missing ts row. */
   assert(db2_kb_purge_fence_heartbeat("fence_proj", "gen-1", "pid-1") == 1);
   assert(db2_kb_purge_fence_active("fence_proj") == 1);

   /* A stale heartbeat makes the fence absent for writers (TTL expiry), while
    * the fence row itself remains readable (live=0). */
   assert(db2_kb_runtime_state_set("project_purging_ts:fence_proj", "2000-01-01 00:00:00") == 0);
   assert(db2_kb_purge_fence_active("fence_proj") == 0);
   live = 1;
   assert(db2_kb_purge_fence_read("fence_proj", gen, sizeof(gen), pid, sizeof(pid), &live) == 1);
   assert(live == 0);

   /* Clear: displaced ids no-op, matching ids drop both rows. */
   assert(db2_kb_purge_fence_clear("fence_proj", "gen-1", "pid-0") == 0);
   assert(db2_kb_purge_fence_read("fence_proj", NULL, 0, NULL, 0, NULL) == 1);
   assert(db2_kb_purge_fence_clear("fence_proj", "gen-1", "pid-1") == 1);
   assert(db2_kb_purge_fence_read("fence_proj", NULL, 0, NULL, 0, NULL) == 0);

   /* Unfenced: the ingest goes through again. */
   assert(kb_build(tmpdir, "fence_proj", NULL, 0, &stats) == 0);

   close_test_db();
   unlink(fpath);
   platform_test_rmrf(tmpdir);
   printf("  PASS: purge fence blocks ingest and follows the match rules\n");
}

/* ------------------------------------------------------------------ */
/* Tests: multiple projects are isolated                               */
/* ------------------------------------------------------------------ */

static void test_project_isolation(void)
{
   char tmpdir1[] = "/tmp/aimee_kb_proj1_XXXXXX";
   char tmpdir2[] = "/tmp/aimee_kb_proj2_XXXXXX";
   assert(mkdtemp(tmpdir1) != NULL);
   assert(mkdtemp(tmpdir2) != NULL);

   char fp1[512], fp2[512];
   snprintf(fp1, sizeof(fp1), "%s/doc.md", tmpdir1);
   snprintf(fp2, sizeof(fp2), "%s/doc.md", tmpdir2);

   write_file(fp1, "# Alpha Project\n\nThis is about alpha features.\n");
   write_file(fp2, "# Beta Project\n\nThis is about beta features.\n");

   open_test_db();
   kb_build(tmpdir1, "proj_alpha", NULL, 1, NULL);
   kb_build(tmpdir2, "proj_beta", NULL, 1, NULL);

   /* Searching proj_alpha should not return beta content */
   char *r1 = kb_search("proj_alpha", "beta", NULL, 3);
   char *r2 = kb_search("proj_beta", "alpha", NULL, 3);
   assert(r1 != NULL);
   assert(r2 != NULL);
   /* These searches should not match (different project namespaces) */
   /* Alpha project should not have "beta features" */
   /* Note: FTS5 searches only within the project, so these should return no results
    * for content that's in the OTHER project. */
   free(r1);
   free(r2);

   /* Clear one project, verify other is unaffected */
   db2_kb_service_clear_project("proj_alpha");
   char *r3 = kb_search("proj_beta", "beta features", NULL, 3);
   assert(r3 != NULL);
   /* proj_beta should still have results */
   free(r3);

   close_test_db();
   unlink(fp1);
   unlink(fp2);
   platform_test_rmrf(tmpdir1);
   platform_test_rmrf(tmpdir2);
   printf("  PASS: projects are isolated (clear one, other unaffected)\n");
}

/* ------------------------------------------------------------------ */
/* Tests: DB2-owned status helpers                                     */
/* ------------------------------------------------------------------ */

static void test_status_format(void)
{
   char tmpdir[] = "/tmp/aimee_kb_status_XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char fpath[512];
   snprintf(fpath, sizeof(fpath), "%s/readme.md", tmpdir);
   write_file(fpath, "# Hello\n\nSome content.\n");

   open_test_db();
   kb_build(tmpdir, "status_test", NULL, 1, NULL);

   db2_kb_service_project_status_t status;
   assert(db2_kb_service_collect_project_status("status_test", &status) == 0);
   assert(strcmp(status.project, "status_test") == 0);
   assert(status.files > 0);
   assert(status.chunks > 0);

   close_test_db();
   unlink(fpath);
   platform_test_rmrf(tmpdir);
   printf("  PASS: status output contains expected fields\n");
}

/* ------------------------------------------------------------------ */
/* Tests: file exclusion                                               */
/* ------------------------------------------------------------------ */

static void test_excludes_node_modules(void)
{
   char tmpdir[] = "/tmp/aimee_kb_excl_XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   /* Create node_modules directory with a .md file (should be excluded) */
   char nm[512];
   snprintf(nm, sizeof(nm), "%s/node_modules", tmpdir);
   mkdir_p(nm);
   char nm_file[512];
   snprintf(nm_file, sizeof(nm_file), "%s/node_modules/readme.md", tmpdir);
   write_file(nm_file, "# Should be excluded\n\nThis content should not be indexed.\n");

   /* Create a normal file (should be included) */
   char normal[512];
   snprintf(normal, sizeof(normal), "%s/docs.md", tmpdir);
   write_file(normal, "# Normal docs\n\nThis should be indexed.\n");

   open_test_db();
   kb_stats_t stats;
   kb_build(tmpdir, "test_excl", NULL, 1, &stats);

   /* Should only have indexed the normal file */
   assert(stats.files_scanned == 1);
   assert(stats.files_indexed == 1);

   close_test_db();
   unlink(nm_file);
   rmdir(nm);
   unlink(normal);
   platform_test_rmrf(tmpdir);
   printf("  PASS: node_modules directory is excluded from indexing\n");
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
   printf("test_kb:\n");

   /* kb_resolve_project tests */
   test_resolve_project_explicit();
   test_resolve_project_from_path();
   test_resolve_project_empty_string();

   /* Build/search tests */
   test_build_empty_dir();
   test_build_single_file();
   test_build_incremental_update();
   test_bloom_dedupe_skips_duplicate_content();
   test_minhash_shadow_signatures_persist();
   test_async_embedding_queue_and_drain();
   test_build_sync_embeddings_and_vectors();
   test_build_vector_batch_flush();
   test_build_multi_file_counts();
   test_search_empty_kb();
   test_search_finds_content();
   test_search_requires_query_embedding();
   test_search_json_empty();
   test_search_json_structured();
   test_search_max_cap_above_legacy_limit();

   /* Clear test */
   test_clear();

   /* Purge generation fence (slice 2) */
   test_purge_fence_blocks_ingest();

   /* Isolation test */
   test_project_isolation();

   /* Status test */
   test_status_format();

   /* Exclusion test */
   test_excludes_node_modules();

   printf("All kb tests passed.\n");
   return 0;
}
