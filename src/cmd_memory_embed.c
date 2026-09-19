#include "json_fluent.h"
#include "module_commands.h"
/* cmd_memory_embed.c: embed, reembed, diagnose, answer, reflect, audit,
 * calibrate, and benchmark subcommand handlers. Includes the
 * memory_score_parts_to_json helper (used by the search / explain paths
 * in cmd_memory_core.c too). */
#include "aimee.h"
#include "cmd_memory_internal.h"
#include "config_database.h"
#include "db1_client/db1.h"
#include "dogfood.h"
#include "modules/db2/c/memory_query.h"
#include "kb_client.h"
#include "platform_process.h"
#include "kb.h"
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

/* Parse a JSON envelope returned by kb_client_memory_*; fatal on non-ok
 * status.  Caller takes ownership of the returned cJSON*. */
static cJSON *mem_rpc_unwrap(char *resp_json, const char *what)
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

void mem_embed(app_ctx_t *ctx, int argc, char **argv)
{
   int all = 0;
   int64_t single_id = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--all") == 0)
         all = 1;
      else
         single_id = atoll(argv[i]);
   }

   if (!all && single_id <= 0)
      fatal("usage: aimee memory embed --all  OR  aimee memory embed <id>");

   if (single_id > 0)
   {
      cJSON *request = cJSON_CreateObject();
      cJSON_AddNumberToObject(request, "memory_id", (double)single_id);
      cJSON *resp =
          mem_rpc_unwrap(kb_v1_action_request_with_timeout("memory.embed", request, 10 * 60 * 1000),
                         "memory embed failed");
      cJSON_Delete(resp);
      printf("Embedded memory %lld\n", (long long)single_id);
      return;
   }

   cJSON *request = cJSON_CreateObject();
   cJSON_AddBoolToObject(request, "all", 1);
   cJSON *resp =
       mem_rpc_unwrap(kb_v1_action_request_with_timeout("memory.embed", request, 10 * 60 * 1000),
                      "memory embed failed");
   int success = 0, fail = 0;
   cJSON *n = cJSON_GetObjectItemCaseSensitive(resp, "embedded");
   if (cJSON_IsNumber(n))
      success = (int)n->valuedouble;
   n = cJSON_GetObjectItemCaseSensitive(resp, "failed");
   if (cJSON_IsNumber(n))
      fail = (int)n->valuedouble;
   cJSON_Delete(resp);

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "embedded", success);
      cJSON_AddNumberToObject(j, "failed", fail);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Embedded %d memories (%d failed)\n", success, fail);
   }
}

/* mem_reembed: online re-embed with versioned embedding rollover.
 *
 * Subcommands:
 *   --start [--version <v>]  Begin (or resume) re-embedding to a new version.
 *   --status                 Show current job progress.
 *   --cutover                Activate the completed new version as the live index.
 *   --rollback <version>     Switch back to any retained prior version.
 */
