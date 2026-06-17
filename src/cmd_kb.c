/* cmd_kb.c: project knowledge base CLI (build, update, search, status, clear, lab, health) */
#include "aimee.h"
#include "commands.h"
#include "kb.h"
#include "headers/kb_lab.h"
#include "kb_client.h"
#include "cmd_kb_export.h"
#include "cJSON.h"
#include "config.h"
#include "memory.h"
#include "headers/curator_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void kb_require_runtime_result(const char *result, const char *op)
{
   if (!result || strncmp(result, "error:", 6) != 0)
      return;
   fatal("%s failed: %s", op, result + 7);
}

static void print_stats(const kb_stats_t *stats)
{
   printf("  files scanned:  %d\n", stats->files_scanned);
   printf("  files indexed:  %d\n", stats->files_indexed);
   printf("  files skipped:  %d (up-to-date)\n", stats->files_skipped);
   printf("  chunks added:   %d\n", stats->chunks_added);
   printf("  embeddings:     %d\n", stats->embeddings_added);
}

static int kb_cmd_json_int(cJSON *obj, const char *key, int fallback)
{
   cJSON *n = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
   return cJSON_IsNumber(n) ? (int)n->valuedouble : fallback;
}

/* ------------------------------------------------------------------ */
/* kb build                                                             */
/* ------------------------------------------------------------------ */

static int kb_extract_stats(cJSON *resp, kb_stats_t *stats)
{
   memset(stats, 0, sizeof(*stats));
   cJSON *n;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "files_scanned")) && cJSON_IsNumber(n))
      stats->files_scanned = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "files_indexed")) && cJSON_IsNumber(n))
      stats->files_indexed = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "files_skipped")) && cJSON_IsNumber(n))
      stats->files_skipped = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "files_removed")) && cJSON_IsNumber(n))
      stats->files_removed = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "chunks_added")) && cJSON_IsNumber(n))
      stats->chunks_added = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "chunks_removed")) && cJSON_IsNumber(n))
      stats->chunks_removed = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "embeddings_added")) && cJSON_IsNumber(n))
      stats->embeddings_added = (int)n->valuedouble;
   return 0;
}

static int kb_async_pending_from_resp(cJSON *resp)
{
   cJSON *queue = cJSON_GetObjectItemCaseSensitive(resp, "async_queue");
   if (!queue)
      return 0;
   cJSON *pending = cJSON_GetObjectItemCaseSensitive(queue, "pending");
   if (!cJSON_IsNumber(pending))
      return 0;
   return (int)pending->valuedouble;
}

/* `kb build` was removed: knowledge-base document ingestion is no longer a
 * separate command. Prose/doc files are now chunked + embedded into the KB-docs
 * layer automatically during workspace ingestion (the curator drain's
 * kb_doc_refresh pass, reading content from DB2), which also works on the thin-
 * client/remote deploy where the server cannot read the project from disk. */

/* ------------------------------------------------------------------ */
/* kb update                                                            */
/* ------------------------------------------------------------------ */

static void kb_cmd_update(app_ctx_t *ctx, int argc, char **argv)
{
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *path = opt_get(&opts, "path");
   const char *project = opt_get(&opts, "project");

   char root[MAX_PATH_LEN];
   if (path && path[0])
      snprintf(root, sizeof(root), "%s", path);
   else if (!getcwd(root, sizeof(root)))
      root[0] = '\0';

   config_t cfg;
   config_load(&cfg);
   /* Pass NULL when no local embedder is configured (the thin client has none)
    * so the kb embeds with its OWN embedder; defaulting to "builtin" (384-dim
    * hash) would mismatch a real-embedder corpus (1024/2560-dim) and return
    * nothing. */
   const char *embed_cmd = cfg.embedding_command[0] ? cfg.embedding_command : NULL;

   char proj[256];
   kb_resolve_project(project, root, proj, sizeof(proj));

   if (!ctx->json_output)
      printf("Updating KB for project '%s'...\n", proj);

   char *resp_json = kb_client_update_json(root, proj, embed_cmd);
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);

   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (!ok)
   {
      const char *msg = "kb update failed";
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      if (!ctx->json_output)
         fprintf(stderr, "KB update failed: %s\n", msg);
      else
         printf("{\"status\":\"error\",\"message\":\"%s\"}\n", msg);
      cJSON_Delete(resp);
      return;
   }

   kb_stats_t stats;
   kb_extract_stats(resp, &stats);

   if (ctx->json_output)
   {
      printf("{\"status\":\"ok\",\"project\":\"%s\","
             "\"files_indexed\":%d,\"files_skipped\":%d,\"chunks_added\":%d}\n",
             proj, stats.files_indexed, stats.files_skipped, stats.chunks_added);
   }
   else
   {
      printf("Done.\n");
      print_stats(&stats);
      int pending = kb_async_pending_from_resp(resp);
      if (pending > 0)
         printf("  async queue: %d pending job(s)\n", pending);
   }
   cJSON_Delete(resp);
}

