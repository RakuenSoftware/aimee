/* cmd_memory_vector.c: pgvector subcommand handlers (reindex, repair,
 * reconcile, rebuild, verify, profile) plus the higher-level improve /
 * cognify / episode / assemble / cite / scene / ontology workflows. */
#include "aimee.h"
#include "cmd_memory_internal.h"
#include "entity_edges.h"
#include "platform_process.h"
#include "db2/vector_verify.h"
#include "kb.h"
#include "kb_client.h"
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct
{
   int64_t ok_ops;
   int64_t pending_ops;
   int64_t failed_ops;
   int64_t stuck_ops;
} mem_pgvec_ops_summary_t;

typedef struct
{
   int64_t point_id;
   char collection[64];
   int64_t memory_id;
   int attempts;
   char last_error[256];
   char updated_at[64];
} mem_pgvec_failed_op_t;

void mem_reindex(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   int limit = opt_get_int(&opts, "limit", 0);

   char *resp_json = kb_client_memory_reindex_json(limit);
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);

   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (!ok)
   {
      const char *msg = "memory reindex failed";
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      cJSON_Delete(resp);
      fatal("%s", msg);
   }

   int rebuilt = 0;
   cJSON *r = cJSON_GetObjectItemCaseSensitive(resp, "rebuilt");
   if (cJSON_IsNumber(r))
      rebuilt = (int)r->valuedouble;
   cJSON_Delete(resp);

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "rebuilt", rebuilt);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Rebuilt derived indexes for %d memories\n", rebuilt);
   }
}

void mem_repair(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);

   int limit = opt_get_int(&opts, "limit", 0);
   int failed_only = opt_get_flag(&opts, "failed-only");
   int reset_stuck = opt_get_flag(&opts, "reset-stuck");
   int64_t single_id = 0;
   if (opts.pos_count > 0)
      single_id = atoll(opts.positional[0]);

   const char *embed_cmd = config_embedding_command(&s_mem_cfg, NULL);
   char *resp_json =
       kb_client_memory_repair_json(limit, failed_only, reset_stuck, single_id, embed_cmd);
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);

   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (!ok)
   {
      const char *msg = "memory repair failed";
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      cJSON_Delete(resp);
      fatal("%s", msg);
   }

   cJSON *mode_j = cJSON_GetObjectItemCaseSensitive(resp, "mode");
   const char *mode =
       (cJSON_IsString(mode_j) && mode_j->valuestring[0]) ? mode_j->valuestring : "all";
   int reset_stuck_count = 0;
   int repaired = 0;
   int failed = 0;
   cJSON *n = cJSON_GetObjectItemCaseSensitive(resp, "reset_stuck");
   if (cJSON_IsNumber(n))
      reset_stuck_count = (int)n->valuedouble;
   n = cJSON_GetObjectItemCaseSensitive(resp, "repaired");
   if (cJSON_IsNumber(n))
      repaired = (int)n->valuedouble;
   n = cJSON_GetObjectItemCaseSensitive(resp, "failed");
   if (cJSON_IsNumber(n))
      failed = (int)n->valuedouble;
   int is_reset_stuck_mode = strcmp(mode, "reset_stuck") == 0;
   cJSON_Delete(resp);

   /* --reset-stuck: zero the attempts counter on permanently-failed points
    * so the next auto-retry pass picks them up again.  Useful after fixing
    * the underlying issue (e.g. restarting the embedding service). */
   if (is_reset_stuck_mode)
   {
      if (ctx->json_output)
      {
         cJSON *j = cJSON_CreateObject();
         cJSON_AddNumberToObject(j, "reset_stuck", reset_stuck_count);
         emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("Reset attempts=0 on %d stuck index ops.  Run `memory repair --failed-only` or "
                "wait for `memory maintain` to retry them.\n",
                reset_stuck_count);
      }
      return;
   }

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "repaired", repaired);
      cJSON_AddNumberToObject(j, "failed", failed);
      if (single_id > 0)
         cJSON_AddNumberToObject(j, "memory_id", (double)single_id);
      if (limit > 0)
         cJSON_AddNumberToObject(j, "limit", limit);
      if (failed_only)
         cJSON_AddBoolToObject(j, "failed_only", 1);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else if (single_id > 0)
   {
      if (failed)
         printf("Failed to repair memory %lld\n", (long long)single_id);
      else
         printf("Repaired pgvector state for memory %lld\n", (long long)single_id);
   }
   else
   {
      printf("Repaired pgvector state for %d memories", repaired);
      if (failed)
         printf(" (%d failed)", failed);
      if (failed_only)
         printf(" (scanned vector_index_ops failed rows)");
      printf("\n");
   }
}

/* aimee memory rebuild [--version <v>]
 *
 * Recreate the pgvector memory collection from scratch and re-upsert every
 * point from DB2 using the active (or specified) embedder version. Wraps
 * memory_rebuild_vector_index_for_version which holds the rebuild concurrency
 * lock, re-creates payload indexes, and stamps the schema version on
 * completion.  Use after a schema mismatch warning or drift detected by
 * `memory verify`. */

/* aimee memory reconcile [--dry-run]
 *
 * Delete orphan pgvector points that have no DB2 backing. Scrolls each
 * collection, diffs ids against the source-of-truth tables, and batch-deletes
 * leftovers. Cheaper than a full `memory rebuild` when DB2 is authoritative
 * and pgvector is just holding stale records from earlier runs. */
void mem_reconcile(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   int dry_run = opt_get_flag(&opts, "dry-run");

   char *resp_json = kb_client_reconcile_json(dry_run);
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);

   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (!ok)
   {
      const char *msg = "memory reconcile failed";
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      cJSON_Delete(resp);
      fatal("%s", msg);
   }

   int rc = 0;
   cJSON *rc_j = cJSON_GetObjectItemCaseSensitive(resp, "rc");
   if (cJSON_IsNumber(rc_j))
      rc = (int)rc_j->valuedouble;
   int64_t mem_pruned = 0, mem_kept = 0, kb_pruned = 0, kb_kept = 0;
   cJSON *mem_obj = cJSON_GetObjectItemCaseSensitive(resp, "memory");
   if (mem_obj)
   {
      cJSON *n;
      if ((n = cJSON_GetObjectItemCaseSensitive(mem_obj, "kept")) && cJSON_IsNumber(n))
         mem_kept = (int64_t)n->valuedouble;
      if ((n = cJSON_GetObjectItemCaseSensitive(mem_obj, "pruned")) && cJSON_IsNumber(n))
         mem_pruned = (int64_t)n->valuedouble;
   }
   cJSON *kb_obj = cJSON_GetObjectItemCaseSensitive(resp, "kb");
   if (kb_obj)
   {
      cJSON *n;
      if ((n = cJSON_GetObjectItemCaseSensitive(kb_obj, "kept")) && cJSON_IsNumber(n))
         kb_kept = (int64_t)n->valuedouble;
      if ((n = cJSON_GetObjectItemCaseSensitive(kb_obj, "pruned")) && cJSON_IsNumber(n))
         kb_pruned = (int64_t)n->valuedouble;
   }
   cJSON_Delete(resp);

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "status", rc == 0 ? (dry_run ? "dry-run" : "ok") : "error");
      cJSON_AddBoolToObject(j, "dry_run", dry_run);
      cJSON *m = cJSON_AddObjectToObject(j, "memory");
      cJSON_AddNumberToObject(m, "kept", (double)mem_kept);
      cJSON_AddNumberToObject(m, "pruned", (double)mem_pruned);
      cJSON *k = cJSON_AddObjectToObject(j, "kb");
      cJSON_AddNumberToObject(k, "kept", (double)kb_kept);
      cJSON_AddNumberToObject(k, "pruned", (double)kb_pruned);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("pgvector reconcile%s:\n", dry_run ? " (dry-run)" : "");
      printf("  memory: kept=%lld pruned=%lld\n", (long long)mem_kept, (long long)mem_pruned);
      printf("  kb:     kept=%lld pruned=%lld\n", (long long)kb_kept, (long long)kb_pruned);
      if (rc != 0)
         printf("  (transport error — some orphans may remain; re-run to retry)\n");
   }
}

