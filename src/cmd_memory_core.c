#include "modules/kb_client/kb_client_pii.h"
/* cmd_memory_core.c: CRUD + stats + task/decision + link/tag subcommand
 * handlers for `aimee memory`. Extracted from cmd_memory.c so each bucket
 * can be read in isolation. Shared helpers and globals live in
 * cmd_memory_internal.h; the subcommand table and dispatcher stay in
 * cmd_memory.c. */
#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "cmd_memory_internal.h"
#include "cmd_review.h"
#include "modules/db2/c/memory_query.h"
#include "platform_process.h"
#include "tasks_compose.h"
#include "kb.h"
#include "kb_client.h"
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

/* The Go owner supplies JSON and text views; transport failures must not look
 * like a healthy empty store. The action transport takes ownership of request. */
static cJSON *memory_console_request(const char *method, cJSON *request)
{
   cJSON_AddStringToObject(request, "view", "console");
   char *raw = kb_v1_action_request(method, request);
   cJSON *reply = raw ? cJSON_ParseWithOpts(raw, NULL, 1) : NULL;
   free(raw);
   if (!cJSON_IsObject(reply) || strcmp(jo_cstr(reply, "status"), "ok") != 0)
      fatal("%s: %s", method,
            jo_str(reply, "message", "memory service unavailable or invalid response"));
   if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(reply, "text")))
      fatal("%s: invalid memory console response", method);
   return reply;
}

static cJSON *memory_console_reply(const char *method, int effectiveness)
{
   cJSON *request = cJSON_CreateObject();
   if (effectiveness)
      cJSON_AddBoolToObject(request, "effectiveness", 1);
   return memory_console_request(method, request);
}

/* Go renders complete inspection output, including field/profile selection.
 * Keeping output as a string avoids cJSON rounding integer IDs in row arrays. */
static void memory_inspection_output(app_ctx_t *ctx, const char *method, cJSON *request)
{
   cJSON_AddStringToObject(request, "format", ctx->json_output ? "json" : "text");
   if (ctx->json_fields)
      cJSON_AddStringToObject(request, "fields", ctx->json_fields);
   if (ctx->response_profile)
      cJSON_AddStringToObject(request, "profile", ctx->response_profile);
   char *raw = kb_v1_action_request(method, request);
   cJSON *reply = raw ? cJSON_ParseWithOpts(raw, NULL, 1) : NULL;
   free(raw);
   const cJSON *output = cJSON_GetObjectItemCaseSensitive(reply, "output");
   if (strcmp(jo_cstr(reply, "status"), "ok") != 0 || !cJSON_IsString(output))
      fatal("%s: %s", method,
            jo_str(reply, "message", "memory inspection unavailable or invalid response"));
   fputs(output->valuestring, stdout);
   cJSON_Delete(reply);
}

void mem_store(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *tier = opt_get(&opts, "tier");
   if (!tier)
      tier = TIER_L0;
   const char *kind = opt_get(&opts, "kind");
   if (!kind)
      kind = KIND_FACT;
   const char *session = opt_get(&opts, "session");
   if (!session)
      session = "";
   const char *key = opt_pos(&opts, 0);
   const char *content = opt_pos(&opts, 1);
   if (!content)
      content = "";

   if (!key)
      fatal("memory store requires a key");

   const char *workspace = opt_get(&opts, "workspace");
   const char *scope_type = cmd_memory_scope_type(&opts);
   const char *scope_value = cmd_memory_scope_value(&opts);

   memory_t mem;
   /* `aimee memory store` is the user typing a note at their own terminal, so the
    * row records the user provenance and facts mined from it may be Class A. */
   if (kb_client_memory_insert_as(tier, kind, key, content, "", 1.0, session, MEMORY_AUTHORITY_USER,
                                  &mem) != 0)
      fatal("failed to store memory (key=%s, tier=%s) — check stderr for details", key, tier);

   /* Apply explicit workspace tag if provided */
   if (workspace && workspace[0])
   {
      cJSON *tag_args = cJSON_CreateObject();
      kb_client_memory_scope_context_apply(tag_args);
      cJSON_AddNumberToObject(tag_args, "memory_id", (double)mem.id);
      cJSON_AddStringToObject(tag_args, "workspace", workspace);
      free(kb_v1_action_request("memory.tag_workspace", tag_args));
   }
   if (scope_type && scope_type[0] && scope_value && scope_value[0])
   {
      cJSON *tag_args = cJSON_CreateObject();
      kb_client_memory_scope_context_apply(tag_args);
      cJSON_AddNumberToObject(tag_args, "memory_id", (double)mem.id);
      cJSON_AddStringToObject(tag_args, "scope_type", scope_type);
      cJSON_AddStringToObject(tag_args, "scope_value", scope_value);
      free(kb_v1_action_request("memory.tag_scope", tag_args));
   }

   if (ctx->json_output)
      emit_json_ctx(memory_to_json(&mem), ctx->json_fields, ctx->response_profile);
}

void mem_get(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory get requires an id");
   int64_t id = atoll(argv[0]);
   memory_t mem;
   if (kb_client_memory_get(id, &mem) != 0)
      fatal("memory not found: %lld", (long long)id);
   if (ctx->json_output)
      emit_json_ctx(memory_to_json(&mem), ctx->json_fields, ctx->response_profile);
}

void mem_delete(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory delete requires an id");
   int64_t id = atoll(argv[0]);
   if (kb_client_memory_delete(id) != 0)
      fatal("failed to delete memory: %lld", (long long)id);
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
}

/* `aimee memory approve <id> [--note "..."]`
 *
 * Record explicit operator approval for an L3→L4 promotion.  Only needed for
 * directive-kind memories (`policy`) — workflows bypass the gate because
 * they already arrive via the explicit store_workflow MCP tool.  The
 * maintenance cycle picks up the approval on the next run when the
 * approval-gated reclassification path is enabled. */
void mem_approve(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory approve requires an id (use `memory list --tier L3 --kind policy` "
            "to find promotion candidates)");
   int64_t id = atoll(argv[0]);

   opt_parsed_t opts;
   opt_parse(argc - 1, argv + 1, NULL, &opts);
   const char *note = opt_get(&opts, "note");
   const char *approver = opt_get(&opts, "approver");

   memory_t mem;
   if (kb_client_memory_get(id, &mem) != 0)
      fatal("memory not found: %lld", (long long)id);

   if (memory_approve_l4_promotion(id, approver ? approver : "operator", note ? note : "") != 0)
      fatal("failed to record approval for memory %lld", (long long)id);

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "memory_id", (double)id);
      cJSON_AddStringToObject(j, "target_tier", "L4");
      cJSON_AddStringToObject(j, "approver", approver ? approver : "operator");
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Approved memory %lld (%s) for L4 promotion on next maintain cycle.\n", (long long)id,
             mem.kind[0] ? mem.kind : "fact");
   }
}

void mem_activation(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory activation requires an id");
   int64_t id = atoll(argv[0]);
   opt_parsed_t opts;
   opt_parse(argc - 1, argv + 1, NULL, &opts);
   int sticky = opt_get_int(&opts, "sticky", 2);
   int cooldown = opt_get_int(&opts, "cooldown", 1);
   int delay = opt_get_int(&opts, "delay", 0);
   int suppressed = opt_get_flag(&opts, "suppress") ? 1 : 0;
   if (sticky < 0 || cooldown < 0 || delay < 0)
      fatal("memory activation turn counts must be non-negative");
   if (memory_activation_policy_set(id, sticky, cooldown, delay, suppressed) != 0)
      fatal("failed to update activation policy for memory %lld", (long long)id);
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "memory_id", (double)id);
      cJSON_AddNumberToObject(j, "sticky_turns", sticky);
      cJSON_AddNumberToObject(j, "cooldown_turns", cooldown);
      cJSON_AddNumberToObject(j, "delay_turns", delay);
      cJSON_AddBoolToObject(j, "suppressed", suppressed);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
      printf("Memory %lld activation: sticky=%d cooldown=%d delay=%d suppressed=%s\n",
             (long long)id, sticky, cooldown, delay, suppressed ? "yes" : "no");
}