void mem_reembed(app_ctx_t *ctx, int argc, char **argv)
{
   const char *verb = NULL, *version = NULL;
   int start = 0, status = 0;
   for (int i = 0; i < argc; ++i)
   {
      if (strcmp(argv[i], "--start") == 0)
      {
         verb = "memory.reembed_start";
         start = 1;
      }
      else if (strcmp(argv[i], "--status") == 0)
      {
         verb = "memory.reembed_status";
         status = 1;
      }
      else if (strcmp(argv[i], "--cutover") == 0)
         verb = "memory.reembed_cutover";
      else if (strcmp(argv[i], "--rollback") == 0 && i + 1 < argc)
      {
         verb = "memory.reembed_rollback";
         version = argv[++i];
      }
      else if (strcmp(argv[i], "--version") == 0 && i + 1 < argc)
         version = argv[++i];
   }
   if (!verb)
      fatal("usage: aimee memory reembed --start [--version <v>] | --status | --cutover | "
            "--rollback <version>");
   start = strcmp(verb, "memory.reembed_start") == 0;
   status = strcmp(verb, "memory.reembed_status") == 0;
   cJSON *request = cJSON_CreateObject();
   char model[CONFIG_COPY_MAX];
   if (start)
   {
      const char *command = config_embedder_command_current(NULL);
      config_embedder_model_copy(model, sizeof(model));
      if (!version)
         version = model[0] ? model : command;
      cJSON_AddStringToObject(request, "embedding_command", command);
   }
   if (version)
      cJSON_AddStringToObject(request, "version", version);
   cJSON *response = mem_rpc_unwrap(
       kb_v1_action_request_with_timeout(verb, request, 10 * 60 * 1000), "re-embedding failed");
   if (ctx->json_output)
   {
      emit_json_ctx(response, ctx->json_fields, ctx->response_profile);
      return;
   }
   if (status)
   {
      const char *active = jo_cstr(response, "active_version");
      printf("Active embedder version: %s\n", active[0] ? active : "(unset)");
      cJSON *job = cJSON_GetObjectItemCaseSensitive(response, "job");
      if (cJSON_IsObject(job))
      {
         printf("Job target: %s\n", jo_cstr(job, "target_version"));
         printf("Progress: %lld / %lld (last_id=%lld)\n", (long long)jo_i64(job, "done", 0),
                (long long)jo_i64(job, "total", 0), (long long)jo_i64(job, "last_id", 0));
         printf("Started: %s\n", jo_cstr(job, "started_at"));
         printf("Ready for cutover: %s\n",
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "ready")) ? "yes" : "no");
         printf("Pending metadata jobs: %lld\n", (long long)jo_i64(job, "pending_metadata", 0));
      }
      else
         printf("No re-embed job recorded.\n");
   }
   else if (start)
   {
      printf("Version '%s': %lld embedded, %lld failed; %lld / %lld current vectors.\n",
             jo_cstr(response, "version"), (long long)jo_i64(response, "embedded", 0),
             (long long)jo_i64(response, "failed", 0), (long long)jo_i64(response, "done", 0),
             (long long)jo_i64(response, "total", 0));
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "ready")))
         printf("Run 'aimee memory reembed --cutover' to activate this version.\n");
      else
         printf("Version is not ready. Check --status and resume --start after resolving failures "
                "or pending indexing.\n");
   }
   else
      printf("Active embedder set to '%s' (%lld vector points).\n", jo_cstr(response, "version"),
             (long long)jo_i64(response, "rebuilt", 0));
   cJSON_Delete(response);
}

cJSON *memory_score_parts_to_json(const memory_score_parts_t *parts)
{
   cJSON *j = cJSON_CreateObject();
   if (!j || !parts)
      return j;
   jo_add_num(j, "lexical", parts->lexical);
   jo_add_num(j, "coverage", parts->coverage);
   jo_add_num(j, "entity", parts->entity);
   jo_add_num(j, "temporal", parts->temporal);
   jo_add_num(j, "evidence", parts->evidence);
   jo_add_num(j, "semantic", parts->semantic);
   jo_add_num(j, "state", parts->state);
   jo_add_num(j, "intent", parts->intent);
   jo_add_num(j, "salience", parts->salience);
   jo_add_num(j, "surprise", parts->surprise);
   jo_add_num(j, "pagerank", parts->pagerank);
   jo_add_num(j, "confidence", parts->confidence);
   if (parts->hybrid_total != 0.0 || parts->blended_total != 0.0)
   {
      jo_add_num(j, "hybrid_total", parts->hybrid_total);
      jo_add_num(j, "blended_total", parts->blended_total);
   }
   /* Phase 6 fusion score parts (provisional; only emitted when non-zero). */
   if (parts->graph_score != 0.0)
      jo_add_num(j, "graph_score", parts->graph_score);
   if (parts->code_proximity != 0.0)
      jo_add_num(j, "code_proximity", parts->code_proximity);
   if (parts->utility != 0.0)
      jo_add_num(j, "utility", parts->utility);
   if (parts->source_fusion != 0.0)
      jo_add_num(j, "source_fusion", parts->source_fusion);
   jo_add_num(j, "total", parts->total);
   return j;
}

/* Phase 6/7: print the graph route to a result for `memory diagnose
 * --trace-graph`.  Uses the shipped graph.explain RPC to surface the edges
 * (relation, neighbour, origin) that connect this result into the graph. */