void mem_rebuild(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *version = opt_get(&opts, "version");

   char *resp_json = kb_client_memory_rebuild_json(version);
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);

   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (!ok)
   {
      const char *msg = "memory rebuild failed";
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      cJSON_Delete(resp);
      fatal("%s", msg);
   }

   const char *resolved_version = "";
   int rebuilt = 0;
   int failed = 0;
   cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "version");
   if (cJSON_IsString(v))
      resolved_version = v->valuestring;
   cJSON *r = cJSON_GetObjectItemCaseSensitive(resp, "rebuilt");
   if (cJSON_IsNumber(r))
      rebuilt = (int)r->valuedouble;
   cJSON *f = cJSON_GetObjectItemCaseSensitive(resp, "failed");
   if (cJSON_IsNumber(f))
      failed = (int)f->valuedouble;

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "version", resolved_version);
      cJSON_AddNumberToObject(j, "rebuilt", rebuilt);
      cJSON_AddNumberToObject(j, "failed", failed);
      cJSON_AddStringToObject(j, "schema_version", pgvec_schema_version());
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Rebuilt pgvector memory collection for version '%s': %d points", resolved_version,
             rebuilt);
      if (failed)
         printf(" (%d failed)", failed);
      printf("\nSchema version stamped: %s\n", pgvec_schema_version());
   }
   cJSON_Delete(resp);
}

/* aimee memory verify [--repair]
 *
 * Report on pgvector index consistency against DB2: collection existence,
 * point counts vs memory row counts, pending/failed op counts, schema version
 * status, and lock state.  Read-only by default.  With --repair, drains
 * failed index ops and triggers a rebuild if schema version mismatches. */
static void mem_verify_run_repair(app_ctx_t *ctx, const char *embed_cmd,
                                  mem_pgvec_ops_summary_t ops_summary, int schema_match,
                                  int64_t drift_mem, int lock_held, const char *active_ver)
{
   int repaired = 0;
   int repaired_failed = 0;
   if (ops_summary.failed_ops > 0)
   {
      char *rjson = kb_client_memory_repair_json(0, 1, 0, 0, embed_cmd);
      cJSON *r = rjson ? cJSON_Parse(rjson) : NULL;
      free(rjson);
      if (r)
      {
         cJSON *st = cJSON_GetObjectItemCaseSensitive(r, "status");
         if (cJSON_IsString(st) && strcmp(st->valuestring, "ok") == 0)
         {
            cJSON *rn = cJSON_GetObjectItemCaseSensitive(r, "repaired");
            if (cJSON_IsNumber(rn))
               repaired = (int)rn->valuedouble;
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(r, "failed");
            if (cJSON_IsNumber(fn))
               repaired_failed = (int)fn->valuedouble;
         }
         cJSON_Delete(r);
      }
   }
   int rebuilt = -1;
   int rebuild_failed = 0;
   int rebuild_skipped_locked = 0;
   if (!schema_match || drift_mem != 0)
   {
      if (lock_held)
      {
         /* Another rebuild is running; --repair should not spin on the
          * lock or race with it.  Surface the condition and bail out. */
         rebuild_skipped_locked = 1;
      }
      else if (active_ver[0])
      {
         char *bjson = kb_client_memory_rebuild_json(active_ver);
         cJSON *b = bjson ? cJSON_Parse(bjson) : NULL;
         free(bjson);
         if (b)
         {
            cJSON *st = cJSON_GetObjectItemCaseSensitive(b, "status");
            if (cJSON_IsString(st) && strcmp(st->valuestring, "ok") == 0)
            {
               cJSON *rb = cJSON_GetObjectItemCaseSensitive(b, "rebuilt");
               if (cJSON_IsNumber(rb))
                  rebuilt = (int)rb->valuedouble;
               cJSON *fn = cJSON_GetObjectItemCaseSensitive(b, "failed");
               if (cJSON_IsNumber(fn))
                  rebuild_failed = (int)fn->valuedouble;
            }
            cJSON_Delete(b);
         }
      }
   }
   if (!ctx->json_output)
   {
      printf("  repair: repaired_failed_ops=%d (of %lld failed)%s%s\n", repaired,
             (long long)ops_summary.failed_ops, rebuilt >= 0 ? " rebuilt=" : "",
             rebuilt >= 0 ? "yes" : "");
      if (rebuild_skipped_locked)
         printf("  rebuild: skipped (another rebuild is already running)\n");
      else if (rebuilt > 0)
         printf("  rebuild: %d points (%d failed)\n", rebuilt, rebuild_failed);
      (void)repaired_failed;
   }
}

typedef struct
{
   const char *server_version;
   int embedder_dim, embedder_dim_matches;
   const char *mem_coll, *kb_coll;
   int mem_exists, kb_exists;
   int64_t mem_points, kb_points;
   int64_t mem_rows, unit_rows, kb_rows;
   const char *mem_indexed, *kb_indexed;
   mem_pgvec_ops_summary_t ops_summary;
   mem_pgvec_failed_op_t failed_detail[20];
   int failed_detail_count;
   int timings_trials;
   int64_t timings_total_us, timings_max_us;
   char stored_ver[64];
   const char *expected_ver;
   int schema_match, lock_held;
   int mem_missing, kb_missing;
   int64_t expected_mem, drift_mem, drift_kb;
   int overall_ok;
} mem_verify_report_t;