void mem_list(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *tier = opt_get(&opts, "tier");
   const char *kind = opt_get(&opts, "kind");
   int limit = opt_get_int(&opts, "limit", 50);
   int low_eff = opt_get_int(&opts, "low-effectiveness", 0);

   if (low_eff)
   {
      cJSON *request = cJSON_CreateObject();
      cJSON_AddStringToObject(request, "view", "console");
      cJSON_AddNumberToObject(request, "limit", limit);
      memory_inspection_output(ctx, "memory.list_low_effectiveness", request);
      return;
   }

   memory_t mems[256];
   int count = kb_client_memory_list(tier, kind, limit, mems, 256);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
         cJSON_AddItemToArray(arr, memory_to_json(&mems[i]));
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
}

void mem_search(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory search requires query terms");

   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   cmd_memory_apply_rerank_mode(&opts);
   int limit = opt_get_int(&opts, "limit", 10);
   int explain = opt_get_flag(&opts, "explain");
   const char *as_of = opt_get(&opts, "as-of");

   memory_filter_t filter;
   cmd_memory_build_filter(&opts, &filter);
   if (as_of && as_of[0])
      snprintf(filter.as_of, sizeof(filter.as_of), "%s", as_of);

   char *clusters[64];
   int cluster_count = 0;
   for (int i = 0; i < opts.pos_count && cluster_count < 64; i++)
      clusters[cluster_count++] = (char *)opts.positional[i];

   /* Build combined query string for fact search */
   char query_buf[2048];
   int qpos = 0;
   for (int i = 0; i < cluster_count; i++)
   {
      if (i > 0)
         qpos = str_appendf(query_buf, qpos, (int)sizeof(query_buf), " ");
      qpos = str_appendf(query_buf, qpos, (int)sizeof(query_buf), "%s", clusters[i]);
   }

   /* Search stored facts */
   memory_t facts[64];
   int fact_count = cmd_memory_find_facts(&opts, query_buf, limit, facts, 64);
   cmd_memory_require_runtime(fact_count, "memory search");

   memory_diagnostic_t explain_rows[64];
   int explain_count = 0;
   cJSON *explain_stats = NULL;
   if (explain)
   {
      explain_count = cmd_memory_diagnose_query(&opts, query_buf, limit, explain_rows, 64);
      cmd_memory_require_runtime(explain_count, "memory search --explain");
      explain_stats = memory_console_reply("memory.stats", 0);
      if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(explain_stats, "pagerank_timing")) ||
          !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(explain_stats, "pagerank_text")))
         fatal("memory search --explain: invalid statistics response");
   }

   /* Search conversation windows */
   int max_results = limit < 64 ? limit : 64;
   search_result_t *results = calloc((size_t)max_results, sizeof(search_result_t));
   if (!results)
      fatal("out of memory");
   int win_count = kb_client_memory_search(clusters, cluster_count, limit, results, max_results);

   /* Optional as-of graph relations search */
   cJSON *as_of_response = NULL, *as_of_rels = NULL;
   if (as_of && query_buf[0])
   {
      cJSON *request = cJSON_CreateObject();
      kb_client_memory_scope_context_apply(request);
      cJSON_AddStringToObject(request, "query", query_buf);
      cJSON_AddStringToObject(request, "as_of", as_of);
      cJSON_AddNumberToObject(request, "limit", limit < 32 ? limit : 32);
      char *raw = kb_v1_action_request("memory.search_graph_as_of", request);
      as_of_response = raw ? cJSON_Parse(raw) : NULL;
      free(raw);
      as_of_rels = cJSON_GetObjectItemCaseSensitive(as_of_response, "relations");
      if (strcmp(jo_cstr(as_of_response, "status"), "ok") || !cJSON_IsArray(as_of_rels))
         fatal("memory as-of graph search failed: %s", jo_cstr(as_of_response, "message"));
   }
   int as_of_count = cJSON_GetArraySize(as_of_rels);

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();

      cJSON *farr = cJSON_CreateArray();
      for (int i = 0; i < fact_count; i++)
      {
         if (!explain)
         {
            cJSON_AddItemToArray(farr, memory_to_json(&facts[i]));
            continue;
         }
         cJSON *entry = cJSON_CreateObject();
         cJSON_AddItemToObject(entry, "memory", memory_to_json(&facts[i]));
         cJSON_AddNumberToObject(entry, "effective_importance",
                                 memory_effective_importance(&facts[i], 0));
         for (int j = 0; j < explain_count; j++)
         {
            if (explain_rows[j].memory.id != facts[i].id)
               continue;
            cJSON_AddItemToObject(entry, "score",
                                  memory_score_parts_to_json(&explain_rows[j].parts));
            break;
         }
         cJSON_AddItemToArray(farr, entry);
      }
      cJSON_AddItemToObject(obj, "facts", farr);

      cJSON *warr = cJSON_CreateArray();
      for (int i = 0; i < win_count; i++)
         cJSON_AddItemToArray(warr, search_result_to_json(&results[i]));
      cJSON_AddItemToObject(obj, "windows", warr);

      if (as_of_count > 0)
      {
         cJSON_AddItemToObject(obj, "as_of_relations", cJSON_Duplicate(as_of_rels, 1));
      }

      if (explain)
      {
         cJSON_AddItemToObject(
             obj, "pagerank_timing",
             cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(explain_stats, "pagerank_timing"),
                             1));

         /* Stable filter contract (memory-public-contract). */
         cJSON_AddItemToObject(obj, "filter_contract", memory_filter_to_json(&filter));
      }

      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else if (explain)
   {
      printf("Query: %s\n", query_buf);
      {
         cJSON *fc = memory_filter_to_json(&filter);
         char *fc_str = cJSON_PrintUnformatted(fc);
         printf("Filter: %s\n", fc_str ? fc_str : "{}");
         free(fc_str);
         cJSON_Delete(fc);
      }
      fputs(jo_cstr(explain_stats, "pagerank_text"), stdout);
      for (int i = 0; i < fact_count; i++)
      {
         printf("[%d] #%lld %s  eff_imp=%.3f\n", i + 1, (long long)facts[i].id, facts[i].key,
                memory_effective_importance(&facts[i], 0));
         for (int j = 0; j < explain_count; j++)
         {
            if (explain_rows[j].memory.id != facts[i].id)
               continue;
            printf(
                "    total=%.3f lexical=%.3f coverage=%.3f entity=%.3f temporal=%.3f evidence=%.3f "
                "semantic=%.3f state=%.3f intent=%.3f salience=%.3f surprise=%.3f pagerank=%.3f\n",
                explain_rows[j].parts.total, explain_rows[j].parts.lexical,
                explain_rows[j].parts.coverage, explain_rows[j].parts.entity,
                explain_rows[j].parts.temporal, explain_rows[j].parts.evidence,
                explain_rows[j].parts.semantic, explain_rows[j].parts.state,
                explain_rows[j].parts.intent, explain_rows[j].parts.salience,
                explain_rows[j].parts.surprise, explain_rows[j].parts.pagerank);
            break;
         }
      }
      if (as_of_count > 0)
      {
         printf("Graph relations as-of %s:\n", as_of);
         cJSON *relation;
         cJSON_ArrayForEach(relation, as_of_rels)
         {
            const char *valid = jo_cstr(relation, "valid_at");
            printf("  %s -[%s]-> %s  (valid_at=%s)\n", jo_cstr(relation, "src_entity"),
                   jo_cstr(relation, "relation"), jo_cstr(relation, "dst_entity"),
                   valid[0] ? valid : "(any)");
         }
      }
   }
   else if (as_of_count > 0)
   {
      printf("Graph relations as-of %s:\n", as_of);
      cJSON *relation;
      cJSON_ArrayForEach(relation, as_of_rels)
      {
         const char *valid = jo_cstr(relation, "valid_at");
         printf("  %s -[%s]-> %s  (valid_at=%s)\n", jo_cstr(relation, "src_entity"),
                jo_cstr(relation, "relation"), jo_cstr(relation, "dst_entity"),
                valid[0] ? valid : "(any)");
      }
   }
   cJSON_Delete(explain_stats);
   cJSON_Delete(as_of_response);
   free(results);
}