/* ------------------------------------------------------------------ */
/* kb search                                                            */
/* ------------------------------------------------------------------ */

static void kb_cmd_search(app_ctx_t *ctx, int argc, char **argv)
{
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *query = opt_pos(&opts, 0);
   if (!query)
      fatal("usage: aimee kb search <query> [--project name] [--max N] [--fusion-mode "
            "rrf|static_alpha|dynamic_alpha]");

   const char *project = opt_get(&opts, "project");
   int max_results = opt_get_int(&opts, "max", KB_DEFAULT_MAX_RESULTS);
   const char *fusion_mode_override = opt_get(&opts, "fusion-mode");

   config_t cfg;
   config_load(&cfg);
   /* Pass NULL when no local embedder is configured (the thin client has none)
    * so the kb embeds with its OWN embedder; defaulting to "builtin" (384-dim
    * hash) would mismatch a real-embedder corpus (1024/2560-dim) and return
    * nothing. */
   const char *embed_cmd = cfg.embedding_command[0] ? cfg.embedding_command : NULL;

   char proj[256];
   kb_resolve_project(project, NULL, proj, sizeof(proj));

   const char *format = ctx->json_output ? "json" : "text";
   char *resp_json =
       kb_client_search_json_ex(proj, query, embed_cmd, max_results, format, fusion_mode_override);
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);

   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (!ok)
   {
      const char *msg = "kb search failed";
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      fatal("kb search failed: %s", msg);
   }

   cJSON *result_field = cJSON_GetObjectItemCaseSensitive(resp, "result");
   const char *result = cJSON_IsString(result_field) ? result_field->valuestring : "";
   kb_require_runtime_result(result, "kb search");
   puts(result);
   cJSON_Delete(resp);
}

/* ------------------------------------------------------------------ */
/* kb status                                                            */
/* ------------------------------------------------------------------ */

static void kb_cmd_status(app_ctx_t *ctx, int argc, char **argv)
{
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *project = opt_get(&opts, "project");

   char proj[256];
   kb_resolve_project(project, NULL, proj, sizeof(proj));

   char *resp_json = kb_client_project_status_json(proj);
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;

   cJSON *status_field = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = cJSON_IsString(status_field) && strcmp(status_field->valuestring, "ok") == 0;
   if (!ok)
   {
      const char *msg = "kb status unavailable";
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      if (ctx->json_output)
         printf("{\"status\":\"error\",\"message\":\"%s\"}\n", msg);
      else
         fprintf(stderr, "KB status failed: %s\n", msg);
      free(resp_json);
      cJSON_Delete(resp);
      return;
   }

   int files = 0, chunks = 0, tokens = 0, embeddings = 0;
   cJSON *n;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "files")) && cJSON_IsNumber(n))
      files = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "chunks")) && cJSON_IsNumber(n))
      chunks = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "tokens")) && cJSON_IsNumber(n))
      tokens = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "embeddings")) && cJSON_IsNumber(n))
      embeddings = (int)n->valuedouble;

   char text[512];
   snprintf(text, sizeof(text),
            "KB project: %s\n  files: %d\n  chunks: %d\n  tokens: ~%d\n"
            "  embeddings: %d\n",
            proj, files, chunks, tokens, embeddings);

   if (ctx->json_output)
      printf("{\"status\":%s}\n", text);
   else
      puts(text);

   free(resp_json);
   cJSON_Delete(resp);
}

/* ------------------------------------------------------------------ */
/* kb clear                                                             */
/* ------------------------------------------------------------------ */

static void kb_cmd_clear(app_ctx_t *ctx, int argc, char **argv)
{
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *project = opt_get(&opts, "project");

   char proj[256];
   kb_resolve_project(project, NULL, proj, sizeof(proj));

   char *resp_json = kb_client_clear_json(proj);
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);

   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (!ok)
   {
      const char *msg = "kb clear failed";
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      if (ctx->json_output)
         printf("{\"status\":\"error\",\"message\":\"%s\"}\n", msg);
      else
         fprintf(stderr, "KB clear failed: %s\n", msg);
      cJSON_Delete(resp);
      return;
   }

   int deleted = 0;
   cJSON *n = cJSON_GetObjectItemCaseSensitive(resp, "chunks_deleted");
   if (cJSON_IsNumber(n))
      deleted = (int)n->valuedouble;

   if (ctx->json_output)
      printf("{\"status\":\"ok\",\"chunks_deleted\":%d}\n", deleted);
   else
      printf("Cleared %d chunks for project '%s'.\n", deleted, proj);
   cJSON_Delete(resp);
}