static void mem_verify_emit_json(const mem_verify_report_t *r, app_ctx_t *ctx, int do_detail,
                                 int do_timings)
{
   cJSON *j = cJSON_CreateObject();
   cJSON_AddBoolToObject(j, "ok", r->overall_ok);
   if (r->server_version[0])
      cJSON_AddStringToObject(j, "r->server_version", r->server_version);
   cJSON *emb = cJSON_AddObjectToObject(j, "embedder");
   cJSON_AddNumberToObject(emb, "dim", r->embedder_dim);
   cJSON_AddNumberToObject(emb, "expected_dim", 384);
   cJSON_AddBoolToObject(emb, "ok", r->embedder_dim_matches);
   cJSON *mem = cJSON_AddObjectToObject(j, "memory");
   cJSON_AddStringToObject(mem, "collection", r->mem_coll);
   cJSON_AddBoolToObject(mem, "collection_exists", r->mem_exists > 0);
   cJSON_AddNumberToObject(mem, "db2_memories", (double)r->mem_rows);
   cJSON_AddNumberToObject(mem, "db2_units", (double)r->unit_rows);
   cJSON_AddNumberToObject(mem, "expected_points", (double)r->expected_mem);
   cJSON_AddNumberToObject(mem, "vector_points", (double)r->mem_points);
   cJSON_AddNumberToObject(mem, "drift", (double)r->drift_mem);
   cJSON *kb = cJSON_AddObjectToObject(j, "kb");
   cJSON_AddStringToObject(kb, "collection", r->kb_coll);
   cJSON_AddBoolToObject(kb, "collection_exists", r->kb_exists > 0);
   cJSON_AddNumberToObject(kb, "db2_chunks", (double)r->kb_rows);
   cJSON_AddNumberToObject(kb, "vector_points", (double)r->kb_points);
   cJSON_AddNumberToObject(kb, "drift", (double)r->drift_kb);
   cJSON *ops = cJSON_AddObjectToObject(j, "index_ops");
   cJSON_AddNumberToObject(ops, "ok", (double)r->ops_summary.ok_ops);
   cJSON_AddNumberToObject(ops, "pending", (double)r->ops_summary.pending_ops);
   cJSON_AddNumberToObject(ops, "failed", (double)r->ops_summary.failed_ops);
   cJSON_AddNumberToObject(ops, "stuck", (double)r->ops_summary.stuck_ops);
   cJSON *sch = cJSON_AddObjectToObject(j, "schema");
   cJSON_AddStringToObject(sch, "expected", r->expected_ver);
   cJSON_AddStringToObject(sch, "stored", r->stored_ver);
   cJSON_AddBoolToObject(sch, "match", r->schema_match);
   cJSON_AddBoolToObject(j, "rebuild_lock_held", r->lock_held);
   cJSON *idx = cJSON_AddObjectToObject(j, "payload_indexes");
   cJSON_AddNumberToObject(idx, "memory_missing", (double)r->mem_missing);
   cJSON_AddNumberToObject(idx, "r->kb_missing", (double)r->kb_missing);
   if (r->mem_indexed)
      cJSON_AddStringToObject(idx, "memory_present", r->mem_indexed);
   if (r->kb_indexed)
      cJSON_AddStringToObject(idx, "kb_present", r->kb_indexed);
   if (do_timings)
   {
      cJSON *tim = cJSON_AddObjectToObject(j, "timings");
      cJSON_AddNumberToObject(tim, "trials", r->timings_trials);
      if (r->timings_trials > 0)
      {
         cJSON_AddNumberToObject(tim, "avg_ms",
                                 (double)r->timings_total_us / r->timings_trials / 1000.0);
         cJSON_AddNumberToObject(tim, "max_ms", (double)r->timings_max_us / 1000.0);
      }
   }
   if (do_detail)
   {
      cJSON *fails = cJSON_AddArrayToObject(j, "failed_ops");
      for (int i = 0; i < r->failed_detail_count; i++)
      {
         cJSON *row = cJSON_CreateObject();
         cJSON_AddNumberToObject(row, "point_id", (double)r->failed_detail[i].point_id);
         cJSON_AddStringToObject(row, "collection", r->failed_detail[i].collection);
         cJSON_AddNumberToObject(row, "memory_id", (double)r->failed_detail[i].memory_id);
         cJSON_AddNumberToObject(row, "attempts", r->failed_detail[i].attempts);
         cJSON_AddStringToObject(row, "last_error", r->failed_detail[i].last_error);
         cJSON_AddStringToObject(row, "updated_at", r->failed_detail[i].updated_at);
         cJSON_AddItemToArray(fails, row);
      }
   }
   emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
}

static void mem_verify_emit_text(const mem_verify_report_t *r, app_ctx_t *ctx, int do_detail)
{
   if (r->server_version[0])
      printf("Vector verify: %s (server %s)\n", r->overall_ok ? "OK" : "PROBLEMS DETECTED",
             r->server_version);
   else
      printf("Vector verify: %s\n", r->overall_ok ? "OK" : "PROBLEMS DETECTED");
   printf("  memory collection '%s': %s\n", r->mem_coll, r->mem_exists > 0 ? "present" : "MISSING");
   printf("    db2: %lld memories + %lld units (expected %lld points)\n", (long long)r->mem_rows,
          (long long)r->unit_rows, (long long)r->expected_mem);
   printf("    vector: %lld points (drift: %+lld)\n", (long long)r->mem_points,
          (long long)r->drift_mem);
   printf("  kb collection '%s': %s\n", r->kb_coll, r->kb_exists > 0 ? "present" : "MISSING");
   printf("    db2: %lld chunks\n", (long long)r->kb_rows);
   printf("    vector: %lld points (drift: %+lld)\n", (long long)r->kb_points,
          (long long)r->drift_kb);
   printf("  index ops: ok=%lld pending=%lld failed=%lld stuck=%lld\n",
          (long long)r->ops_summary.ok_ops, (long long)r->ops_summary.pending_ops,
          (long long)r->ops_summary.failed_ops, (long long)r->ops_summary.stuck_ops);
   printf("  schema: expected=%s stored=%s (%s)\n", r->expected_ver,
          r->stored_ver[0] ? r->stored_ver : "(none)", r->schema_match ? "match" : "MISMATCH");
   printf("  rebuild lock: %s\n", r->lock_held ? "HELD" : "free");
   printf("  payload indexes: memory_missing=%d r->kb_missing=%d\n", r->mem_missing, r->kb_missing);
   printf("  embedder: dim=%d expected=384 (%s)\n", r->embedder_dim,
          r->embedder_dim_matches ? "ok" : "MISMATCH");

   if (do_detail && r->ops_summary.failed_ops > 0)
   {
      printf("\n  Top 20 failed index ops:\n");
      for (int i = 0; i < r->failed_detail_count; i++)
      {
         printf("    point=%lld coll=%s mem=%lld attempts=%d when=%s err=%s\n",
                (long long)r->failed_detail[i].point_id, r->failed_detail[i].collection,
                (long long)r->failed_detail[i].memory_id, r->failed_detail[i].attempts,
                r->failed_detail[i].updated_at[0] ? r->failed_detail[i].updated_at : "?",
                r->failed_detail[i].last_error);
      }
   }

   /* Tail of the pgvector log — useful when the vector store is
    * unhealthy and the operator doesn't know where to look.  Only
    * under --detail because the log can be noisy. */
   if (do_detail)
   {
      char log_path[4096];
      snprintf(log_path, sizeof(log_path), "%s/pgvec.log", config_default_dir());
      FILE *lf = fopen(log_path, "r");
      if (lf)
      {
         /* Seek to at most last 2KB and print from the next newline */
         fseek(lf, 0, SEEK_END);
         long size = ftell(lf);
         long start = size > 2048 ? size - 2048 : 0;
         fseek(lf, start, SEEK_SET);
         printf("\n  Last ~%ld bytes of %s:\n", size - start, log_path);
         char line[1024];
         int first = 1;
         while (fgets(line, sizeof(line), lf))
         {
            /* Skip partial first line when seeked mid-file */
            if (first && start > 0)
            {
               first = 0;
               continue;
            }
            first = 0;
            printf("    %s", line);
         }
         if (size == 0)
            printf("    (empty)\n");
         fclose(lf);
      }
   }
}