void mem_plan(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory plan requires query terms");

   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   int limit = opt_get_int(&opts, "limit", 10);
   int hard_cap = opt_get_int(&opts, "hard-cap", 96);

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
      fatal("memory plan requires query terms");

   memory_query_plan_t plan;
   if (memory_query_plan(query_buf, limit, hard_cap, &plan) != 0)
      fatal("failed to build memory query plan");

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      jo_add_str(obj, "query", query_buf);
      jo_add_str(obj, "route", memory_query_route_name(plan.route));
      jo_add_str(obj, "shape", memory_query_shape_name(plan.shape));
      jo_add_num(obj, "fetch_multiplier", plan.fetch_multiplier);
      jo_add_num(obj, "min_fetch", plan.min_fetch);
      jo_add_num(obj, "max_fetch", plan.max_fetch);
      jo_add_num(obj, "graph_hops", plan.graph_hops);
      jo_add_bool(obj, "semantic_enabled", plan.semantic_enabled);
      cJSON *weights = cJSON_CreateObject();
      jo_add_num(weights, "lexical", plan.weights.lexical_weight);
      jo_add_num(weights, "semantic", plan.weights.semantic_weight);
      jo_add_num(weights, "graph", plan.weights.graph_weight);
      jo_add_num(weights, "temporal", plan.weights.temporal_weight);
      cJSON_AddItemToObject(obj, "weights", weights);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("Query: %s\n", query_buf);
   printf("route=%s shape=%s fetch_multiplier=%d min_fetch=%d max_fetch=%d graph_hops=%d "
          "semantic_enabled=%s\n",
          memory_query_route_name(plan.route), memory_query_shape_name(plan.shape),
          plan.fetch_multiplier, plan.min_fetch, plan.max_fetch, plan.graph_hops,
          plan.semantic_enabled ? "true" : "false");
   printf("weights: lexical=%.2f semantic=%.2f graph=%.2f temporal=%.2f\n",
          plan.weights.lexical_weight, plan.weights.semantic_weight, plan.weights.graph_weight,
          plan.weights.temporal_weight);
}

void mem_stats(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   cJSON *reply = memory_console_reply("memory.stats", ctx->json_output);
   cJSON *display = cJSON_DetachItemFromObjectCaseSensitive(reply, "display");
   if (!cJSON_IsObject(display))
      fatal("memory.stats: invalid statistics response");
   if (ctx->json_output)
      emit_json_ctx(display, ctx->json_fields, ctx->response_profile);
   else
   {
      fputs(jo_cstr(reply, "text"), stdout);
      cJSON_Delete(display);
   }
   cJSON_Delete(reply);
}

void mem_scan(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   char dirs[8][MAX_PATH_LEN];
   int dir_count = config_conversation_dirs(dirs, 8);
   kb_client_memory_scan_conversations(dirs, dir_count);
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
}

void mem_queue(app_ctx_t *ctx, int argc, char **argv)
{
   const char *sub = argc > 0 ? argv[0] : "status";
   if (strcmp(sub, "status") != 0)
      fatal("memory queue only supports: status");

   char *resp_json = kb_client_queue_status_json();
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);

   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (!ok)
   {
      const char *msg = "failed to query async memory queue";
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      cJSON_Delete(resp);
      fatal("%s", msg);
   }

   int pending = 0, running = 0, done = 0, failed = 0, total = 0;
   cJSON *n;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "pending")) && cJSON_IsNumber(n))
      pending = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "running")) && cJSON_IsNumber(n))
      running = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "done")) && cJSON_IsNumber(n))
      done = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "failed")) && cJSON_IsNumber(n))
      failed = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "total")) && cJSON_IsNumber(n))
      total = (int)n->valuedouble;
   cJSON_Delete(resp);

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "pending", pending);
      cJSON_AddNumberToObject(j, "running", running);
      cJSON_AddNumberToObject(j, "done", done);
      cJSON_AddNumberToObject(j, "failed", failed);
      cJSON_AddNumberToObject(j, "total", total);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("Async memory queue:\n");
   printf("  pending: %d\n", pending);
   printf("  running: %d\n", running);
   printf("  done:    %d\n", done);
   printf("  failed:  %d\n", failed);
   printf("  total:   %d\n", total);
}

void mem_drain(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   int timeout = opt_get_int(&opts, "timeout", 0);
   const char *embed_cmd = config_embedder_command_current(NULL);

   char *resp_json = kb_client_queue_drain_json(embed_cmd, timeout);
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);

   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (!ok)
   {
      const char *msg = "failed to drain async memory queue";
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      cJSON_Delete(resp);
      fatal("%s", msg);
   }

   kb_async_queue_stats_t stats;
   memset(&stats, 0, sizeof(stats));
   cJSON *n;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "processed")) && cJSON_IsNumber(n))
      stats.processed = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "pending")) && cJSON_IsNumber(n))
      stats.pending = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "running")) && cJSON_IsNumber(n))
      stats.running = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "done")) && cJSON_IsNumber(n))
      stats.done = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "failed")) && cJSON_IsNumber(n))
      stats.failed = (int)n->valuedouble;
   if ((n = cJSON_GetObjectItemCaseSensitive(resp, "total")) && cJSON_IsNumber(n))
      stats.total = (int)n->valuedouble;
   cJSON_Delete(resp);

   cJSON *cog_args = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(cog_args);
   char *cog_raw = kb_v1_action_request_with_timeout("memory.cognify_drain", cog_args, 60000L);
   cJSON *cog = cog_raw ? cJSON_Parse(cog_raw) : NULL;
   free(cog_raw);
   const char *cog_status = jo_cstr(cog, "status");
   if (strcmp(cog_status, "ok") != 0 && strcmp(cog_status, "disabled") != 0)
   {
      cJSON_Delete(cog);
      fatal("failed to drain cognification queue");
   }

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "processed", stats.processed);
      cJSON_AddNumberToObject(j, "pending", stats.pending);
      cJSON_AddNumberToObject(j, "running", stats.running);
      cJSON_AddNumberToObject(j, "done", stats.done);
      cJSON_AddNumberToObject(j, "failed", stats.failed);
      cJSON_AddNumberToObject(j, "total", stats.total);
      cJSON_AddItemToObject(j, "cognify", cog);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("Async memory queue drained: processed=%d pending=%d running=%d failed=%d\n",
          stats.processed, stats.pending, stats.running, stats.failed);
   if (strcmp(cog_status, "ok") == 0)
      printf("Cognify queue: processed=%d pending=%d failed=%d retried=%d\n",
             jo_int(cog, "processed", 0), jo_int(cog, "pending", 0), jo_int(cog, "failed", 0),
             jo_int(cog, "retried", 0));
   cJSON_Delete(cog);
}

void mem_edges(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory edges requires an entity name");
   edge_t edges[128];
   int count = kb_client_memory_query_edges(argv[0], edges, 128);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *e = cJSON_CreateObject();
         cJSON_AddNumberToObject(e, "id", (double)edges[i].id);
         cJSON_AddStringToObject(e, "source", edges[i].source);
         cJSON_AddStringToObject(e, "relation", edges[i].relation);
         cJSON_AddStringToObject(e, "target", edges[i].target);
         cJSON_AddNumberToObject(e, "weight", edges[i].weight);
         cJSON_AddItemToArray(arr, e);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
}