static void cmd_memory_print_graph_trace(const char *entity_key)
{
   if (!entity_key || !entity_key[0])
      return;
   char *json = kb_client_graph_explain_json(entity_key, 8);
   if (!json)
   {
      printf("    trace-graph: (kb unreachable)\n");
      return;
   }
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return;
   cJSON *edges = cJSON_GetObjectItemCaseSensitive(resp, "edges");
   int shown = 0;
   if (cJSON_IsArray(edges))
   {
      cJSON *e;
      cJSON_ArrayForEach(e, edges)
      {
         if (shown >= 5)
            break;
         cJSON *src = cJSON_GetObjectItemCaseSensitive(e, "source");
         cJSON *rel = cJSON_GetObjectItemCaseSensitive(e, "relation");
         cJSON *tgt = cJSON_GetObjectItemCaseSensitive(e, "target");
         cJSON *org = cJSON_GetObjectItemCaseSensitive(e, "edge_origin");
         printf("    trace-graph: %s --%s--> %s [%s]\n",
                cJSON_IsString(src) ? src->valuestring : "",
                cJSON_IsString(rel) ? rel->valuestring : "",
                cJSON_IsString(tgt) ? tgt->valuestring : "",
                cJSON_IsString(org) ? org->valuestring : "");
         shown++;
      }
   }
   if (shown == 0)
      printf("    trace-graph: (no incident graph edges)\n");
   cJSON_Delete(resp);
}

void mem_diagnose(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory diagnose requires query terms");

   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   cmd_memory_apply_rerank_mode(&opts);
   int limit = opt_get_int(&opts, "limit", 5);
   int trace_graph = opt_has(&opts, "trace-graph");

   char query_buf[2048];
   int qpos = 0;
   query_buf[0] = '\0';
   for (int i = 0; i < opts.pos_count; i++)
   {
      if (i > 0)
         qpos = str_appendf(query_buf, qpos, (int)sizeof(query_buf), " ");
      qpos = str_appendf(query_buf, qpos, (int)sizeof(query_buf), "%s", opts.positional[i]);
   }
   if (!query_buf[0])
      fatal("memory diagnose requires query terms");

   memory_diagnostic_t rows[16];
   int count = cmd_memory_diagnose_query(&opts, query_buf, limit, rows, 16);
   cmd_memory_require_runtime(count, "memory diagnose");

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      jo_add_str(obj, "query", query_buf);
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *entry = cJSON_CreateObject();
         cJSON_AddItemToObject(entry, "memory", memory_to_json(&rows[i].memory));
         cJSON_AddItemToObject(entry, "score", memory_score_parts_to_json(&rows[i].parts));
         cJSON_AddItemToArray(arr, entry);
      }
      cJSON_AddItemToObject(obj, "results", arr);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("Query: %s\n", query_buf);
   for (int i = 0; i < count; i++)
   {
      printf("[%d] #%lld %s\n", i + 1, (long long)rows[i].memory.id, rows[i].memory.key);
      printf("    total=%.3f lexical=%.3f coverage=%.3f entity=%.3f temporal=%.3f evidence=%.3f "
             "semantic=%.3f state=%.3f intent=%.3f salience=%.3f surprise=%.3f pagerank=%.3f\n",
             rows[i].parts.total, rows[i].parts.lexical, rows[i].parts.coverage,
             rows[i].parts.entity, rows[i].parts.temporal, rows[i].parts.evidence,
             rows[i].parts.semantic, rows[i].parts.state, rows[i].parts.intent,
             rows[i].parts.salience, rows[i].parts.surprise, rows[i].parts.pagerank);
      if (trace_graph)
         cmd_memory_print_graph_trace(rows[i].memory.key);
   }
}