void mem_verify(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t vopts;
   opt_parse(argc, argv, NULL, &vopts);
   int do_repair = opt_get_flag(&vopts, "repair");
   int do_detail = opt_get_flag(&vopts, "detail");
   int do_timings = opt_get_flag(&vopts, "timings");

   const char *embed_cmd = config_embedding_command(&s_mem_cfg, NULL);

   char *verify_json = kb_client_memory_verify_json(do_detail, do_timings, embed_cmd);
   cJSON *resp = verify_json ? cJSON_Parse(verify_json) : NULL;
   free(verify_json);
   cJSON *status_j = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   if (!cJSON_IsString(status_j) || strcmp(status_j->valuestring, "ok") != 0)
   {
      const char *msg = "memory verify failed";
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      char buf[256];
      snprintf(buf, sizeof(buf), "%s", msg);
      cJSON_Delete(resp);
      fatal("%s", buf);
   }

   cJSON *sv_j = cJSON_GetObjectItemCaseSensitive(resp, "server_version");
   const char *server_version =
       (cJSON_IsString(sv_j) && sv_j->valuestring) ? sv_j->valuestring : "";
   cJSON *av_j = cJSON_GetObjectItemCaseSensitive(resp, "active_embedder_version");
   const char *active_ver = (cJSON_IsString(av_j) && av_j->valuestring) ? av_j->valuestring : "";

   cJSON *emb_obj = cJSON_GetObjectItemCaseSensitive(resp, "embedder");
   int embedder_dim = 0;
   if (emb_obj)
   {
      cJSON *d = cJSON_GetObjectItemCaseSensitive(emb_obj, "dim");
      if (cJSON_IsNumber(d))
         embedder_dim = (int)d->valuedouble;
   }
   int embedder_dim_matches = (embedder_dim == 384);

   cJSON *mem_obj = cJSON_GetObjectItemCaseSensitive(resp, "memory");
   cJSON *kb_obj = cJSON_GetObjectItemCaseSensitive(resp, "kb");
   const char *mem_coll = "";
   const char *kb_coll = "";
   int mem_exists = 0, kb_exists = 0;
   int64_t mem_points = 0, kb_points = 0;
   int64_t mem_rows = 0, unit_rows = 0, kb_rows = 0;
   const char *mem_indexed = NULL, *kb_indexed = NULL;
   if (mem_obj)
   {
      cJSON *v = cJSON_GetObjectItemCaseSensitive(mem_obj, "collection");
      if (cJSON_IsString(v))
         mem_coll = v->valuestring;
      v = cJSON_GetObjectItemCaseSensitive(mem_obj, "collection_exists");
      mem_exists = cJSON_IsTrue(v) ? 1 : 0;
      v = cJSON_GetObjectItemCaseSensitive(mem_obj, "db2_memories");
      if (cJSON_IsNumber(v))
         mem_rows = (int64_t)v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(mem_obj, "db2_units");
      if (cJSON_IsNumber(v))
         unit_rows = (int64_t)v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(mem_obj, "vector_points");
      if (cJSON_IsNumber(v))
         mem_points = (int64_t)v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(mem_obj, "indexed_fields");
      if (cJSON_IsString(v))
         mem_indexed = v->valuestring;
   }
   if (kb_obj)
   {
      cJSON *v = cJSON_GetObjectItemCaseSensitive(kb_obj, "collection");
      if (cJSON_IsString(v))
         kb_coll = v->valuestring;
      v = cJSON_GetObjectItemCaseSensitive(kb_obj, "collection_exists");
      kb_exists = cJSON_IsTrue(v) ? 1 : 0;
      v = cJSON_GetObjectItemCaseSensitive(kb_obj, "db2_chunks");
      if (cJSON_IsNumber(v))
         kb_rows = (int64_t)v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(kb_obj, "vector_points");
      if (cJSON_IsNumber(v))
         kb_points = (int64_t)v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(kb_obj, "indexed_fields");
      if (cJSON_IsString(v))
         kb_indexed = v->valuestring;
   }

   mem_pgvec_ops_summary_t ops_summary;
   memset(&ops_summary, 0, sizeof(ops_summary));
   cJSON *ops_obj = cJSON_GetObjectItemCaseSensitive(resp, "index_ops");
   if (ops_obj)
   {
      cJSON *v = cJSON_GetObjectItemCaseSensitive(ops_obj, "ok");
      if (cJSON_IsNumber(v))
         ops_summary.ok_ops = (int64_t)v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(ops_obj, "pending");
      if (cJSON_IsNumber(v))
         ops_summary.pending_ops = (int64_t)v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(ops_obj, "failed");
      if (cJSON_IsNumber(v))
         ops_summary.failed_ops = (int64_t)v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(ops_obj, "stuck");
      if (cJSON_IsNumber(v))
         ops_summary.stuck_ops = (int64_t)v->valuedouble;
   }

   mem_pgvec_failed_op_t failed_detail[20];
   int failed_detail_count = 0;
   cJSON *fails = cJSON_GetObjectItemCaseSensitive(resp, "failed_ops");
   if (cJSON_IsArray(fails))
   {
      int n = cJSON_GetArraySize(fails);
      if (n > 20)
         n = 20;
      for (int i = 0; i < n; i++)
      {
         cJSON *row = cJSON_GetArrayItem(fails, i);
         if (!row)
            continue;
         mem_pgvec_failed_op_t *out = &failed_detail[failed_detail_count++];
         memset(out, 0, sizeof(*out));
         cJSON *v = cJSON_GetObjectItemCaseSensitive(row, "point_id");
         if (cJSON_IsNumber(v))
            out->point_id = (int64_t)v->valuedouble;
         v = cJSON_GetObjectItemCaseSensitive(row, "collection");
         if (cJSON_IsString(v))
            snprintf(out->collection, sizeof(out->collection), "%s", v->valuestring);
         v = cJSON_GetObjectItemCaseSensitive(row, "memory_id");
         if (cJSON_IsNumber(v))
            out->memory_id = (int64_t)v->valuedouble;
         v = cJSON_GetObjectItemCaseSensitive(row, "attempts");
         if (cJSON_IsNumber(v))
            out->attempts = (int)v->valuedouble;
         v = cJSON_GetObjectItemCaseSensitive(row, "last_error");
         if (cJSON_IsString(v))
            snprintf(out->last_error, sizeof(out->last_error), "%s", v->valuestring);
         v = cJSON_GetObjectItemCaseSensitive(row, "updated_at");
         if (cJSON_IsString(v))
            snprintf(out->updated_at, sizeof(out->updated_at), "%s", v->valuestring);
      }
   }

   int timings_trials = 0;
   int64_t timings_total_us = 0;
   int64_t timings_max_us = 0;
   cJSON *tim_obj = cJSON_GetObjectItemCaseSensitive(resp, "timings");
   if (tim_obj)
   {
      cJSON *v = cJSON_GetObjectItemCaseSensitive(tim_obj, "trials");
      if (cJSON_IsNumber(v))
         timings_trials = (int)v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(tim_obj, "total_us");
      if (cJSON_IsNumber(v))
         timings_total_us = (int64_t)v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(tim_obj, "max_us");
      if (cJSON_IsNumber(v))
         timings_max_us = (int64_t)v->valuedouble;
   }

   /* Schema version + rebuild-lock state arrive in the verify RPC payload —
    * they live in DB2 (aimee-kb's store), not DB1.  An empty `stored_ver`
    * means no rebuild has stamped a version yet.  On a fresh install that's
    * benign; when the corpus is populated but `stored_ver` is empty the
    * version is unknown, which IS a mismatch. */
   char stored_ver[64] = "";
   const char *expected_ver = pgvec_schema_version();
   cJSON *sch_obj = cJSON_GetObjectItemCaseSensitive(resp, "schema");
   if (sch_obj)
   {
      cJSON *v = cJSON_GetObjectItemCaseSensitive(sch_obj, "stored");
      if (cJSON_IsString(v) && v->valuestring)
         snprintf(stored_ver, sizeof(stored_ver), "%s", v->valuestring);
      v = cJSON_GetObjectItemCaseSensitive(sch_obj, "expected");
      if (cJSON_IsString(v) && v->valuestring && v->valuestring[0])
         expected_ver = v->valuestring;
   }
   int has_corpus = (mem_rows > 0) || (unit_rows > 0) || (kb_rows > 0);
   int schema_match = stored_ver[0] ? (strcmp(stored_ver, expected_ver) == 0) : !has_corpus;

   cJSON *lock_j = cJSON_GetObjectItemCaseSensitive(resp, "rebuild_lock_held");
   int lock_held = cJSON_IsTrue(lock_j) ? 1 : 0;

   /* Payload indexes arrived with the verify RPC payload — use them straight. */
   const char *req_mem[] = {"record_type", "primary_scope", "project",    "workspace", "tier",
                            "memory_id",   "created_at",    "updated_at", NULL};
   const char *req_kb[] = {"record_type", "project", "file_path", "document_id", NULL};
   int mem_missing = 0, kb_missing = 0;
   for (int i = 0; req_mem[i]; i++)
      if (!mem_indexed || !strstr(mem_indexed, req_mem[i]))
         mem_missing++;
   for (int i = 0; req_kb[i]; i++)
      if (!kb_indexed || !strstr(kb_indexed, req_kb[i]))
         kb_missing++;

   /* Expected memory-collection points ≈ memory rows + unit rows (one point each). */
   int64_t expected_mem = mem_rows + unit_rows;
   int64_t drift_mem = mem_points >= 0 ? (mem_points - expected_mem) : 0;
   int64_t drift_kb = kb_points >= 0 ? (kb_points - kb_rows) : 0;

   /* overall_ok: true when every check passes.  CI pipelines and
    * automation can gate on this single boolean rather than
    * reconstructing it from the sub-fields. */
   int overall_ok = (mem_exists > 0) && (kb_exists > 0) && (drift_mem == 0) && (drift_kb == 0) &&
                    (ops_summary.failed_ops == 0) && (ops_summary.stuck_ops == 0) && schema_match &&
                    embedder_dim_matches;

   mem_verify_report_t r;
   memset(&r, 0, sizeof(r));
   r.server_version = server_version;
   r.embedder_dim = embedder_dim;
   r.embedder_dim_matches = embedder_dim_matches;
   r.mem_coll = mem_coll;
   r.kb_coll = kb_coll;
   r.mem_exists = mem_exists;
   r.kb_exists = kb_exists;
   r.mem_points = mem_points;
   r.kb_points = kb_points;
   r.mem_rows = mem_rows;
   r.unit_rows = unit_rows;
   r.kb_rows = kb_rows;
   r.mem_indexed = mem_indexed;
   r.kb_indexed = kb_indexed;
   r.ops_summary = ops_summary;
   memcpy(r.failed_detail, failed_detail, sizeof(failed_detail));
   r.failed_detail_count = failed_detail_count;
   r.timings_trials = timings_trials;
   r.timings_total_us = timings_total_us;
   r.timings_max_us = timings_max_us;
   snprintf(r.stored_ver, sizeof(r.stored_ver), "%s", stored_ver);
   r.expected_ver = expected_ver;
   r.schema_match = schema_match;
   r.lock_held = lock_held;
   r.mem_missing = mem_missing;
   r.kb_missing = kb_missing;
   r.expected_mem = expected_mem;
   r.drift_mem = drift_mem;
   r.drift_kb = drift_kb;
   r.overall_ok = overall_ok;

   if (ctx->json_output)
      mem_verify_emit_json(&r, ctx, do_detail, do_timings);
   else
      mem_verify_emit_text(&r, ctx, do_detail);

   /* Print timings into text output (already computed above). */
   if (do_timings && !ctx->json_output && timings_trials > 0)
   {
      printf("  timings (%d probe queries): avg=%.2fms max=%.2fms\n", timings_trials,
             (double)timings_total_us / timings_trials / 1000.0, (double)timings_max_us / 1000.0);
   }

   /* Exit code 2 when verify found problems — CI scripts, pre-commit
    * hooks, and monitoring pipelines can gate on this without parsing
    * output.  Skipped when --repair is active so the repair path can
    * complete before signaling overall status. */
   if (!overall_ok && !do_repair)
   {
      cJSON_Delete(resp);
      exit(2);
   }

   if (do_repair)
      mem_verify_run_repair(ctx, embed_cmd, ops_summary, schema_match, drift_mem, lock_held,
                            active_ver);

   cJSON_Delete(resp);
}