void mem_compact(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   int summary_count = 0, fact_count = 0;
   kb_client_memory_compact_windows(&summary_count, &fact_count);
   if (ctx->json_output)
   {
      cJSON *j = jo_ok();
      cJSON_AddNumberToObject(j, "summaries", summary_count);
      cJSON_AddNumberToObject(j, "facts", fact_count);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
}

void mem_conflicts(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   conflict_t conflicts[64];
   int count = kb_client_memory_list_conflicts(conflicts, 64);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
         cJSON_AddItemToArray(arr, conflict_to_json(&conflicts[i]));
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
}

void mem_health(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   cJSON *reply = memory_console_reply("memory.query_health", 0);
   cJSON *health = cJSON_DetachItemFromObjectCaseSensitive(reply, "health");
   if (!cJSON_IsObject(health))
      fatal("memory.query_health: invalid health response");
   if (ctx->json_output)
      emit_json_ctx(health, ctx->json_fields, ctx->response_profile);
   else
   {
      fputs(jo_cstr(reply, "text"), stdout);
      cJSON_Delete(health);
   }
   cJSON_Delete(reply);
}

void mem_provenance(app_ctx_t *ctx, int argc, char **argv)
{
   /* --stale flag: show memories with suspicious provenance */
   if (argc >= 1 && strcmp(argv[0], "--stale") == 0)
   {
      cJSON *request = cJSON_CreateObject();
      cJSON_AddStringToObject(request, "view", "stale");
      memory_inspection_output(ctx, "memory.list_unused_l2", request);
      return;
   }

   /* Default: show provenance for a specific memory ID */
   if (argc < 1)
      fatal("usage: aimee memory provenance <id> | --stale");

   int64_t id = atoll(argv[0]);

   /* Get memory info */
   memory_t mem;
   if (kb_client_memory_get(id, &mem) != 0)
      fatal("memory not found: %lld", (long long)id);

   cJSON *args = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(args);
   cJSON_AddNumberToObject(args, "memory_id", (double)id);
   cJSON_AddNumberToObject(args, "max", MAX_PROVENANCE_ENTRIES);
   char *json = kb_v1_action_request("memory.get_provenance", args);
   cJSON *response = json ? cJSON_Parse(json) : NULL;
   free(json);
   cJSON *entries = cJSON_GetObjectItemCaseSensitive(response, "entries");
   if (strcmp(jo_cstr(response, "status"), "ok") != 0 || !cJSON_IsArray(entries))
   {
      cJSON_Delete(response);
      fatal("failed to read provenance for memory %lld", (long long)id);
   }
   int count = cJSON_GetArraySize(entries);
   if (ctx->json_output)
   {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddNumberToObject(root, "memory_id", (double)id);
      cJSON_AddStringToObject(root, "key", mem.key);
      cJSON_AddStringToObject(root, "tier", mem.tier);
      cJSON_AddStringToObject(root, "kind", mem.kind);
      cJSON_AddItemToObject(root, "provenance", cJSON_DetachItemViaPointer(response, entries));
      emit_json_ctx(root, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Memory #%lld: %s (%s, %s, confidence: %.2f)\n", (long long)id, mem.key, mem.tier,
             mem.kind, mem.confidence);
      if (count == 0)
         printf("  (no provenance records)\n");
      cJSON *entry = NULL;
      cJSON_ArrayForEach(entry, entries)
      {
         printf("  %.10s  %-10s session:%.8s", jo_cstr(entry, "created_at"),
                jo_cstr(entry, "action"), jo_cstr(entry, "session_id"));
         const char *details = jo_cstr(entry, "details");
         if (details[0])
            printf("  \"%s\"", details);
         printf("\n");
      }
   }
   cJSON_Delete(response);
}

void mem_maintain(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *modes = opt_get(&opts, "modes");
   int watch_secs = opt_get_int(&opts, "watch", 0);
   do
   {
      cJSON *request = cJSON_CreateObject();
      cJSON_AddStringToObject(request, "modes_csv", modes ? modes : "");
      cJSON_AddBoolToObject(request, "force", opt_get_flag(&opts, "force"));
      cJSON_AddBoolToObject(request, "dry_run", opt_get_flag(&opts, "dry-run"));
      cJSON *reply = memory_console_request("memory.maintenance_run", request);
      cJSON *display = cJSON_DetachItemFromObjectCaseSensitive(reply, "display");
      if (!cJSON_IsObject(display))
         fatal("memory.maintenance_run: invalid maintenance response");
      if (ctx->json_output)
         emit_json_ctx(display, ctx->json_fields, ctx->response_profile);
      else
      {
         fputs(jo_cstr(reply, "text"), stdout);
         cJSON_Delete(display);
      }
      cJSON_Delete(reply);
      if (watch_secs > 0)
         sleep((unsigned)watch_secs);
   } while (watch_secs > 0);
}

void mem_briefing(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   cJSON *request = cJSON_CreateObject();
   cJSON_AddNumberToObject(request, "limit_tokens", opt_get_int(&opts, "limit-tokens", 0));
   cJSON_AddStringToObject(request, "format", ctx->json_output ? "json" : "text");
   if (ctx->json_fields)
      cJSON_AddStringToObject(request, "fields", ctx->json_fields);
   if (ctx->response_profile)
      cJSON_AddStringToObject(request, "profile", ctx->response_profile);
   kb_client_memory_scope_context_apply(request);
   char *raw = kb_v1_action_request("memory.briefing", request);
   cJSON *reply = raw ? cJSON_Parse(raw) : NULL;
   free(raw);
   const cJSON *output = reply ? cJSON_GetObjectItemCaseSensitive(reply, "output") : NULL;
   if (!reply || strcmp(jo_cstr(reply, "status"), "ok") != 0 || !cJSON_IsString(output))
   {
      cJSON_Delete(reply);
      fatal("memory briefing failed or returned malformed output");
   }
   fputs(output->valuestring, stdout);
   if (ctx->json_output)
      fputc('\n', stdout);
   cJSON_Delete(reply);
}

/* Read the "prospective" object from a kb response envelope and detach
 * it for the caller to render or emit.  Returns NULL when status != ok
 * or the object is missing.  Caller frees the detached object. */
static cJSON *mem_prospective_detach_from_envelope(const char *envelope_str, const char *field)
{
   cJSON *resp = envelope_str ? cJSON_Parse(envelope_str) : NULL;
   if (!resp)
      return NULL;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON *body = cJSON_GetObjectItemCaseSensitive(resp, field);
   cJSON *detached = body ? cJSON_DetachItemViaPointer(resp, body) : NULL;
   cJSON_Delete(resp);
   return detached;
}

void mem_remind(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *when = opt_get(&opts, "when");
   const char *doit = opt_get(&opts, "do");
   const char *entity = opt_get(&opts, "entity");
   const char *file = opt_get(&opts, "file");
   const char *recur = opt_get(&opts, "recur");
   const char *valid_until = opt_get(&opts, "valid-until");
   const char *session = opt_get(&opts, "session");

   if (!when || !when[0])
      fatal("memory remind requires --when \"<trigger text>\"");
   if (!doit || !doit[0])
      fatal("memory remind requires --do \"<reminder text>\"");

   (void)session; /* the kb side stamps the source session itself */
   cJSON *create_args = cJSON_CreateObject();
   int withheld = !create_args ||
                  kb_client_pii_add_string_required(create_args, "trigger_text", when) != 0 ||
                  kb_client_pii_add_string_required(create_args, "action_text", doit) != 0 ||
                  kb_client_pii_add_string(create_args, "anchor_entity", entity) != 0 ||
                  kb_client_pii_identifier_sensitive(file);
   char *envelope = NULL;
   if (withheld)
   {
      cJSON_Delete(create_args);
      kb_client_memory_audit_note("memory.prospective_create.withheld_pii", 0, NULL, NULL, NULL, 0,
                                  NULL, 0);
      envelope = kb_client_pii_withheld_json();
   }
   else
   {
      if (file)
         cJSON_AddStringToObject(create_args, "anchor_file", file);
      if (recur)
         cJSON_AddStringToObject(create_args, "recurrence", recur);
      if (valid_until)
         cJSON_AddStringToObject(create_args, "valid_until", valid_until);
      envelope = kb_v1_action_request("memory.prospective_create", create_args);
   }
   cJSON *prospective = mem_prospective_detach_from_envelope(envelope, "prospective");
   free(envelope);
   if (!prospective)
      fatal("memory remind failed: check recurrence (once|repeat) and required fields");

   if (ctx->json_output)
      emit_json_ctx(prospective, ctx->json_fields, ctx->response_profile);
   else
   {
      cJSON *id_j = cJSON_GetObjectItemCaseSensitive(prospective, "id");
      cJSON *trig = cJSON_GetObjectItemCaseSensitive(prospective, "trigger_text");
      cJSON *act = cJSON_GetObjectItemCaseSensitive(prospective, "action_text");
      cJSON *recr = cJSON_GetObjectItemCaseSensitive(prospective, "recurrence");
      cJSON *valid = cJSON_GetObjectItemCaseSensitive(prospective, "valid_until");
      const char *valid_str = cJSON_IsString(valid) ? valid->valuestring : "";
      printf("Armed reminder #%lld: when=\"%s\" do=\"%s\" recurrence=%s%s%s\n",
             (long long)(cJSON_IsNumber(id_j) ? id_j->valuedouble : 0),
             cJSON_IsString(trig) ? trig->valuestring : "",
             cJSON_IsString(act) ? act->valuestring : "",
             cJSON_IsString(recr) ? recr->valuestring : "", valid_str[0] ? " valid_until=" : "",
             valid_str);
      cJSON_Delete(prospective);
   }
}

void mem_reminders(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);

   /* --complete <id> / --expire-sweep are mutating operations that take
    * precedence over the list view so `reminders --complete N` doesn't get
    * confused with a filter. */
   const char *complete_arg = opt_get(&opts, "complete");
   if (complete_arg && complete_arg[0])
   {
      int64_t id = atoll(complete_arg);
      cJSON *complete_args = cJSON_CreateObject();
      cJSON_AddNumberToObject(complete_args, "id", (double)(id));
      char *envelope = kb_v1_action_request("memory.prospective_complete", complete_args);
      cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
      free(envelope);
      cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
      int ok = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0);
      cJSON_Delete(resp);
      if (!ok)
         fatal("could not complete reminder %lld (already terminal or missing)", (long long)id);
      if (ctx->json_output)
      {
         cJSON *j = cJSON_CreateObject();
         cJSON_AddNumberToObject(j, "id", (double)id);
         cJSON_AddStringToObject(j, "state", MEMORY_PROSPECTIVE_STATE_COMPLETED);
         emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("Completed reminder %lld\n", (long long)id);
      }
      return;
   }
   if (opt_get_flag(&opts, "expire-sweep"))
   {
      char *envelope =
          kb_v1_action_request("memory.prospective_sweep_expired", cJSON_CreateObject());
      cJSON *response = envelope ? cJSON_Parse(envelope) : NULL;
      free(envelope);
      const char *status = jo_cstr(response, "status");
      if (strcmp(status, "ok") != 0)
      {
         cJSON_Delete(response);
         fatal("memory reminder expiry failed");
      }
      int n = jo_int(response, "expired", 0);
      cJSON_Delete(response);
      if (ctx->json_output)
      {
         cJSON *j = cJSON_CreateObject();
         cJSON_AddNumberToObject(j, "expired", n);
         emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("Expired %d reminder(s)\n", n);
      }
      return;
   }

   const char *state = opt_get(&opts, "state");
   int limit = opt_get_int(&opts, "limit", 50);
   if (limit < 1)
      limit = 1;
   if (limit > 256)
      limit = 256;

   cJSON *list_args = cJSON_CreateObject();
   if (state && state[0])
      cJSON_AddStringToObject(list_args, "state", state);
   cJSON_AddNumberToObject(list_args, "limit", limit);
   char *envelope = kb_v1_action_request("memory.prospective_list", list_args);
   cJSON *prospectives = mem_prospective_detach_from_envelope(envelope, "prospectives");
   free(envelope);
   if (!prospectives || !cJSON_IsArray(prospectives))
   {
      cJSON_Delete(prospectives);
      if (ctx->json_output)
         emit_json_ctx(cJSON_CreateArray(), ctx->json_fields, ctx->response_profile);
      else
         printf("No reminders%s%s.\n", state && state[0] ? " in state " : "",
                state && state[0] ? state : "");
      return;
   }

   if (ctx->json_output)
   {
      emit_json_ctx(prospectives, ctx->json_fields, ctx->response_profile);
      return;
   }

   int n = cJSON_GetArraySize(prospectives);
   if (n == 0)
   {
      printf("No reminders%s%s.\n", state && state[0] ? " in state " : "",
             state && state[0] ? state : "");
      cJSON_Delete(prospectives);
      return;
   }
   printf("%d reminder(s):\n", n);
   cJSON *r = NULL;
   cJSON_ArrayForEach(r, prospectives)
   {
      cJSON *id_j = cJSON_GetObjectItemCaseSensitive(r, "id");
      cJSON *st = cJSON_GetObjectItemCaseSensitive(r, "state");
      cJSON *trig = cJSON_GetObjectItemCaseSensitive(r, "trigger_text");
      cJSON *act = cJSON_GetObjectItemCaseSensitive(r, "action_text");
      cJSON *recr = cJSON_GetObjectItemCaseSensitive(r, "recurrence");
      cJSON *valid = cJSON_GetObjectItemCaseSensitive(r, "valid_until");
      const char *valid_str = cJSON_IsString(valid) ? valid->valuestring : "";
      printf(
          "  #%lld [%s] when=\"%s\"\n", (long long)(cJSON_IsNumber(id_j) ? id_j->valuedouble : 0),
          cJSON_IsString(st) ? st->valuestring : "", cJSON_IsString(trig) ? trig->valuestring : "");
      printf("        do=\"%s\" recur=%s%s%s\n", cJSON_IsString(act) ? act->valuestring : "",
             cJSON_IsString(recr) ? recr->valuestring : "", valid_str[0] ? " valid_until=" : "",
             valid_str);
   }
   cJSON_Delete(prospectives);
}