void mem_answer(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory answer requires query terms");

   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   cmd_memory_apply_rerank_mode(&opts);
   int limit = opt_get_int(&opts, "limit", 5);
   int explain = opt_get_flag(&opts, "explain");

   char query_buf[2048];
   int qpos = 0;
   query_buf[0] = '\0';
   for (int i = 0; i < opts.pos_count; i++)
   {
      if (i > 0)
         qpos = str_appendf(query_buf, qpos, (int)sizeof(query_buf), " ");
      qpos = str_appendf(query_buf, qpos, (int)sizeof(query_buf), "%s", opts.positional[i]);
   }
   if (!query_buf[0])
      fatal("memory answer requires query terms");

   memory_filter_t filter;
   cmd_memory_build_filter(&opts, &filter);

   cJSON *request = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(request);
   cJSON_AddStringToObject(request, "query", query_buf);
   cJSON_AddNumberToObject(request, "limit", limit);
   const char *scope_type = cmd_memory_scope_type(&opts);
   const char *scope_value = cmd_memory_scope_value(&opts);
   if (scope_type && scope_type[0])
      cJSON_AddStringToObject(request, "scope_type", scope_type);
   if (scope_value && scope_value[0])
      cJSON_AddStringToObject(request, "scope_value", scope_value);
   cJSON *result = mem_rpc_unwrap(kb_v1_action_request("memory.ask", request), "memory ask failed");
   cJSON *citation_ids = cJSON_GetObjectItemCaseSensitive(result, "citation_ids");
   if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(result, "answer")) ||
       !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(result, "no_answer")) ||
       !cJSON_IsArray(citation_ids))
   {
      cJSON_Delete(result);
      fatal("memory ask returned an invalid answer");
   }
   int64_t surfaced_ids[4];
   int surfaced_count = 0;
   cJSON *id;
   cJSON_ArrayForEach(id, citation_ids) if (cJSON_IsNumber(id) && surfaced_count < 4)
       surfaced_ids[surfaced_count++] = (int64_t)id->valuedouble;
   dogfood_log_moment_live("memory_ask", query_buf, surfaced_ids, surfaced_count, NULL);

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      jo_add_str(obj, "query", query_buf);
      jo_add_str(obj, "answer", jo_cstr(result, "answer"));
      cJSON_AddNumberToObject(obj, "confidence", jo_num(result, "confidence", 0));
      cJSON_AddBoolToObject(obj, "no_answer", jo_bool(result, "no_answer", 0));
      cJSON_AddBoolToObject(obj, "low_confidence", jo_bool(result, "low_confidence", 0));
      cJSON *trace = cJSON_DetachItemFromObjectCaseSensitive(result, "evidence_trace");
      if (trace)
         cJSON_AddItemToObject(obj, "evidence_trace", trace);
      cJSON *citations = cJSON_AddArrayToObject(obj, "citations");
      for (int i = 0; i < surfaced_count; i++)
      {
         cJSON *citation = cJSON_CreateObject();
         cJSON_AddNumberToObject(citation, "memory_id", (double)surfaced_ids[i]);
         cJSON_AddItemToArray(citations, citation);
      }
      if (explain)
         cJSON_AddItemToObject(obj, "filter_contract", memory_filter_to_json(&filter));
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (jo_bool(result, "no_answer", 0))
         printf("No confident answer for \"%s\"\n", query_buf);
      else
         printf("%s\n", jo_cstr(result, "answer"));
      if (explain)
      {
         cJSON *fc = memory_filter_to_json(&filter);
         char *filter_json = cJSON_Print(fc);
         cJSON_Delete(fc);
         if (filter_json)
         {
            printf("Filter: %s\n", filter_json);
            free(filter_json);
         }
      }
   }
   cJSON_Delete(result);
}

/* The shared Go owner implements reflection, synthesis and draft-rule policy. */
void mem_reflect(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("usage: aimee memory reflect <query> [--scope auto|global|workspace|project] "
            "[--scope-value VALUE] [--limit N] [--draft-rule] [--synthesize]");
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   cmd_memory_apply_rerank_mode(&opts);
   char query[2048] = "";
   size_t used = 0;
   for (int i = 0; i < opts.pos_count; i++)
   {
      size_t len = strlen(opts.positional[i]);
      if (used + len + (i > 0) >= sizeof(query))
         fatal("memory reflect query exceeds 2047 bytes");
      if (i > 0)
         query[used++] = ' ';
      memcpy(query + used, opts.positional[i], len + 1);
      used += len;
   }
   if (!query[0])
      fatal("memory reflect requires query terms");
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "query", query);
   cJSON_AddNumberToObject(request, "limit", opt_get_int(&opts, "limit", 10));
   cJSON_AddBoolToObject(request, "draft_rule", opt_get_flag(&opts, "draft-rule"));
   cJSON_AddBoolToObject(request, "synthesize", opt_get_flag(&opts, "synthesize"));
   cJSON_AddStringToObject(request, "format", ctx->json_output ? "json" : "text");
   if (ctx->json_fields)
      cJSON_AddStringToObject(request, "fields", ctx->json_fields);
   const char *scope = opt_get(&opts, "scope");
   if (scope && strcmp(scope, "auto") != 0)
   {
      const char *value = opt_get(&opts, "scope-value");
      cJSON_AddStringToObject(request, "scope_type", scope);
      cJSON_AddStringToObject(request, "scope_value", value && value[0] ? value : scope);
   }
   else
   {
      if (config_workspace_count() > 0)
         cJSON_AddStringToObject(request, "workspace", config_workspaces(0));
      cJSON_AddBoolToObject(request, "scope_context", 1);
      kb_client_memory_scope_context_apply(request);
   }
   cJSON *result =
       mem_rpc_unwrap(kb_v1_action_request_with_timeout("memory.reflect", request, 60000),
                      "memory reflect failed");
   const cJSON *output = cJSON_GetObjectItemCaseSensitive(result, "output");
   if (!cJSON_IsString(output))
   {
      cJSON_Delete(result);
      fatal("memory reflect returned malformed output");
   }
   fputs(output->valuestring, stdout);
   if (ctx->json_output)
      fputc('\n', stdout);
   cJSON_Delete(result);
}