/* ------------------------------------------------------------------ */
/* kb docs                                                             */
/* ------------------------------------------------------------------ */

static void kb_cmd_docs(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1 || strcmp(argv[0], "push") != 0)
      fatal("usage: aimee kb docs push [--scope SCOPE|--project NAME] <file> [file...]");

   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc - 1, argv + 1, bool_flags, &opts);

   if (opts.pos_count <= 0)
      fatal("usage: aimee kb docs push [--scope SCOPE|--project NAME] <file> [file...]");

   const char *scope = opt_get(&opts, "scope");
   if (!scope || !scope[0])
      scope = opt_get(&opts, "project");
   if (!scope || !scope[0])
      scope = "global";

   const char *push_paths[OPT_MAX_POSITIONAL];
   char abs_paths[OPT_MAX_POSITIONAL][MAX_PATH_LEN];
   char cwd_buf[MAX_PATH_LEN] = "";
   getcwd(cwd_buf, sizeof(cwd_buf));
   for (int i = 0; i < opts.pos_count; i++)
   {
      const char *path = opts.positional[i];
      if (path && path[0] != '/' && cwd_buf[0])
      {
         snprintf(abs_paths[i], sizeof(abs_paths[i]), "%s/%s", cwd_buf, path);
         path = abs_paths[i];
      }
      push_paths[i] = path;
   }

   char *resp_json = kb_client_docs_push_json(scope, push_paths, opts.pos_count);
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;

   if (ctx->json_output)
   {
      printf("%s\n",
             resp_json ? resp_json : "{\"status\":\"error\",\"message\":\"docs push failed\"}");
      cJSON_Delete(resp);
      free(resp_json);
      return;
   }

   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (!ok)
   {
      const char *msg = "docs push failed";
      cJSON *m = resp ? cJSON_GetObjectItemCaseSensitive(resp, "message") : NULL;
      if (cJSON_IsString(m) && m->valuestring[0])
         msg = m->valuestring;
      fprintf(stderr, "Docs push failed: %s\n", msg);
      cJSON_Delete(resp);
      free(resp_json);
      return;
   }

   int total = kb_cmd_json_int(resp, "total", opts.pos_count);
   int uploaded = kb_cmd_json_int(resp, "uploaded", 0);
   int skipped = kb_cmd_json_int(resp, "skipped", total - uploaded);
   int failed = kb_cmd_json_int(resp, "failed", 0);
   printf("Docs push complete for scope '%s': %d uploaded, %d skipped, %d failed (%d total).\n",
          scope, uploaded, skipped, failed, total);

   cJSON_Delete(resp);
   free(resp_json);
}