/* aimee memory profile [--refresh] [--min-obs N] [<entity>]
 *
 * Without --refresh: show the stored profile card for <entity>, or build
 * one on the fly if <entity> has enough observations.
 * With --refresh: refresh all stale profile cards.
 */
void mem_profile(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   int do_refresh = opt_get_flag(&opts, "refresh");
   int min_obs = opt_get_int(&opts, "min-obs", s_mem_cfg.memory_profile_cards_min_obs);
   if (min_obs <= 0)
      min_obs = 10;

   if (do_refresh)
   {
      int stale_secs = s_mem_cfg.memory_profile_cards_stale_secs;
      if (stale_secs <= 0)
         stale_secs = 86400;
      int refreshed = memory_profile_card_refresh(min_obs, stale_secs);
      if (ctx->json_output)
      {
         cJSON *j = cJSON_CreateObject();
         cJSON_AddNumberToObject(j, "refreshed", refreshed);
         emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("Refreshed %d profile card(s)\n", refreshed);
      }
      return;
   }

   const char *entity = opt_pos(&opts, 0);
   if (!entity || !entity[0])
      fatal("memory profile requires an entity name (or --refresh)");

   char card_json[8192];
   int rc = memory_profile_card_get(entity, card_json, sizeof(card_json));
   if (rc != 0)
   {
      /* Not found: try building on the fly */
      rc = memory_profile_card_build(entity, 1 /* on-demand: any obs */, card_json,
                                     sizeof(card_json));
      if (rc != 0)
         fatal("No profile card found for '%s' — entity may have no observations", entity);
   }

   if (ctx->json_output)
   {
      cJSON *j = cJSON_Parse(card_json);
      if (!j)
         j = cJSON_CreateObject();
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      return;
   }

   /* Parse and display */
   cJSON *card = cJSON_Parse(card_json);
   if (!card)
   {
      printf("%s\n", card_json);
      return;
   }

   cJSON *name = cJSON_GetObjectItemCaseSensitive(card, "canonical_name");
   cJSON *obs = cJSON_GetObjectItemCaseSensitive(card, "observation_count");
   printf("Profile: %s\n", name && cJSON_IsString(name) ? name->valuestring : entity);
   if (obs && cJSON_IsNumber(obs))
      printf("  Observations: %d\n", (int)obs->valuedouble);

   cJSON *topics = cJSON_GetObjectItemCaseSensitive(card, "recurring_topics");
   if (topics && cJSON_IsArray(topics) && cJSON_GetArraySize(topics) > 0)
   {
      printf("  Recurring topics:");
      cJSON *t;
      cJSON_ArrayForEach(t, topics)
      {
         if (cJSON_IsString(t))
            printf(" %s", t->valuestring);
      }
      printf("\n");
   }

   cJSON *rels = cJSON_GetObjectItemCaseSensitive(card, "relationships");
   if (rels && cJSON_IsArray(rels) && cJSON_GetArraySize(rels) > 0)
   {
      printf("  Relationships:");
      cJSON *r;
      cJSON_ArrayForEach(r, rels)
      {
         cJSON *with = cJSON_GetObjectItemCaseSensitive(r, "with");
         if (with && cJSON_IsString(with))
            printf(" %s", with->valuestring);
      }
      printf("\n");
   }

   cJSON_Delete(card);
}