/* Label parsing, scoring and calibration belong to the Go owner. */
static void mem_label_analysis(app_ctx_t *ctx, int argc, char **argv, const char *method)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *labels = opt_get(&opts, "labels");
   if (!labels)
      fatal("memory analysis requires --labels <query<TAB>memory_id file>");
   FILE *fp = fopen(labels, "rb");
   if (!fp)
      fatal("cannot open labels file");
   char *input = malloc(1048578);
   if (!input)
      fatal("cannot allocate labels input");
   size_t count = fread(input, 1, 1048577, fp);
   int failed = ferror(fp);
   fclose(fp);
   if (failed || count > 1048576 || memchr(input, '\0', count))
      fatal("labels must be text of at most 1 MiB");
   input[count] = '\0';
   cJSON *request = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(request);
   cJSON_AddStringToObject(request, "labels_tsv", input);
   free(input);
   cJSON_AddStringToObject(request, "labels", labels);
   cJSON_AddNumberToObject(request, "limit", opt_get_int(&opts, "limit", 10));
   cJSON_AddNumberToObject(request, "candidate_limit", opt_get_int(&opts, "candidate-limit", 24));
   cJSON_AddNumberToObject(request, "rounds", opt_get_int(&opts, "rounds", 2));
   cJSON_AddBoolToObject(request, "apply_config", opt_get_flag(&opts, "apply-config"));
   cJSON_AddStringToObject(request, "format", ctx->json_output ? "json" : "text");
   if (ctx->json_fields)
      cJSON_AddStringToObject(request, "fields", ctx->json_fields);
   if (ctx->response_profile)
      cJSON_AddStringToObject(request, "profile", ctx->response_profile);
   cJSON *result = mem_rpc_unwrap(kb_v1_action_request(method, request), "memory analysis failed");
   const cJSON *output = cJSON_GetObjectItemCaseSensitive(result, "output");
   const cJSON *artifact = cJSON_GetObjectItemCaseSensitive(result, "artifact");
   if (!cJSON_IsString(output) || !cJSON_IsString(artifact))
      fatal("memory analysis returned malformed output");
   const char *write_path = opt_get(&opts, "write");
   if (write_path && write_path[0])
   {
      fp = fopen(write_path, "wb");
      if (!fp)
         fatal("cannot open analysis artifact");
      size_t length = strlen(artifact->valuestring);
      failed = fwrite(artifact->valuestring, 1, length, fp) != length;
      failed |= fclose(fp) != 0;
      if (failed)
         fatal("cannot write analysis artifact");
   }
   fputs(output->valuestring, stdout);
   cJSON_Delete(result);
}

void mem_audit(app_ctx_t *ctx, int argc, char **argv)
{
   mem_label_analysis(ctx, argc, argv, "memory.audit");
}

void mem_calibrate(app_ctx_t *ctx, int argc, char **argv)
{
   mem_label_analysis(ctx, argc, argv, "memory.calibrate");
}

static const char *mem_benchmark_cli_weight_profile(int argc, char **argv)
{
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--weight-profile") == 0)
      {
         if (i + 1 < argc && argv[i + 1] && argv[i + 1][0])
            return argv[i + 1];
         return NULL;
      }
      if (strncmp(argv[i], "--weight-profile=", 17) == 0 && argv[i][17])
         return argv[i] + 17;
   }
   return NULL;
}