static void kb_cmd_repair(app_ctx_t *ctx, int argc, char **argv)
{
   static const char *bool_flags[] = {"reindex-corpus", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   int reindex_corpus = opt_get_flag(&opts, "reindex-corpus");
   const char *path = opt_get(&opts, "path");
   const char *project = opt_get(&opts, "project");

   if (reindex_corpus)
   {
      config_t cfg;
      config_load(&cfg);

      const char *configured =
          cfg.db2_vector_corpus_index[0] ? cfg.db2_vector_corpus_index : "auto";
      int64_t threshold = cfg.db2_vector_corpus_diskann_threshold > 0
                              ? cfg.db2_vector_corpus_diskann_threshold
                              : 1000000;

      /* Resolve which index type the config selects (no DB needed for the policy). */
      const char *index_type;
      if (strcmp(configured, "hnsw") == 0)
         index_type = "hnsw";
      else if (strcmp(configured, "diskann") == 0)
         index_type = "diskann";
      else
         index_type = "auto"; /* resolved to hnsw or diskann at runtime by aimee-kb */

      if (ctx->json_output)
      {
         printf("{\"status\":\"ok\",\"action\":\"reindex-corpus\","
                "\"corpus_index\":\"%s\",\"corpus_diskann_threshold\":%lld,"
                "\"note\":\"send to aimee-kb when corpus tables exist\"}\n",
                index_type, (long long)threshold);
      }
      else
      {
         printf("Corpus vector reindex — policy:\n");
         printf("  corpus_index:              %s\n", configured);
         printf("  corpus_diskann_threshold:  %lld vectors\n", (long long)threshold);
         printf("  resolved index type:       %s\n", index_type);
         printf("Note: actual reindex runs via 'aimee-kb' once corpus tables exist.\n");
         printf("      Run `aimee kb repair --reindex-corpus` again after the deep-curator\n");
         printf("      proposal is fully implemented to rebuild corpus vector indexes.\n");
      }
      return;
   }

   char root[MAX_PATH_LEN];
   if (path && path[0])
      snprintf(root, sizeof(root), "%s", path);
   else if (!getcwd(root, sizeof(root)))
      root[0] = '\0';

   config_t cfg;
   config_load(&cfg);
   /* Pass NULL when no local embedder is configured (the thin client has none)
    * so the kb embeds with its OWN embedder; defaulting to "builtin" (384-dim
    * hash) would mismatch a real-embedder corpus (1024/2560-dim) and return
    * nothing. */
   const char *embed_cmd = cfg.embedding_command[0] ? cfg.embedding_command : NULL;

   char proj[256];
   kb_resolve_project(project, root, proj, sizeof(proj));

   if (!ctx->json_output)
      printf("Repairing documentation index for project '%s' from %s...\n", proj, root);

   char *resp_json = kb_client_repair_json(root, proj, embed_cmd);
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);

   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (!ok)
   {
      const char *msg = "knowledge service repair failed";
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      if (ctx->json_output)
         printf("{\"status\":\"error\",\"message\":\"%s\"}\n", msg);
      else
         fprintf(stderr, "Knowledge service repair failed: %s\n", msg);
      cJSON_Delete(resp);
      return;
   }

   kb_stats_t stats;
   memset(&stats, 0, sizeof(stats));
   cJSON *n;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "files_scanned")) && cJSON_IsNumber(n))
      stats.files_scanned = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "files_indexed")) && cJSON_IsNumber(n))
      stats.files_indexed = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "files_skipped")) && cJSON_IsNumber(n))
      stats.files_skipped = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "files_removed")) && cJSON_IsNumber(n))
      stats.files_removed = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "chunks_added")) && cJSON_IsNumber(n))
      stats.chunks_added = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "chunks_removed")) && cJSON_IsNumber(n))
      stats.chunks_removed = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "embeddings_added")) && cJSON_IsNumber(n))
      stats.embeddings_added = (int)n->valuedouble;

   if (ctx->json_output)
   {
      printf("{\"status\":\"ok\",\"project\":\"%s\","
             "\"files_indexed\":%d,\"chunks_added\":%d,\"embeddings\":%d}\n",
             proj, stats.files_indexed, stats.chunks_added, stats.embeddings_added);
   }
   else
   {
      printf("KB vector repair complete.\n");
      print_stats(&stats);
   }
   cJSON_Delete(resp);
}

/* ------------------------------------------------------------------ */
/* kb lab                                                               */
/* ------------------------------------------------------------------ */