/* --- mem_improve: run dedupe / summarise / feedback-reweight passes --- */

void mem_improve(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);

   int do_dedupe = opt_get_flag(&opts, "dedupe");
   int do_summarise = opt_get_flag(&opts, "summarise");
   int do_all = opt_get_flag(&opts, "all");
   int dry_run = opt_get_flag(&opts, "dry-run");

   if (!do_dedupe && !do_summarise && !do_all)
      do_all = 1; /* default: run all enabled passes */

   if (do_all)
   {
      do_dedupe = 1;
      do_summarise = 1;
   }

   int dedupe_merged = 0, summaries_created = 0;

   if (do_dedupe)
   {
      dedupe_merged = memory_improve_dedupe(dry_run);
      if (dedupe_merged < 0)
      {
         fprintf(stderr, "error: dedupe pass failed\n");
         dedupe_merged = 0;
      }
   }

   if (do_summarise)
   {
      summaries_created = memory_improve_summarise(dry_run, 3, 0.5);
      if (summaries_created < 0)
      {
         fprintf(stderr, "error: summarise pass failed\n");
         summaries_created = 0;
      }
   }

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddBoolToObject(obj, "dry_run", dry_run);
      cJSON_AddNumberToObject(obj, "dedupe_merged", dedupe_merged);
      cJSON_AddNumberToObject(obj, "summaries_created", summaries_created);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (dry_run)
         printf("[dry-run] ");
      printf("dedupe: %d merged, summarise: %d created\n", dedupe_merged, summaries_created);
   }
}

/* --- mem_cognify: run LLM-driven cognification on a memory unit --- */

void mem_cognify(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);

   /* Require --unit <id> */
   const char *unit_str = opt_get(&opts, "unit");
   if (!unit_str || !unit_str[0])
   {
      fprintf(stderr, "Usage: aimee memory cognify --unit <memory_id>\n");
      return;
   }
   int64_t unit_id = (int64_t)strtoll(unit_str, NULL, 10);
   if (unit_id <= 0)
   {
      fprintf(stderr, "error: invalid --unit id '%s'\n", unit_str);
      return;
   }

   /* Load the memory */
   memory_t m;
   if (memory_get(unit_id, &m) != 0)
   {
      fprintf(stderr, "error: memory %lld not found\n", (long long)unit_id);
      return;
   }

   /* Load config */
   config_t cfg;
   config_load(&cfg);
   if (!cfg.memory_cognify_enabled)
   {
      fprintf(stderr,
              "error: cognification disabled (set memory.cognify.enabled=true in config)\n");
      return;
   }

   memory_cognify_result_t result;
   int rc = memory_cognify_unit(unit_id, m.content, &cfg, &result);
   if (rc != 0)
   {
      fprintf(stderr, "error: cognification failed for unit %lld\n", (long long)unit_id);
      return;
   }

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "unit_id", (double)unit_id);
      cJSON_AddStringToObject(obj, "summary", result.summary);
      cJSON_AddStringToObject(obj, "memory_kind", result.memory_kind);
      cJSON *rels = cJSON_AddArrayToObject(obj, "relations");
      for (int i = 0; i < result.relation_count; i++)
      {
         cJSON *r = cJSON_CreateObject();
         cJSON_AddStringToObject(r, "subject", result.relations[i].src_entity);
         cJSON_AddStringToObject(r, "relation", result.relations[i].relation);
         cJSON_AddStringToObject(r, "object", result.relations[i].dst_entity);
         cJSON_AddStringToObject(r, "fact_text", result.relations[i].fact_text);
         cJSON_AddItemToArray(rels, r);
      }
      cJSON *clms = cJSON_AddArrayToObject(obj, "claims");
      for (int i = 0; i < result.claim_count; i++)
      {
         cJSON *c = cJSON_CreateObject();
         cJSON_AddStringToObject(c, "subject", result.claims[i].subject);
         cJSON_AddStringToObject(c, "attribute", result.claims[i].attribute);
         cJSON_AddStringToObject(c, "value", result.claims[i].value);
         cJSON_AddStringToObject(c, "kind", result.claims[i].kind);
         cJSON_AddItemToArray(clms, c);
      }
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Cognified unit %lld\n", (long long)unit_id);
      if (result.summary[0])
         printf("Summary: %s\n", result.summary);
      if (result.relation_count > 0)
      {
         printf("Relations (%d):\n", result.relation_count);
         for (int i = 0; i < result.relation_count; i++)
            printf("  [%s -[%s]-> %s] %s\n", result.relations[i].src_entity,
                   result.relations[i].relation, result.relations[i].dst_entity,
                   result.relations[i].fact_text);
      }
      if (result.claim_count > 0)
      {
         printf("Claims (%d):\n", result.claim_count);
         for (int i = 0; i < result.claim_count; i++)
            printf("  [%s] %s:%s = %s\n", result.claims[i].kind, result.claims[i].subject,
                   result.claims[i].attribute, result.claims[i].value);
      }
   }
}

/* --- mem_episode: view or generate episode cards for a session --- */

void mem_episode(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);

   const char *session_id = opt_pos(&opts, 0);
   int do_generate = opt_get_flag(&opts, "generate");

   if (!session_id || !session_id[0])
      fatal("usage: aimee memory episode <session_id> [--generate]");

   if (do_generate)
   {
      config_t cfg;
      config_load(&cfg);
      if (!cfg.memory_episode_summaries_enabled)
      {
         fprintf(stderr, "error: episode summaries disabled"
                         " (set memory.episode_summaries.enabled=true in config)\n");
         return;
      }
      int64_t uid = memory_episode_card_generate(session_id, &cfg);
      if (uid <= 0)
      {
         fprintf(stderr, "error: episode card generation failed for session '%s'\n", session_id);
         return;
      }
      if (ctx->json_output)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddNumberToObject(obj, "unit_id", (double)uid);
         cJSON_AddStringToObject(obj, "session_id", session_id);
         emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("Episode card generated (unit_id=%lld) for session '%s'\n", (long long)uid,
                session_id);
      }
      return;
   }

   /* Show existing episode cards */
   char *cards[16];
   int n = memory_episode_cards_query(session_id, cards, 16);
   if (n == 0)
   {
      if (ctx->json_output)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "session_id", session_id);
         cJSON *arr = cJSON_AddArrayToObject(obj, "cards");
         (void)arr;
         emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("No episode cards found for session '%s'\n", session_id);
      }
      return;
   }

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "session_id", session_id);
      cJSON *arr = cJSON_AddArrayToObject(obj, "cards");
      for (int i = 0; i < n; i++)
      {
         cJSON_AddItemToArray(arr, cJSON_CreateString(cards[i]));
         free(cards[i]);
      }
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      for (int i = 0; i < n; i++)
      {
         printf("--- Episode Card %d ---\n%s\n\n", i + 1, cards[i]);
         free(cards[i]);
      }
   }
}

/* --- mem_assemble: show assembled context (optionally with --explain) --- */