static void mem_benchmark_resolve_weight_profile(int argc, char **argv, char *active,
                                                 size_t active_len, char *previous,
                                                 size_t previous_len)
{
   if (active && active_len > 0)
      active[0] = '\0';
   if (previous && previous_len > 0)
      previous[0] = '\0';

   const char *env_profile = getenv("AIMEE_MEMORY_WEIGHT_PROFILE");
   if (previous && previous_len > 0 && env_profile && env_profile[0])
      snprintf(previous, previous_len, "%s", env_profile);

   const char *cli_profile = mem_benchmark_cli_weight_profile(argc, argv);
   if (cli_profile && cli_profile[0])
   {
      platform_setenv("AIMEE_MEMORY_WEIGHT_PROFILE", cli_profile);
      if (active && active_len > 0)
         snprintf(active, active_len, "%s", cli_profile);
      return;
   }

   if (env_profile && env_profile[0])
   {
      if (active && active_len > 0)
         snprintf(active, active_len, "%s", env_profile);
      return;
   }
   if (config_present() && config_memory_weight_profile()[0] && active && active_len > 0)
      snprintf(active, active_len, "%s", config_memory_weight_profile());
}

static void mem_benchmark_restore_weight_profile(const char *previous)
{
   platform_setenv("AIMEE_MEMORY_WEIGHT_PROFILE", (previous && previous[0]) ? previous : "");
}