void mem_alerts(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *since = opt_get(&opts, "since");

   cJSON *request = cJSON_CreateObject();
   if (since)
      cJSON_AddStringToObject(request, "since", since);
   cJSON_AddStringToObject(request, "format", ctx->json_output ? "json" : "text");
   if (ctx->json_fields)
      cJSON_AddStringToObject(request, "fields", ctx->json_fields);
   if (ctx->response_profile)
      cJSON_AddStringToObject(request, "profile", ctx->response_profile);
   kb_client_memory_scope_context_apply(request);
   char *raw = kb_v1_action_request("memory.alerts", request);
   cJSON *reply = raw ? cJSON_Parse(raw) : NULL;
   free(raw);
   const cJSON *output = reply ? cJSON_GetObjectItemCaseSensitive(reply, "output") : NULL;
   if (!reply || strcmp(jo_cstr(reply, "status"), "ok") != 0 || !cJSON_IsString(output))
   {
      cJSON_Delete(reply);
      fatal("memory alerts failed or returned malformed output");
   }
   fputs(output->valuestring, stdout);
   if (ctx->json_output)
      fputc('\n', stdout);
   cJSON_Delete(reply);
}

static void mem_recall_print_section(const char *header, cJSON *section, int show_why)
{
   int n = cJSON_GetArraySize(section);
   printf("## %s (%d)\n", header, n);
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, section)
   {
      long long id = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "memory_id"));
      const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(it, "key"));
      const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
      printf("  - #%lld [%s] %s\n", id, key ? key : "", text ? text : "");
      if (show_why)
      {
         const char *why = cJSON_GetStringValue(cJSON_GetObjectItem(it, "why"));
         printf("      why: %s\n", why ? why : "");
      }
   }
}

void mem_recall(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *task_hint = opt_get(&opts, "task");
   int session_start = opt_get_flag(&opts, "session-start");
   int explain = opt_get_flag(&opts, "explain");
   int limit_tokens = opt_get_int(&opts, "limit-tokens", 0);

   char *envelope = kb_client_memory_recall_json(task_hint, limit_tokens, session_start);
   cJSON *bundle = mem_prospective_detach_from_envelope(envelope, "recall");
   free(envelope);
   if (!bundle)
      fatal("memory recall failed");

   if (ctx->json_output)
   {
      emit_json_ctx(bundle, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("# Proactive Recall (%s)\n\n", session_start ? "session-start" : "per-turn");
   cJSON *aor = cJSON_GetObjectItemCaseSensitive(bundle, "always_on_rules");
   int aor_n = aor ? cJSON_GetArraySize(aor) : 0;
   if (aor_n > 0)
   {
      printf("## Always-On Rules (%d)\n", aor_n);
      cJSON *it = NULL;
      cJSON_ArrayForEach(it, aor)
      {
         const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(it, "title"));
         const char *desc = cJSON_GetStringValue(cJSON_GetObjectItem(it, "description"));
         int weight = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "weight"));
         printf("  - [w=%d] %s", weight, title ? title : "");
         if (desc && desc[0])
            printf(": %s", desc);
         printf("\n");
      }
   }
   mem_recall_print_section("Identity", cJSON_GetObjectItemCaseSensitive(bundle, "identity"),
                            explain);
   mem_recall_print_section("Preferences", cJSON_GetObjectItemCaseSensitive(bundle, "preferences"),
                            explain);
   mem_recall_print_section("Active Context",
                            cJSON_GetObjectItemCaseSensitive(bundle, "active_context"), explain);
   mem_recall_print_section("Open Commitments",
                            cJSON_GetObjectItemCaseSensitive(bundle, "open_commitments"), explain);
   mem_recall_print_section("Reminders", cJSON_GetObjectItemCaseSensitive(bundle, "reminders"),
                            explain);
   mem_recall_print_section("Directives", cJSON_GetObjectItemCaseSensitive(bundle, "directives"),
                            explain);

   int approx =
       (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(bundle, "approx_tokens"));
   int cap = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(bundle, "limit_tokens"));
   double ms = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(bundle, "elapsed_ms"));
   printf("\napprox_tokens=%d / limit_tokens=%d, assembled in %.2fms\n", approx, cap, ms);
   cJSON_Delete(bundle);
}