static void kb_cmd_lab(app_ctx_t *ctx, int argc, char **argv)
{
   static const char *bool_flags[] = {"json", "content", "compare-strategies", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *path = opt_pos(&opts, 0);
   if (!path)
      fatal("usage: aimee kb lab <file> [--strategy heading|paragraph|fixed|auto]"
            " [--compare-strategies] [--json] [--content]");

   int json_out = opt_get_flag(&opts, "json") || (ctx && ctx->json_output);
   int show_content = opt_get_flag(&opts, "content");
   int compare = opt_get_flag(&opts, "compare-strategies");
   const char *strat_s = opt_get(&opts, "strategy");

   if (compare)
   {
      static const kb_lab_strategy_t all_strategies[] = {
          KB_LAB_STRATEGY_HEADING_AWARE,
          KB_LAB_STRATEGY_PARAGRAPH,
          KB_LAB_STRATEGY_FIXED_SIZE,
      };
      kb_lab_compare_t cmp;
      kb_lab_compare_strategies(path, all_strategies, 3, &cmp);
      if (json_out)
         kb_lab_compare_print_json(&cmp);
      else
         kb_lab_compare_print(&cmp);
      return;
   }

   kb_lab_strategy_t strategy = KB_LAB_STRATEGY_AUTO;
   if (strat_s)
   {
      if (strcmp(strat_s, "heading") == 0)
         strategy = KB_LAB_STRATEGY_HEADING_AWARE;
      else if (strcmp(strat_s, "paragraph") == 0)
         strategy = KB_LAB_STRATEGY_PARAGRAPH;
      else if (strcmp(strat_s, "fixed") == 0)
         strategy = KB_LAB_STRATEGY_FIXED_SIZE;
      else if (strcmp(strat_s, "auto") != 0)
         fatal("kb lab: unknown strategy '%s'; use heading, paragraph, fixed, or auto", strat_s);
   }

   kb_lab_report_t report;
   if (kb_lab_run_strategy(path, strategy, &report) != 0 && report.chunk_count == 0)
   {
      fprintf(stderr, "kb lab: could not read %s\n", path);
      exit(1);
   }

   if (json_out)
      kb_lab_print_json(&report);
   else
      kb_lab_print(&report, show_content);
}

/* ------------------------------------------------------------------ */
/* kb health                                                            */
/* ------------------------------------------------------------------ */

static void kb_cmd_health(app_ctx_t *ctx, int argc, char **argv)
{
   static const char *bool_flags[] = {"json", "verbose", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);
   int json_out = opt_get_flag(&opts, "json") || (ctx && ctx->json_output);

   kb_health_t h;
   int rc = kb_client_health(&h);

   if (json_out)
   {
      cJSON *obj = cJSON_CreateObject();
      if (rc != 0)
      {
         cJSON_AddStringToObject(obj, "status", "error");
         cJSON_AddStringToObject(obj, "message", "aimee-kb is not running");
         cJSON_AddBoolToObject(obj, "process_ok", 0);
      }
      else
      {
         cJSON_AddStringToObject(obj, "status", "ok");
         cJSON_AddBoolToObject(obj, "process_ok", h.process_ok);
         cJSON_AddBoolToObject(obj, "db2_ok", h.db2_ok);
         cJSON_AddBoolToObject(obj, "db2_kb_tables_ok", h.db2_kb_tables_ok);
         cJSON_AddBoolToObject(obj, "pgvec_ok", h.pgvec_ok);
         cJSON_AddBoolToObject(obj, "pgvec_collection_ok", h.pgvec_collection_ok);
         cJSON_AddNumberToObject(obj, "pgvec_vectors", h.pgvec_vectors);
         cJSON_AddNumberToObject(obj, "pgvec_indexed_vectors", h.pgvec_indexed);
         cJSON_AddBoolToObject(obj, "embed_ok", h.embed_ok);
         cJSON_AddStringToObject(obj, "embed_command", h.embed_command);
         cJSON_AddNumberToObject(obj, "freshness_days", h.freshness_days);
         cJSON_AddStringToObject(obj, "last_ingest_at", h.last_ingest_at);
         cJSON_AddNumberToObject(obj, "chunk_count", h.chunk_count);
         cJSON_AddNumberToObject(obj, "embedding_count", h.embedding_count);
         cJSON_AddStringToObject(obj, "warnings", h.warnings);
      }
      char *json = cJSON_Print(obj);
      cJSON_Delete(obj);
      if (json)
      {
         puts(json);
         free(json);
      }
      if (rc != 0)
         exit(1);
      return;
   }

   if (rc != 0)
   {
      fprintf(stderr, "KB health\n");
      fprintf(stderr, "  process:          down (aimee-kb not running)\n");
      fprintf(stderr, "\n1 error(s)\n");
      exit(1);
   }

   const char *fmt = "  %-18s%s\n";
   fprintf(stdout, "KB health\n");
   fprintf(stdout, fmt, "process:", h.process_ok ? "ok" : "down");
   fprintf(stdout, fmt, "db2 schema:", h.db2_ok ? "ok" : "FAIL");
   fprintf(stdout, fmt, "db2 kb tables:", h.db2_kb_tables_ok ? "ok" : "WARN (missing)");
   fprintf(stdout, fmt, "pgvector ext:", h.pgvec_ok ? "ok" : "FAIL");
   fprintf(stdout, fmt, "pgvec table:", h.pgvec_collection_ok ? "ok" : "FAIL");
   fprintf(stdout, fmt, "embed model:",
           h.embed_ok ? (h.embed_command[0] ? h.embed_command : "builtin") : "not configured");

   char fresh_buf[80];
   if (h.freshness_days < 0)
      snprintf(fresh_buf, sizeof(fresh_buf), "never ingested");
   else if (h.freshness_days == 0)
      snprintf(fresh_buf, sizeof(fresh_buf), "ingested today");
   else
      snprintf(fresh_buf, sizeof(fresh_buf), "%d day(s) ago", h.freshness_days);
   fprintf(stdout, fmt, "freshness:", fresh_buf);

   char stats_buf[80];
   snprintf(stats_buf, sizeof(stats_buf), "%d chunks, %d embeddings", h.chunk_count,
            h.embedding_count);
   fprintf(stdout, fmt, "stats:", stats_buf);

   if (h.warnings[0])
   {
      fprintf(stdout, "\nWarnings:\n");
      const char *p = h.warnings;
      while (*p)
      {
         const char *nl = strchr(p, '\n');
         int len = nl ? (int)(nl - p) : (int)strlen(p);
         fprintf(stdout, "  - %.*s\n", len, p);
         p += len + (nl ? 1 : 0);
      }
   }
}

/* ------------------------------------------------------------------ */
/* kb terms / kb gaps                                                   */
/* ------------------------------------------------------------------ */

static void kb_cmd_terms(app_ctx_t *ctx, int argc, char **argv)
{
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);
   const char *limit_s = opt_get(&opts, "limit");
   int limit = limit_s ? atoi(limit_s) : 20;
   if (limit <= 0 || limit > 200)
      limit = 20;

   char *raw = kb_client_artifacts_list_proposed_json(NULL, limit * 4);
   if (!raw)
   {
      fprintf(stderr, "terms: no response from aimee-kb\n");
      return;
   }
   if (ctx && ctx->json_output)
   {
      puts(raw);
      free(raw);
      return;
   }
   cJSON *resp = cJSON_ParseWithLength(raw, strlen(raw));
   free(raw);
   if (!resp)
      return;
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "artifacts");
   int shown = 0;
   if (cJSON_IsArray(arr))
   {
      cJSON *item;
      cJSON_ArrayForEach(item, arr)
      {
         cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
         if (!cJSON_IsString(kind) || strcmp(kind->valuestring, "term_mapping") != 0)
            continue;
         if (shown >= limit)
            break;
         cJSON *payload = cJSON_GetObjectItemCaseSensitive(item, "payload");
         cJSON *raw_term = payload ? cJSON_GetObjectItemCaseSensitive(payload, "raw_term") : NULL;
         cJSON *preferred =
             payload ? cJSON_GetObjectItemCaseSensitive(payload, "preferred_term") : NULL;
         cJSON *state = cJSON_GetObjectItemCaseSensitive(item, "state");
         cJSON *scope_kind = cJSON_GetObjectItemCaseSensitive(item, "scope_kind");
         printf("%-32s -> %-32s [%s] (%s)\n",
                cJSON_IsString(raw_term) ? raw_term->valuestring : "?",
                cJSON_IsString(preferred) ? preferred->valuestring : "?",
                cJSON_IsString(state) ? state->valuestring : "proposed",
                cJSON_IsString(scope_kind) ? scope_kind->valuestring : "global");
         shown++;
      }
   }
   cJSON_Delete(resp);
   if (shown == 0)
      printf("No term_mapping artifacts found.\n");
}