void mem_benchmark(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   char benchmark_weight_profile[512];
   char previous_weight_profile[512];
   mem_benchmark_resolve_weight_profile(argc, argv, benchmark_weight_profile,
                                        sizeof(benchmark_weight_profile), previous_weight_profile,
                                        sizeof(previous_weight_profile));
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   cmd_memory_apply_rerank_mode(&opts);

#define BENCHMARK_RETURN                                                                           \
   do                                                                                              \
   {                                                                                               \
      mem_benchmark_restore_weight_profile(previous_weight_profile);                               \
      return;                                                                                      \
   } while (0)

   const char *suite = opt_get(&opts, "suite");
   if ((!suite || !suite[0]) && opts.pos_count > 0)
      suite = opts.positional[0];
   if (!suite || !suite[0])
      suite = "corpus";

   if (strcmp(suite, "corpus") == 0 || strcmp(suite, "memory-retrieval") == 0 ||
       strcmp(suite, "locomo") == 0 || strcmp(suite, "longmemeval") == 0 ||
       strcmp(suite, "locomo-qa") == 0 || strcmp(suite, "longmemeval-qa") == 0 ||
       strcmp(suite, "locomo-session-support") == 0 || strcmp(suite, "locomo-misses") == 0 ||
       strcmp(suite, "longmemeval-misses") == 0)
   {
      int dataset_suite = strcmp(suite, "corpus") != 0 && strcmp(suite, "memory-retrieval") != 0;
      int qa_suite = strcmp(suite, "locomo-qa") == 0 || strcmp(suite, "longmemeval-qa") == 0;
      if (benchmark_weight_profile[0])
         fatal("legacy weight profiles are not supported by Go memory evaluation");
      char helper[4096], executable[4096], dimension[32];
      if (platform_get_exe_path(helper, sizeof(helper)) != 0)
         fatal("cannot locate the Go memory evaluator");
      snprintf(executable, sizeof(executable), "%s", helper);
      char *slash = strrchr(helper, '/');
#ifdef _WIN32
      char *backslash = strrchr(helper, '\\');
      if (!slash || (backslash && backslash > slash))
         slash = backslash;
      const char *suffix = "/aimee-memory-eval.exe";
#else
      const char *suffix = "/aimee-memory-eval";
#endif
      if (!slash || (size_t)(slash - helper) + strlen(suffix) + 1 > sizeof(helper))
         fatal("cannot locate the Go memory evaluator");
      strcpy(slash, suffix);
      snprintf(dimension, sizeof(dimension), "%d", config_resolve_embedder_dims_current());
      const char *embedder = config_embedder_command_current(NULL);
      const char *args[48] = {helper,
                              "-embedding-command",
                              embedder ? embedder : "",
                              "-embedding-dim",
                              dimension,
                              "-format",
                              ctx->json_output ? "json" : "text"};
      int next = 7;
      if (dataset_suite)
      {
         const char *dataset = opt_get(&opts, "dataset");
         const char *max_cases = opt_get(&opts, "max-cases");
         args[next++] = "-suite";
         args[next++] = suite;
         args[next++] = "-dataset";
         args[next++] = dataset ? dataset
                                : (strncmp(suite, "locomo", 6) == 0
                                       ? "data/locomo/locomo10.json"
                                       : "data/longmemeval/longmemeval_s_cleaned.json");
         args[next++] = "-max-cases";
         args[next++] = max_cases ? max_cases : "0";
         if (opt_get(&opts, "baseline") || opt_get_flag(&opts, "update-baseline"))
            fatal("dataset evaluation does not support corpus baselines");
      }
      else
      {
         const char *corpus = opt_get(&opts, "corpus");
         const char *baseline = opt_get(&opts, "baseline");
         args[next++] = "-corpus";
         args[next++] = corpus ? corpus : "tests/eval/memory_retrieval_corpus.json";
         args[next++] = "-baseline";
         args[next++] = baseline ? baseline : "tests/eval/memory_retrieval_baseline.json";
      }
      if (strcmp(suite, "locomo-misses") == 0 || strcmp(suite, "longmemeval-misses") == 0)
      {
         const char *limit = opt_get(&opts, "limit");
         const char *max_misses = opt_get(&opts, "max-misses");
         args[next++] = "-limit";
         args[next++] = limit ? limit : "5";
         args[next++] = "-max-misses";
         args[next++] = max_misses ? max_misses : "20";
      }
      if (qa_suite)
      {
         const char *top_k = opt_get(&opts, "top-k");
         const char *budget = opt_get(&opts, "token-budget");
         const char *failures = opt_get(&opts, "max-failures");
         args[next++] = "-agent-executable";
         args[next++] = executable;
         args[next++] = "-top-k";
         args[next++] = top_k ? top_k : "10";
         args[next++] = "-token-budget";
         args[next++] = budget ? budget : "2000";
         args[next++] = "-max-failures";
         args[next++] = failures ? failures : "5";
         if (opt_get_flag(&opts, "report-failures"))
            args[next++] = "-report-failures";
      }
      if (ctx->json_fields && ctx->json_fields[0])
      {
         args[next++] = "-fields";
         args[next++] = ctx->json_fields;
      }
      if (ctx->response_profile && ctx->response_profile[0])
      {
         args[next++] = "-profile";
         args[next++] = ctx->response_profile;
      }
      if (opt_get_flag(&opts, "update-baseline"))
         args[next++] = "-update-baseline";
      const char *schema = opt_get(&opts, "schema");
      if (schema)
      {
         args[next++] = "-schema";
         args[next++] = schema;
      }
      args[next] = NULL;
      char *output = NULL;
      int rc = safe_exec_capture(args, &output, 1024 * 1024);
      if (rc != 0 || !output)
      {
         free(output);
         fatal("Go memory evaluation failed (exit %d)", rc);
      }
      fputs(output, stdout);
      free(output);
      BENCHMARK_RETURN;
   }

   if (strcmp(suite, "live") == 0 || strcmp(suite, "code-graph-fusion") == 0)
   {
      if (benchmark_weight_profile[0])
         fatal("legacy weight profiles are not supported by Go memory evaluation");
      cJSON *request = cJSON_CreateObject();
      kb_client_memory_scope_context_apply(request);
      cJSON_AddStringToObject(request, "suite", suite);
      cJSON_AddStringToObject(request, "format", ctx->json_output ? "json" : "text");
      if (ctx->json_fields)
         cJSON_AddStringToObject(request, "fields", ctx->json_fields);
      if (ctx->response_profile)
         cJSON_AddStringToObject(request, "profile", ctx->response_profile);
      if (opt_get(&opts, "arm"))
         cJSON_AddStringToObject(request, "arm", opt_get(&opts, "arm"));
      if (opt_get(&opts, "fusion-state"))
         cJSON_AddStringToObject(request, "fusion_state", opt_get(&opts, "fusion-state"));
      const char *path = opt_get(&opts, "corpus");
      if (path)
         cJSON_AddStringToObject(request, "corpus", path);
      if (!strcmp(suite, "code-graph-fusion") && !path)
         path = "benchmarks/code-vector-graph/production-corpus.json";
      cJSON *result =
          mem_rpc_unwrap(kb_client_memory_benchmark_json(
                             request, !strcmp(suite, "code-graph-fusion") ? path : NULL),
                         "Go memory benchmark failed");
      const cJSON *output = cJSON_GetObjectItemCaseSensitive(result, "output");
      if (!cJSON_IsString(output))
         fatal("Go memory benchmark returned invalid output");
      fputs(output->valuestring, stdout);
      cJSON_Delete(result);
      BENCHMARK_RETURN;
   }

#undef BENCHMARK_RETURN
   fatal("unknown benchmark suite: %s", suite);
}