/* Parse a JSON envelope returned by kb_client_memory_directive_*; fatal on
 * non-ok status.  Returns the owned cJSON* the caller must cJSON_Delete. */
static cJSON *directive_rpc_unwrap(char *resp_json, const char *what)
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

void mem_directive(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *question = opt_get(&opts, "question");
   const char *topic = opt_get(&opts, "topic");
   const char *entity = opt_get(&opts, "entity");
   const char *file = opt_get(&opts, "file");
   const char *cause = opt_get(&opts, "cause");
   const char *valid_until = opt_get(&opts, "valid-until");
   const char *session = opt_get(&opts, "session");
   int priority = opt_get_int(&opts, "priority", 50);

   if (!question || !question[0])
      fatal("memory directive requires --question \"<text>\"");

   cJSON *directive_args = cJSON_CreateObject();
   if (kb_client_pii_identifier_sensitive(entity) || kb_client_pii_identifier_sensitive(file) ||
       kb_client_pii_add_string_required(directive_args, "question", question) != 0 ||
       kb_client_pii_add_string(directive_args, "topic", topic) != 0 ||
       kb_client_pii_add_string(directive_args, "cause", cause) != 0)
   {
      cJSON_Delete(directive_args);
      fatal("withheld_pii: content was not sent to aimee-kb");
   }
   if (entity && entity[0])
      cJSON_AddStringToObject(directive_args, "entity", entity);
   if (file && file[0])
      cJSON_AddStringToObject(directive_args, "file", file);
   cJSON_AddNumberToObject(directive_args, "priority", priority);
   if (session && session[0])
      cJSON_AddStringToObject(directive_args, "session", session);
   if (valid_until && valid_until[0])
      cJSON_AddStringToObject(directive_args, "valid_until", valid_until);
   cJSON *resp =
       directive_rpc_unwrap(kb_v1_action_request("memory.directive_create", directive_args),
                            "memory directive create failed: check cause "
                            "(contradiction|retrieval_failure|missing_config|user_follow_up) "
                            "and required fields");

   cJSON *dedup_j = cJSON_GetObjectItemCaseSensitive(resp, "dedup");
   if (cJSON_IsTrue(dedup_j))
   {
      cJSON_Delete(resp);
      printf("Directive already exists for this cause+key; no-op.\n");
      return;
   }

   cJSON *d = cJSON_GetObjectItemCaseSensitive(resp, "directive");
   if (!cJSON_IsObject(d) || jo_i64(d, "id", 0) <= 0 ||
       !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(d, "question")))
   {
      cJSON_Delete(resp);
      fatal("memory directive create: unexpected response");
   }

   if (ctx->json_output)
      emit_json_ctx(cJSON_Duplicate(d, 1), ctx->json_fields, ctx->response_profile);
   else
      printf("Opened directive #%lld [%s, p%d]: %s\n", (long long)jo_i64(d, "id", 0),
             jo_str(d, "cause", ""), jo_int(d, "priority", 0), jo_str(d, "question", ""));
   cJSON_Delete(resp);
}

void mem_directives(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc > 0 && strcmp(argv[0], "review") == 0)
   {
      cmd_review_surface("epistemic_directive", ctx, argc - 1, argv + 1);
      return;
   }

   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);

   const char *resolve_arg = opt_get(&opts, "resolve");
   if (resolve_arg && resolve_arg[0])
   {
      int64_t id = atoll(resolve_arg);
      int64_t with_memory = opt_get_int(&opts, "with-memory", 0);
      const char *note = opt_get(&opts, "note");
      char err[96];
      snprintf(err, sizeof(err), "could not resolve directive %lld (not open or missing)",
               (long long)id);
      cJSON *directive_args = cJSON_CreateObject();
      cJSON_AddNumberToObject(directive_args, "id", (double)id);
      if (with_memory > 0)
         cJSON_AddNumberToObject(directive_args, "with_memory", (double)with_memory);
      if (kb_client_pii_add_string(directive_args, "note", note) != 0)
      {
         cJSON_Delete(directive_args);
         fatal("withheld_pii: content was not sent to aimee-kb");
      }
      cJSON *resp = directive_rpc_unwrap(
          kb_v1_action_request("memory.directive_resolve", directive_args), err);
      cJSON_Delete(resp);
      if (ctx->json_output)
      {
         cJSON *j = cJSON_CreateObject();
         cJSON_AddNumberToObject(j, "id", (double)id);
         cJSON_AddStringToObject(j, "state", MEMORY_DIRECTIVE_STATE_RESOLVED);
         emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      }
      else
         printf("Resolved directive %lld\n", (long long)id);
      return;
   }
   const char *suppress_arg = opt_get(&opts, "suppress");
   if (suppress_arg && suppress_arg[0])
   {
      int64_t id = atoll(suppress_arg);
      char err[96];
      snprintf(err, sizeof(err), "could not suppress directive %lld (not open or missing)",
               (long long)id);
      cJSON *directive_args = cJSON_CreateObject();
      cJSON_AddNumberToObject(directive_args, "id", (double)id);
      cJSON *resp = directive_rpc_unwrap(
          kb_v1_action_request("memory.directive_suppress", directive_args), err);
      cJSON_Delete(resp);
      if (ctx->json_output)
      {
         cJSON *j = cJSON_CreateObject();
         cJSON_AddNumberToObject(j, "id", (double)id);
         cJSON_AddStringToObject(j, "state", MEMORY_DIRECTIVE_STATE_SUPPRESSED);
         emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      }
      else
         printf("Suppressed directive %lld\n", (long long)id);
      return;
   }
   if (opt_get_flag(&opts, "expire-sweep"))
   {
      cJSON *resp = directive_rpc_unwrap(
          kb_v1_action_request("memory.directive_sweep_expired", cJSON_CreateObject()),
          "directive sweep failed");
      cJSON *n_j = cJSON_GetObjectItemCaseSensitive(resp, "expired");
      int n = cJSON_IsNumber(n_j) ? (int)n_j->valuedouble : 0;
      cJSON_Delete(resp);
      if (ctx->json_output)
      {
         cJSON *j = cJSON_CreateObject();
         cJSON_AddNumberToObject(j, "expired", n);
         emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      }
      else
         printf("Expired %d directive(s)\n", n);
      return;
   }

   const char *state = opt_get(&opts, "state");
   const char *cause = opt_get(&opts, "cause");
   int limit = opt_get_int(&opts, "limit", 50);
   if (limit < 1)
      limit = 1;
   if (limit > 256)
      limit = 256;

   cJSON *directive_args = cJSON_CreateObject();
   if (state && state[0])
      cJSON_AddStringToObject(directive_args, "state", state);
   if (cause && cause[0])
      cJSON_AddStringToObject(directive_args, "cause", cause);
   cJSON_AddNumberToObject(directive_args, "limit", limit);
   cJSON *resp = directive_rpc_unwrap(kb_v1_action_request("memory.directive_list", directive_args),
                                      "directive list failed");
   cJSON *arr_src = cJSON_GetObjectItemCaseSensitive(resp, "directives");
   if (!cJSON_IsArray(arr_src))
   {
      cJSON_Delete(resp);
      fatal("directive list: unexpected response");
   }
   int n = cJSON_GetArraySize(arr_src);
   if (ctx->json_output)
   {
      emit_json_ctx(cJSON_Duplicate(arr_src, 1), ctx->json_fields, ctx->response_profile);
   }
   else if (n == 0)
   {
      printf("No directives%s%s.\n", state && state[0] ? " in state " : "",
             state && state[0] ? state : "");
   }
   else
   {
      printf("%d directive(s):\n", n);
      cJSON *row;
      cJSON_ArrayForEach(row, arr_src)
      {
         printf("  #%lld [%s, %s, p%d] %s\n", (long long)jo_i64(row, "id", 0),
                jo_str(row, "state", ""), jo_str(row, "cause", ""), jo_int(row, "priority", 0),
                jo_str(row, "question", ""));
         const char *topic = jo_str(row, "topic", ""), *until = jo_str(row, "valid_until", "");
         if (topic[0])
            printf("        topic=%s surfaced=%d%s%s\n", topic, jo_int(row, "surfaced_count", 0),
                   until[0] ? " valid_until=" : "", until);
      }
   }
   cJSON_Delete(resp);
}