void mem_assemble(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);

   int explain = opt_get_flag(&opts, "explain");

   /* Collect optional task_hint from positional args */
   char task_hint[1024] = "";
   int thpos = 0;
   for (int i = 0; i < opts.pos_count; i++)
   {
      /* snprintf returns the would-be length; a long positional arg can run
       * thpos past the buffer, after which sizeof(task_hint) - thpos wraps to a
       * huge size_t and the next write is out of bounds. Stop when full. */
      if (thpos >= (int)sizeof(task_hint) - 1)
         break;
      if (i > 0)
         thpos += snprintf(task_hint + thpos, sizeof(task_hint) - thpos, " ");
      thpos += snprintf(task_hint + thpos, sizeof(task_hint) - thpos, "%s", opts.positional[i]);
   }

   if (explain)
   {
#define EXPLAIN_MAX 128
      context_assemble_explain_entry_t entries[EXPLAIN_MAX];
      int ecount = 0;
      context_budget_metrics_t metrics;
      memset(&metrics, 0, sizeof(metrics));

      char *ctx_text = memory_assemble_context_explain(task_hint[0] ? task_hint : NULL, entries,
                                                       &ecount, EXPLAIN_MAX, &metrics);

      if (ctx->json_output)
      {
         cJSON *obj = cJSON_CreateObject();
         if (task_hint[0])
            cJSON_AddStringToObject(obj, "task_hint", task_hint);
         cJSON *budget = cJSON_AddObjectToObject(obj, "budget");
         cJSON_AddNumberToObject(budget, "budget_tokens", metrics.budget_tokens);
         cJSON_AddNumberToObject(budget, "used_tokens", metrics.used_tokens);
         cJSON_AddNumberToObject(budget, "rejected_for_budget", metrics.rejected_for_budget);
         cJSON *arr = cJSON_AddArrayToObject(obj, "candidates");
         for (int i = 0; i < ecount; i++)
         {
            context_assemble_explain_entry_t *e = &entries[i];
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", (double)e->id);
            cJSON_AddStringToObject(item, "tier", e->tier);
            cJSON_AddStringToObject(item, "kind", e->kind);
            cJSON_AddStringToObject(item, "key", e->key);
            cJSON_AddStringToObject(item, "scope", e->scope);
            cJSON_AddNumberToObject(item, "score", e->score);
            cJSON_AddNumberToObject(item, "tokens", e->tokens);
            cJSON_AddNumberToObject(item, "score_per_token", e->score_per_token);
            cJSON_AddBoolToObject(item, "selected", e->selected);
            if (!e->selected)
               cJSON_AddStringToObject(item, "rejection_reason", e->rejection_reason);
            cJSON_AddItemToArray(arr, item);
         }
         emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("## Context Assembly Explain\n");
         if (metrics.budget_tokens > 0)
            printf("budget: %d tokens | used: %d | rejected_for_budget: %d\n\n",
                   metrics.budget_tokens, metrics.used_tokens, metrics.rejected_for_budget);
         printf("%-8s %-4s %-12s %-6s %-10s %-8s %-8s %-6s\n", "ID", "Tier", "Kind", "Tok",
                "Score/Tok", "Score", "Scope", "Status");
         printf("%s\n", "------------------------------------------------------------------------");
         for (int i = 0; i < ecount; i++)
         {
            context_assemble_explain_entry_t *e = &entries[i];
            printf("%-8lld %-4s %-12s %-6d %-10.4f %-8.4f %-8s %s  %s\n", (long long)e->id, e->tier,
                   e->kind, e->tokens, e->score_per_token, e->score, e->scope[0] ? e->scope : "-",
                   e->selected ? "SELECTED" : "rejected", e->selected ? "" : e->rejection_reason);
         }
         printf("\n--- Assembled Context ---\n%s\n", ctx_text ? ctx_text : "");
      }
      free(ctx_text);
#undef EXPLAIN_MAX
   }
   else
   {
      char *ctx_text = memory_assemble_context(task_hint[0] ? task_hint : NULL);
      if (!ctx_text)
         ctx_text = safe_strdup("");

      if (ctx->json_output)
      {
         cJSON *obj = cJSON_CreateObject();
         if (task_hint[0])
            cJSON_AddStringToObject(obj, "task_hint", task_hint);
         cJSON_AddStringToObject(obj, "context", ctx_text);
         emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("%s\n", ctx_text);
      }
      free(ctx_text);
   }
}

/* --- cite: show provenance + lineage chain for a memory ID --- */

void mem_cite(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory cite requires a memory ID");

   int64_t memory_id = (int64_t)strtoll(argv[0], NULL, 10);
   if (memory_id <= 0)
      fatal("memory cite: invalid memory ID '%s'", argv[0]);

   memory_cite(memory_id, ctx->json_output);
}

/* --- scene: list or show memory scenes --- */

/* Parse a kb_client_memory_* JSON envelope; fatal on non-ok.  Returns
 * the heap-owned cJSON* (caller frees). */
static cJSON *scene_rpc_unwrap(char *resp_json, const char *what)
{
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);
   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      const char *msg = what;
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      char buf[256];
      snprintf(buf, sizeof(buf), "%s", msg);
      cJSON_Delete(resp);
      fatal("%s", buf);
   }
   return resp;
}