static void kb_cmd_gaps(app_ctx_t *ctx, int argc, char **argv)
{
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);
   const char *limit_s = opt_get(&opts, "limit");
   int limit = limit_s ? atoi(limit_s) : 20;
   if (limit <= 0 || limit > 200)
      limit = 20;

   char *raw = kb_client_artifacts_list_proposed_json(NULL, limit * 4);
   if (!raw)
   {
      fprintf(stderr, "gaps: no response from aimee-kb\n");
      return;
   }
   if (ctx && ctx->json_output)
   {
      puts(raw);
      free(raw);
      return;
   }
   cJSON *resp = cJSON_ParseWithLength(raw, strlen(raw));
   free(raw);
   if (!resp)
      return;
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "artifacts");
   int shown = 0;
   if (cJSON_IsArray(arr))
   {
      cJSON *item;
      cJSON_ArrayForEach(item, arr)
      {
         cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
         if (!cJSON_IsString(kind) || strcmp(kind->valuestring, "gap") != 0)
            continue;
         if (shown >= limit)
            break;
         cJSON *payload = cJSON_GetObjectItemCaseSensitive(item, "payload");
         cJSON *gap_kind = payload ? cJSON_GetObjectItemCaseSensitive(payload, "gap_kind") : NULL;
         cJSON *subject = payload ? cJSON_GetObjectItemCaseSensitive(payload, "subject") : NULL;
         cJSON *state = cJSON_GetObjectItemCaseSensitive(item, "state");
         printf("%-24s: %-48s [%s]\n", cJSON_IsString(gap_kind) ? gap_kind->valuestring : "unknown",
                cJSON_IsString(subject) ? subject->valuestring : "?",
                cJSON_IsString(state) ? state->valuestring : "proposed");
         shown++;
      }
   }
   cJSON_Delete(resp);
   if (shown == 0)
      printf("No gap artifacts found.\n");
}

/* ------------------------------------------------------------------ */
/* kb pipeline                                                          */
/* ------------------------------------------------------------------ */