void mem_task(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory task requires a subcommand: "
            "create, list, update, edges, link, delete");

   const char *tsub = argv[0];
   argc--;
   argv++;

   if (strcmp(tsub, "create") == 0)
   {
      if (argc < 1)
         fatal("memory task create requires a title");

      opt_parsed_t opts;
      opt_parse(argc, argv, NULL, &opts);
      const char *session = opt_get(&opts, "session");
      if (!session)
         session = "";
      const char *parent_str = opt_get(&opts, "parent");
      int64_t parent = parent_str ? atoll(parent_str) : 0;
      const char *title = opt_pos(&opts, 0);

      if (!title)
         fatal("memory task create requires a title");

      aimee_task_t task;
      if (kb_client_task_create(title, session, parent, &task) != 0)
         fatal("failed to create task");
      if (ctx->json_output)
         emit_json_ctx(aimee_task_to_json(&task), ctx->json_fields, ctx->response_profile);
   }
   else if (strcmp(tsub, "list") == 0)
   {
      opt_parsed_t opts;
      opt_parse(argc, argv, NULL, &opts);
      const char *state = opt_get(&opts, "state");
      const char *session = opt_get(&opts, "session");
      int limit = opt_get_int(&opts, "limit", 50);

      aimee_task_t tasks[128];
      int count = kb_client_task_list(state, session, limit, tasks, 128);
      if (ctx->json_output)
      {
         cJSON *arr = cJSON_CreateArray();
         for (int i = 0; i < count; i++)
            cJSON_AddItemToArray(arr, aimee_task_to_json(&tasks[i]));
         emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      }
   }
   else if (strcmp(tsub, "update") == 0)
   {
      if (argc < 2)
         fatal("memory task update requires id and state");
      int64_t id = atoll(argv[0]);
      kb_client_task_update_state(id, argv[1]);
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else if (strcmp(tsub, "edges") == 0)
   {
      if (argc < 1)
         fatal("memory task edges requires a task id");
      int64_t id = atoll(argv[0]);
      task_edge_t edges[64];
      int count = kb_client_task_get_edges(id, edges, 64);
      if (ctx->json_output)
      {
         cJSON *arr = cJSON_CreateArray();
         for (int i = 0; i < count; i++)
         {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "id", (double)edges[i].id);
            cJSON_AddNumberToObject(e, "source_id", (double)edges[i].source_id);
            cJSON_AddNumberToObject(e, "target_id", (double)edges[i].target_id);
            cJSON_AddStringToObject(e, "relation", edges[i].relation);
            cJSON_AddItemToArray(arr, e);
         }
         emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      }
   }
   else if (strcmp(tsub, "link") == 0)
   {
      if (argc < 2)
         fatal("memory task link requires source and target ids");
      int64_t source = atoll(argv[0]);
      int64_t target = atoll(argv[1]);
      const char *relation = "depends_on";
      if (argc >= 3)
         relation = argv[2];
      kb_client_task_add_edge(source, target, relation);
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else if (strcmp(tsub, "delete") == 0)
   {
      if (argc < 1)
         fatal("memory task delete requires an id");
      int64_t id = atoll(argv[0]);
      kb_client_task_delete(id);
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else
   {
      fatal("unknown task subcommand: %s", tsub);
   }
}

void mem_decide(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *options = opt_get(&opts, "options");
   if (!options)
      options = "";
   const char *chosen = opt_get(&opts, "chosen");
   if (!chosen)
      chosen = "";
   const char *rationale = opt_get(&opts, "rationale");
   if (!rationale)
      rationale = "";
   const char *assumptions = opt_get(&opts, "assumptions");
   if (!assumptions)
      assumptions = "";
   const char *task_str = opt_get(&opts, "task");
   int64_t task_id = task_str ? atoll(task_str) : 0;

   db2_decision_log_row_t dec;
   if (kb_client_decision_log_insert(task_id, options, chosen, rationale, assumptions, &dec) != 0)
      fatal("failed to log decision");
   if (ctx->json_output)
      emit_json_ctx(decision_to_json(&dec), ctx->json_fields, ctx->response_profile);
}

void mem_decisions(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *outcome = opt_get(&opts, "outcome");
   int limit = opt_get_int(&opts, "limit", 50);

   db2_decision_log_row_t decs[128];
   int count = kb_client_decision_log_list(outcome, limit, decs, 128);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
         cJSON_AddItemToArray(arr, decision_to_json(&decs[i]));
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
}

void mem_antipattern(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory antipattern requires: list, add, delete, or reset");

   const char *apsub = argv[0];
   argc--;
   argv++;

   if (strcmp(apsub, "list") == 0)
   {
      anti_pattern_t aps[128];
      int count = kb_client_anti_pattern_list(aps, 128);
      if (ctx->json_output)
      {
         cJSON *arr = cJSON_CreateArray();
         for (int i = 0; i < count; i++)
            cJSON_AddItemToArray(arr, anti_pattern_to_json(&aps[i]));
         emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      }
   }
   else if (strcmp(apsub, "add") == 0)
   {
      opt_parsed_t opts;
      opt_parse(argc, argv, NULL, &opts);
      const char *desc = opt_get(&opts, "desc");
      if (!desc)
         desc = "";
      const char *source = opt_get(&opts, "source");
      if (!source)
         source = "";
      const char *ref = opt_get(&opts, "ref");
      if (!ref)
         ref = "";
      const char *conf_str = opt_get(&opts, "confidence");
      double conf = conf_str ? atof(conf_str) : 1.0;
      const char *pattern = opt_pos(&opts, 0);

      if (!pattern)
         fatal("memory antipattern add requires a pattern");

      anti_pattern_t ap;
      if (kb_client_anti_pattern_insert(pattern, desc, source, ref, conf, &ap) != 0)
         fatal("failed to add anti-pattern");
      if (ctx->json_output)
         emit_json_ctx(anti_pattern_to_json(&ap), ctx->json_fields, ctx->response_profile);
   }
   else if (strcmp(apsub, "delete") == 0)
   {
      if (argc < 1)
         fatal("memory antipattern delete requires an id");
      int64_t id = atoll(argv[0]);
      kb_client_anti_pattern_delete(id);
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else if (strcmp(apsub, "review") == 0)
   {
      cmd_review_surface("anti_pattern", ctx, argc, argv);
   }
   else if (strcmp(apsub, "reset") == 0)
   {
      /* Clear per-session hit counters so a pattern that hit the block
       * threshold stops blocking further tool calls this session. Acts on the
       * session identified by AIMEE_SESSION_ID / CLAUDE_SESSION_ID. */
      const char *sid = session_id();

      session_state_t state;
      session_state_load(&state, sid);
      int cleared = state.ap_hit_count;
      state.ap_hit_count = 0;
      memset(state.ap_hits, 0, sizeof(state.ap_hits));
      session_state_force_save(&state, sid);

      if (ctx->json_output)
      {
         cJSON *obj = jo_ok();
         cJSON_AddNumberToObject(obj, "cleared", cleared);
         emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("cleared %d anti-pattern hit counter(s) for session\n", cleared);
      }
   }
   else
   {
      fatal("unknown antipattern subcommand: %s", apsub);
   }
}

void mem_supersede(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 2)
      fatal("memory supersede requires old_id and new_content");

   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *id_str = opt_pos(&opts, 0);
   const char *new_content = opt_pos(&opts, 1);
   if (!id_str || !new_content)
      fatal("memory supersede requires old_id and new_content");
   int64_t old_id = atoll(id_str);
   const char *conf_str = opt_get(&opts, "confidence");
   double conf = conf_str ? atof(conf_str) : 1.0;
   const char *session = opt_get(&opts, "session");
   if (!session)
      session = "";

   memory_t mem;
   if (kb_client_memory_supersede(old_id, new_content, conf, session, &mem) != 0)
      fatal("failed to supersede memory");
   if (ctx->json_output)
      emit_json_ctx(memory_to_json(&mem), ctx->json_fields, ctx->response_profile);
}

void mem_history(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory history requires a key");
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "view", "console");
   cJSON_AddStringToObject(request, "key", argv[0]);
   cJSON_AddNumberToObject(request, "max", 64);
   memory_inspection_output(ctx, "memory.fact_history", request);
}