void mem_scene(app_ctx_t *ctx, int argc, char **argv)
{
   const char *sub = argc > 0 ? argv[0] : "list";

   if (strcmp(sub, "list") == 0)
   {
      cJSON *resp =
          scene_rpc_unwrap(kb_client_memory_scene_list_json(), "memory scene list failed");
      cJSON *arr_src = cJSON_GetObjectItemCaseSensitive(resp, "scenes");
      int n = cJSON_IsArray(arr_src) ? cJSON_GetArraySize(arr_src) : 0;

      if (ctx->json_output)
      {
         cJSON *root = cJSON_CreateObject();
         cJSON *out = cJSON_AddArrayToObject(root, "scenes");
         for (int i = 0; i < n; i++)
            cJSON_AddItemToArray(out, cJSON_Duplicate(cJSON_GetArrayItem(arr_src, i), 1));
         emit_json_ctx(root, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("%-6s  %-20s  %-10s  %s\n", "ID", "workspace", "turns", "created_at");
         for (int i = 0; i < n; i++)
         {
            cJSON *row = cJSON_GetArrayItem(arr_src, i);
            cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
            cJSON *ws = cJSON_GetObjectItemCaseSensitive(row, "workspace_id");
            cJSON *tc = cJSON_GetObjectItemCaseSensitive(row, "turn_count");
            cJSON *ca = cJSON_GetObjectItemCaseSensitive(row, "created_at");
            printf("%-6lld  %-20s  %-10d  %s\n",
                   cJSON_IsNumber(id) ? (long long)id->valuedouble : 0LL,
                   cJSON_IsString(ws) ? ws->valuestring : "",
                   cJSON_IsNumber(tc) ? (int)tc->valuedouble : 0,
                   cJSON_IsString(ca) ? ca->valuestring : "");
         }
      }
      cJSON_Delete(resp);
      return;
   }

   if (strcmp(sub, "show") == 0)
   {
      if (argc < 2)
         fatal("memory scene show requires a scene ID");
      int64_t scene_id = (int64_t)strtoll(argv[1], NULL, 10);
      if (scene_id <= 0)
         fatal("memory scene show: invalid scene ID '%s'", argv[1]);

      cJSON *resp =
          scene_rpc_unwrap(kb_client_memory_scene_show_json(scene_id), "memory scene show failed");
      cJSON *arr_src = cJSON_GetObjectItemCaseSensitive(resp, "members");
      int n = cJSON_IsArray(arr_src) ? cJSON_GetArraySize(arr_src) : 0;

      if (ctx->json_output)
      {
         cJSON *root = cJSON_CreateObject();
         cJSON_AddNumberToObject(root, "scene_id", (double)scene_id);
         cJSON *out = cJSON_AddArrayToObject(root, "members");
         for (int i = 0; i < n; i++)
            cJSON_AddItemToArray(out, cJSON_Duplicate(cJSON_GetArrayItem(arr_src, i), 1));
         emit_json_ctx(root, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("Scene %lld members:\n", (long long)scene_id);
         printf("%-8s  %-50s  %s\n", "mem_id", "key", "strength");
         for (int i = 0; i < n; i++)
         {
            cJSON *row = cJSON_GetArrayItem(arr_src, i);
            cJSON *mid = cJSON_GetObjectItemCaseSensitive(row, "memory_id");
            cJSON *key = cJSON_GetObjectItemCaseSensitive(row, "key");
            cJSON *str = cJSON_GetObjectItemCaseSensitive(row, "membership_strength");
            printf("%-8lld  %-50s  %.4f\n", cJSON_IsNumber(mid) ? (long long)mid->valuedouble : 0LL,
                   cJSON_IsString(key) ? key->valuestring : "",
                   cJSON_IsNumber(str) ? str->valuedouble : 0.0);
         }
      }
      cJSON_Delete(resp);
      return;
   }

   fprintf(stderr, "memory scene: unknown subcommand '%s' (list, show <id>)\n", sub);
}

/* --- ontology: dump schema, or walk typed graph --- */

void mem_ontology(app_ctx_t *ctx, int argc, char **argv)
{
   const char *sub = argc > 0 ? argv[0] : "list";

   if (strcmp(sub, "list") == 0)
   {
      /* Dump node kinds and relation kinds */
      if (ctx->json_output)
      {
         cJSON *root = cJSON_CreateObject();

         cJSON *nodes = cJSON_AddArrayToObject(root, "node_kinds");
         for (int i = 0; i <= (int)NODE_OTHER; i++)
         {
            const char *label = memory_ontology_node_kind_to_text((memory_node_kind_t)i);
            if (strcmp(label, "other") == 0 && i != (int)NODE_OTHER)
               continue;
            cJSON *n = cJSON_CreateObject();
            cJSON_AddNumberToObject(n, "id", i);
            cJSON_AddStringToObject(n, "label", label);
            cJSON_AddItemToArray(nodes, n);
         }

         cJSON *rels = cJSON_AddArrayToObject(root, "relation_kinds");
         for (int i = 0; i <= (int)REL_OTHER; i++)
         {
            const char *label = memory_ontology_relation_to_text((memory_relation_kind_t)i);
            if (strcmp(label, "other") == 0 && i != (int)REL_OTHER)
               continue;
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "id", i);
            cJSON_AddStringToObject(r, "label", label);
            cJSON_AddItemToArray(rels, r);
         }

         /* Also dump the DB schema table if present (via aimee-kb). */
         db2_relation_schema_row_t schema_rows[256];
         int n_schema = kb_client_relations_schema_list(
             schema_rows, (int)(sizeof(schema_rows) / sizeof(schema_rows[0])));
         if (n_schema > 0)
         {
            cJSON *rules = cJSON_AddArrayToObject(root, "schema_rules");
            for (int i = 0; i < n_schema; i++)
            {
               cJSON *rule = cJSON_CreateObject();
               int rid = schema_rows[i].relation_id;
               int sk = schema_rows[i].subject_kind;
               int ok = schema_rows[i].object_kind;
               cJSON_AddStringToObject(
                   rule, "relation", memory_ontology_relation_to_text((memory_relation_kind_t)rid));
               cJSON_AddStringToObject(
                   rule, "subject_kind",
                   sk == 99 ? "any" : memory_ontology_node_kind_to_text((memory_node_kind_t)sk));
               cJSON_AddStringToObject(
                   rule, "object_kind",
                   ok == 99 ? "any" : memory_ontology_node_kind_to_text((memory_node_kind_t)ok));
               cJSON_AddItemToArray(rules, rule);
            }
         }

         emit_json_ctx(root, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("Node kinds:\n");
         for (int i = 0; i <= (int)NODE_OTHER; i++)
         {
            const char *label = memory_ontology_node_kind_to_text((memory_node_kind_t)i);
            if (strcmp(label, "other") == 0 && i != (int)NODE_OTHER)
               continue;
            printf("  %3d  %s\n", i, label);
         }
         printf("\nRelation kinds:\n");
         for (int i = 0; i <= (int)REL_OTHER; i++)
         {
            const char *label = memory_ontology_relation_to_text((memory_relation_kind_t)i);
            if (strcmp(label, "other") == 0 && i != (int)REL_OTHER)
               continue;
            printf("  %3d  %s\n", i, label);
         }
      }
   }
   else if (strcmp(sub, "walk") == 0)
   {
      /* Walk typed graph from a seed entity */
      if (argc < 2)
      {
         fprintf(stderr, "memory ontology walk <entity> [--hops N] [--rel relation,...]\n");
         return;
      }
      const char *seed = argv[1];
      int max_hops = 2;
      unsigned int mask = RELATION_MASK_ALL;

      for (int i = 2; i < argc; i++)
      {
         if (strcmp(argv[i], "--hops") == 0 && i + 1 < argc)
            max_hops = atoi(argv[++i]);
         else if (strcmp(argv[i], "--rel") == 0 && i + 1 < argc)
         {
            /* Comma-separated relation names */
            mask = 0;
            char rel_copy[512];
            snprintf(rel_copy, sizeof(rel_copy), "%s", argv[++i]);
            char *sv = NULL;
            char *tok = strtok_r(rel_copy, ",", &sv);
            while (tok)
            {
               memory_relation_kind_t r = memory_ontology_relation_from_text(tok);
               if ((int)r < 32)
                  mask |= RELATION_MASK(r);
               tok = strtok_r(NULL, ",", &sv);
            }
         }
      }

#define WALK_MAX 128
      graph_walk_entry_t entries[WALK_MAX];
      int count = memory_graph_walk(seed, mask, max_hops, entries, WALK_MAX);

      if (ctx->json_output)
      {
         cJSON *arr = cJSON_CreateArray();
         for (int i = 0; i < count; i++)
         {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "hop", entries[i].hop);
            cJSON_AddStringToObject(e, "source", entries[i].source);
            cJSON_AddStringToObject(e, "relation", entries[i].relation);
            cJSON_AddStringToObject(e, "target", entries[i].target);
            cJSON_AddStringToObject(
                e, "relation_kind",
                memory_ontology_relation_to_text((memory_relation_kind_t)entries[i].relation_id));
            cJSON_AddStringToObject(
                e, "subject_kind",
                memory_ontology_node_kind_to_text((memory_node_kind_t)entries[i].subject_kind));
            cJSON_AddStringToObject(
                e, "object_kind",
                memory_ontology_node_kind_to_text((memory_node_kind_t)entries[i].object_kind));
            cJSON_AddNumberToObject(e, "weight", entries[i].weight);
            cJSON_AddItemToArray(arr, e);
         }
         emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         for (int i = 0; i < count; i++)
            printf("[hop %d] %s -[%s]-> %s (wt=%d)\n", entries[i].hop, entries[i].source,
                   entries[i].relation, entries[i].target, entries[i].weight);
      }
#undef WALK_MAX
   }
   else
   {
      fprintf(stderr, "memory ontology: unknown subcommand '%s' (list, walk <entity>)\n", sub);
   }
}