static void kb_cmd_pipeline(app_ctx_t *ctx, int argc, char **argv)
{
   const char *verb = argc > 0 ? argv[0] : "status";
   char *raw = NULL;

   if (strcmp(verb, "status") == 0)
      raw = kb_client_corpus_pipeline_status_json();
   else if (strcmp(verb, "drain") == 0)
   {
      static const char *bool_flags[] = {NULL};
      opt_parsed_t opts;
      opt_parse(argc - 1, argv + 1, bool_flags, &opts);
      const char *limit_s = opt_get(&opts, "limit");
      int limit = limit_s ? atoi(limit_s) : 0;
      raw = kb_client_corpus_pipeline_drain_json(limit);
   }
   else
      fatal("usage: aimee kb pipeline [status|drain] [--limit N]");

   if (!raw)
   {
      fprintf(stderr, "pipeline: no response from aimee-kb\n");
      return;
   }
   if (ctx->json_output)
   {
      puts(raw);
      free(raw);
      return;
   }

   cJSON *resp = cJSON_ParseWithLength(raw, strlen(raw));
   free(raw);
   if (!resp)
   {
      fprintf(stderr, "pipeline: invalid response from aimee-kb\n");
      return;
   }
   cJSON *error = cJSON_GetObjectItemCaseSensitive(resp, "error");
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (cJSON_IsString(error) ||
       (cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0))
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      fprintf(stderr, "pipeline: %s\n",
              cJSON_IsString(error) ? error->valuestring
                                    : (cJSON_IsString(msg) ? msg->valuestring : "unknown error"));
      cJSON_Delete(resp);
      return;
   }

   cJSON *state = cJSON_GetObjectItemCaseSensitive(resp, "state");
   cJSON *processed = cJSON_GetObjectItemCaseSensitive(resp, "processed");
   printf("Corpus pipeline: %s\n", cJSON_IsString(state) ? state->valuestring : "unknown");
   if (cJSON_IsNumber(processed))
      printf("  processed: %d\n", (int)processed->valuedouble);
   printf("  pending:   %d\n", kb_cmd_json_int(resp, "pending", 0));
   printf("  running:   %d\n", kb_cmd_json_int(resp, "running", 0));
   printf("  complete:  %d\n", kb_cmd_json_int(resp, "done", 0));
   printf("  failed:    %d\n", kb_cmd_json_int(resp, "failed", 0));
   printf("  total:     %d\n", kb_cmd_json_int(resp, "total", 0));
   cJSON *stages = cJSON_GetObjectItemCaseSensitive(resp, "stages");
   if (cJSON_IsArray(stages) && cJSON_GetArraySize(stages) > 0)
   {
      printf("\n  Stages:\n");
      cJSON *row;
      cJSON_ArrayForEach(row, stages)
      {
         cJSON *stage = cJSON_GetObjectItemCaseSensitive(row, "stage");
         cJSON *status_row = cJSON_GetObjectItemCaseSensitive(row, "status");
         cJSON *count = cJSON_GetObjectItemCaseSensitive(row, "count");
         printf("    %-22s %-10s %d\n", cJSON_IsString(stage) ? stage->valuestring : "?",
                cJSON_IsString(status_row) ? status_row->valuestring : "?",
                cJSON_IsNumber(count) ? (int)count->valuedouble : 0);
      }
   }
   cJSON_Delete(resp);
}

/* ------------------------------------------------------------------ */
/* Dispatch table                                                       */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* kb curator status — read the curator block from /v1/health (§4)     */
/* ------------------------------------------------------------------ */

static void kb_curator_print_tier(const char *label, cJSON *tier)
{
   int configured = tier && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(tier, "configured"));
   if (!configured)
   {
      printf("  %-8s idle (no provider configured)\n", label);
      return;
   }
   cJSON *base = cJSON_GetObjectItemCaseSensitive(tier, "base_url");
   cJSON *model = cJSON_GetObjectItemCaseSensitive(tier, "model");
   printf("  %-8s %s  (%s)\n", label,
          cJSON_IsString(model) && model->valuestring[0] ? model->valuestring : "(model unset)",
          cJSON_IsString(base) ? base->valuestring : "");
}

static void kb_cmd_curator(app_ctx_t *ctx, int argc, char **argv)
{
   const char *verb = (argc > 0 && argv[0][0] != '-') ? argv[0] : "status";
   if (strcmp(verb, "status") != 0)
   {
      fprintf(stderr, "Usage: aimee kb curator status [--json]\n");
      return;
   }
   int json_out = ctx && ctx->json_output;
   for (int i = 0; i < argc; i++)
      if (strcmp(argv[i], "--json") == 0)
         json_out = 1;

   char *body = kb_client_health_json();
   if (!body)
   {
      if (json_out)
         puts("{\"status\":\"error\",\"message\":\"aimee-kb is not running\"}");
      else
         fprintf(stderr, "curator status: aimee-kb is not running\n");
      exit(1);
   }
   cJSON *root = cJSON_Parse(body);
   free(body);
   cJSON *cur = root ? cJSON_GetObjectItemCaseSensitive(root, "curator") : NULL;
   if (!cur)
   {
      if (json_out)
         puts("{\"status\":\"error\",\"message\":\"curator block unavailable\"}");
      else
         fprintf(stderr, "curator status: curator block unavailable (older aimee-kb?)\n");
      cJSON_Delete(root);
      exit(1);
   }

   if (json_out)
   {
      char *j = cJSON_Print(cur);
      if (j)
      {
         puts(j);
         free(j);
      }
      else
      {
         puts("{\"status\":\"error\",\"message\":\"curator status serialization failed\"}");
      }
      cJSON_Delete(root);
      return;
   }

   printf("Curator\n");
   kb_curator_print_tier("tier-A:", cJSON_GetObjectItemCaseSensitive(cur, "tier_a"));
   kb_curator_print_tier("tier-B:", cJSON_GetObjectItemCaseSensitive(cur, "tier_b"));
   cJSON *q = cJSON_GetObjectItemCaseSensitive(cur, "queue");
   if (q)
      printf("  queue:   extract %d pending / %d done; code_unit %d pending / %d done\n",
             kb_cmd_json_int(q, "extract_pending", 0), kb_cmd_json_int(q, "extract_done", 0),
             kb_cmd_json_int(q, "code_unit_pending", 0), kb_cmd_json_int(q, "code_unit_done", 0));
   cJSON_Delete(root);
}