void mem_checkpoint(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory checkpoint requires: create, list, restore, delete");

   const char *cpsub = argv[0];
   argc--;
   argv++;

   if (strcmp(cpsub, "create") == 0)
   {
      opt_parsed_t opts;
      opt_parse(argc, argv, NULL, &opts);
      const char *session = opt_get(&opts, "session");
      if (!session)
         session = "";
      const char *task_str = opt_get(&opts, "task");
      int64_t task_id = task_str ? atoll(task_str) : 0;
      const char *label = opt_pos(&opts, 0);
      if (!label)
         label = "";

      db1_checkpoint_t cp;
      if (tasks_checkpoint_create(label, session, task_id, &cp) != 0)
         fatal("failed to create checkpoint");
      if (ctx->json_output)
         emit_json_ctx(checkpoint_to_json(&cp), ctx->json_fields, ctx->response_profile);
   }
   else if (strcmp(cpsub, "list") == 0)
   {
      opt_parsed_t opts;
      opt_parse(argc, argv, NULL, &opts);
      int limit = opt_get_int(&opts, "limit", 50);

      db1_checkpoint_t cps[64];
      int count = db1_checkpoint_list(limit, cps, 64);
      if (ctx->json_output)
      {
         cJSON *arr = cJSON_CreateArray();
         for (int i = 0; i < count; i++)
            cJSON_AddItemToArray(arr, checkpoint_to_json(&cps[i]));
         emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      }
   }
   else if (strcmp(cpsub, "restore") == 0)
   {
      if (argc < 1)
         fatal("memory checkpoint restore requires an id");
      opt_parsed_t rsopts;
      opt_parse(argc, argv, NULL, &rsopts);
      const char *id_str = opt_pos(&rsopts, 0);
      if (!id_str)
         fatal("memory checkpoint restore requires an id");
      int64_t id = atoll(id_str);
      const char *session = opt_get(&rsopts, "session");
      if (!session)
         session = "";
      if (tasks_checkpoint_restore(id, session) != 0)
         fatal("failed to restore checkpoint");
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else if (strcmp(cpsub, "delete") == 0)
   {
      if (argc < 1)
         fatal("memory checkpoint delete requires an id");
      int64_t id = atoll(argv[0]);
      db1_checkpoint_delete(id);
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else
   {
      fatal("unknown checkpoint subcommand: %s", cpsub);
   }
}

void mem_style(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   kb_client_memory_learn_style();
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
}

void mem_read(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   /* Read = assemble context */
   char *mem_ctx = kb_client_memory_assemble_context(NULL);
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "context", mem_ctx ? mem_ctx : "");
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   free(mem_ctx);
}

void mem_link(app_ctx_t *ctx, int argc, char **argv)
{
   /* Artifact-aware memory: link a memory to an artifact (Feature 9) */
   if (argc < 3)
      fatal("usage: aimee memory link <id> file|commit|pr <ref>");
   int mem_id = atoi(argv[0]);
   const char *artifact_type = argv[1];
   const char *artifact_ref = argv[2];

   if (strcmp(artifact_type, "file") != 0 && strcmp(artifact_type, "commit") != 0 &&
       strcmp(artifact_type, "pr") != 0 && strcmp(artifact_type, "test_run") != 0)
      fatal("artifact type must be: file, commit, pr, or test_run");

   char hash[65] = {0};
   if (strcmp(artifact_type, "file") == 0)
   {
      FILE *f = fopen(artifact_ref, "r");
      if (f)
      {
         unsigned long h = 0;
         int c;
         while ((c = fgetc(f)) != EOF)
            h = h * 31 + (unsigned long)c;
         fclose(f);
         snprintf(hash, sizeof(hash), "%016lx", h);
      }
   }

   if (kb_client_memory_set_artifact(mem_id, artifact_type, artifact_ref, hash[0] ? hash : NULL) !=
       0)
      fatal("memory %d not found", mem_id);

   if (ctx->json_output)
   {
      cJSON *j = jo_ok();
      cJSON_AddNumberToObject(j, "memory_id", mem_id);
      cJSON_AddStringToObject(j, "artifact_type", artifact_type);
      cJSON_AddStringToObject(j, "artifact_ref", artifact_ref);
      if (hash[0])
         cJSON_AddStringToObject(j, "artifact_hash", hash);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Linked memory %d to %s:%s\n", mem_id, artifact_type, artifact_ref);
   }
}

void mem_mlink(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 3)
      fatal("usage: aimee memory mlink <source_id> <target_id> <relation>");
   int64_t src = atoll(argv[0]);
   int64_t tgt = atoll(argv[1]);
   const char *rel = argv[2];

   if (strcmp(rel, "supersedes") != 0 && strcmp(rel, "depends_on") != 0 &&
       strcmp(rel, "contradicts") != 0 && strcmp(rel, "related_to") != 0)
      fatal("relation must be: supersedes, depends_on, contradicts, or related_to");

   if (kb_client_memory_link_create(src, tgt, rel) != 0)
      fatal("failed to create link");

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      printf("Linked memory %lld -[%s]-> %lld\n", (long long)src, rel, (long long)tgt);
}

void mem_mlinks(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("usage: aimee memory mlinks <id>");
   int64_t id = atoll(argv[0]);

   memory_link_t links[32];
   int count = kb_client_memory_link_query(id, links, 32);

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddNumberToObject(obj, "id", (double)links[i].id);
         cJSON_AddNumberToObject(obj, "source_id", (double)links[i].source_id);
         cJSON_AddNumberToObject(obj, "target_id", (double)links[i].target_id);
         cJSON_AddStringToObject(obj, "relation", links[i].relation);
         cJSON_AddStringToObject(obj, "created_at", links[i].created_at);
         cJSON_AddItemToArray(arr, obj);
      }
      char *json = cJSON_Print(arr);
      printf("%s\n", json);
      free(json);
      cJSON_Delete(arr);
   }
   else
   {
      if (count == 0)
      {
         printf("No links for memory %lld\n", (long long)id);
         return;
      }
      for (int i = 0; i < count; i++)
      {
         const char *dir = (links[i].source_id == id) ? "->" : "<-";
         int64_t other = (links[i].source_id == id) ? links[i].target_id : links[i].source_id;
         printf("  [%lld] %s [%s] %lld  (%s)\n", (long long)links[i].id, dir, links[i].relation,
                (long long)other, links[i].created_at);
      }
   }
}

void mem_munlink(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("usage: aimee memory munlink <link_id>");
   int64_t link_id = atoll(argv[0]);

   if (kb_client_memory_link_delete(link_id) != 0)
      fatal("failed to delete link %lld", (long long)link_id);

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      printf("Deleted link %lld\n", (long long)link_id);
}

void mem_tag(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   if (opts.pos_count < 1)
      fatal("usage: aimee memory tag <id> <workspace> | <id> --scope-type <type> --scope <value>");
   int64_t id = atoll(opts.positional[0]);
   const char *scope_type = cmd_memory_scope_type(&opts);
   const char *scope_value = cmd_memory_scope_value(&opts);
   if ((!scope_type || !scope_type[0]) && opts.pos_count >= 2)
   {
      scope_type = "workspace";
      scope_value = opts.positional[1];
   }
   if (!scope_type || !scope_type[0] || !scope_value || !scope_value[0])
      fatal("memory tag requires a scope value");
   cJSON *tag_args = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(tag_args);
   cJSON_AddNumberToObject(tag_args, "memory_id", (double)id);
   cJSON_AddStringToObject(tag_args, "scope_type", scope_type);
   cJSON_AddStringToObject(tag_args, "scope_value", scope_value);
   char *json = kb_v1_action_request("memory.tag_scope", tag_args);
   cJSON *reply = json ? cJSON_Parse(json) : NULL;
   free(json);
   int ok = strcmp(jo_cstr(reply, "status"), "ok") == 0;
   cJSON_Delete(reply);
   if (!ok)
      fatal("failed to tag memory %lld", (long long)id);
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      printf("Tagged memory %lld with %s '%s'\n", (long long)id, scope_type, scope_value);
}