static void kb_cmd_curator_profile(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   const char *endpoint_url = NULL;
   for (int i = 0; i < argc - 1; i++)
   {
      if (strcmp(argv[i], "--endpoint") == 0)
         endpoint_url = argv[i + 1];
   }

   curator_profile_t p = curator_profile_detect_and_select(endpoint_url);
   char desc[512] = "";
   curator_profile_describe(&p, desc, sizeof(desc));

   if (ctx && ctx->json_output)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "backend", curator_backend_name(p.backend));
      cJSON_AddStringToObject(o, "model", p.model);
      cJSON_AddStringToObject(o, "extract_command", p.extract_command);
      if (p.endpoint_url[0])
         cJSON_AddStringToObject(o, "endpoint_url", p.endpoint_url);
      cJSON_AddBoolToObject(o, "docs_enabled", p.docs_enabled);
      cJSON_AddBoolToObject(o, "code_enabled", p.code_enabled);
      emit_json_ctx(o, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("curator profile: %s\n", desc);
   }
}

static const subcmd_t kb_subcmds[] = {
    {"repair", "Rebuild KB vector state from project docs (--path DIR, --project NAME)",
     kb_cmd_repair},
    {"update", "Incrementally update KB (only re-indexes changed files)", kb_cmd_update},
    {"search", "Search KB: aimee kb search <query> [--max N]", kb_cmd_search},
    {"status", "Show KB statistics for a project", kb_cmd_status},
    {"clear", "Delete all KB chunks for a project", kb_cmd_clear},
    {"docs", "Push docs over /v1 with manifest pre-check: docs push [--scope SCOPE] <file>...",
     kb_cmd_docs},
    {"pipeline", "Show or drain corpus processing pipeline: pipeline [status|drain]",
     kb_cmd_pipeline},
    {"lab", "Ingest lab: preview chunks and quality signals for a file (--json, --content)",
     kb_cmd_lab},
    {"health", "Check KB subsystem health (--json)", kb_cmd_health},
    {"export",
     "Export KB to markdown or JSON: [--format obsidian|json] [--out PATH] [--workspace NAME] "
     "[--kind KIND] [--since DATE] [--include-pruned]",
     kb_cmd_export},
    {"import", "Import a JSON KB export: <file.json> [--workspace NAME] [--dry-run]",
     kb_cmd_import},
    {"terms", "List term_mapping artifacts: [--limit N]", kb_cmd_terms},
    {"gaps", "List corpus gap artifacts: [--limit N]", kb_cmd_gaps},
    {"curator", "Show curator LLM provider + queue status: curator status [--json]",
     kb_cmd_curator},
    {"curator-profile", "Show hardware-selected curator backend profile [--endpoint URL] [--json]",
     kb_cmd_curator_profile},
    {NULL, NULL, NULL}};

const subcmd_t *get_kb_subcmds(void)
{
   return kb_subcmds;
}

void cmd_kb(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee kb <subcommand> [options]\n\nSubcommands:\n");
      for (int i = 0; kb_subcmds[i].name; i++)
         fprintf(stderr, "  %-12s %s\n", kb_subcmds[i].name, kb_subcmds[i].help);
      return;
   }

   /* All kb subcommands run inside aimee-kb via RPC.  The server-side
    * process owns DB1 only per the 3DB proposal, so cmd_kb has no
    * reason to open the shared DB. */
   if (subcmd_dispatch(kb_subcmds, argv[0], ctx, argc - 1, argv + 1) != 0)
   {
      fprintf(stderr, "Unknown kb subcommand: %s\n", argv[0]);
      fprintf(stderr, "Run 'aimee kb' for usage.\n");
   }
}
