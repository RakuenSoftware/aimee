/* cmd_memory_core.c: CRUD + stats + task/decision + link/tag subcommand
 * handlers for `aimee memory`. Extracted from cmd_memory.c so each bucket
 * can be read in isolation. Shared helpers and globals live in
 * cmd_memory_internal.h; the subcommand table and dispatcher stay in
 * cmd_memory.c. */
#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "cmd_memory_internal.h"
#include "cmd_review.h"
#include "db2/memory_query.h"
#include "platform_process.h"
#include "tasks_compose.h"
#include "kb.h"
#include "kb_client.h"
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

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
   if (kb_client_memory_insert(tier, kind, key, content, 1.0, session, &mem) != 0)
      fatal("failed to store memory (key=%s, tier=%s) — check stderr for details", key, tier);

   /* Apply explicit workspace tag if provided */
   if (workspace && workspace[0])
      kb_client_memory_tag_workspace(mem.id, workspace);
   if (scope_type && scope_type[0] && scope_value && scope_value[0])
      kb_client_memory_tag_scope(mem.id, scope_type, scope_value);

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
      db2_memory_low_eff_row_t lrows[256];
      int n = kb_client_memory_list_low_effectiveness(EFFECTIVENESS_DEMOTE_THRESHOLD,
                                                      limit > 256 ? 256 : limit, lrows, 256);
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < n; i++)
      {
         cJSON *m = cJSON_CreateObject();
         jo_add_i64(m, "id", lrows[i].id);
         jo_add_str(m, "tier", lrows[i].tier);
         jo_add_str(m, "kind", lrows[i].kind);
         jo_add_str(m, "key", lrows[i].key);
         jo_add_num(m, "effectiveness", lrows[i].effectiveness);
         jo_add_i64(m, "use_count", lrows[i].use_count);
         cJSON_AddItemToArray(arr, m);
      }
      if (ctx->json_output)
         emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      else
         cJSON_Delete(arr);
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
   memory_stats_t explain_stats;
   memset(&explain_stats, 0, sizeof(explain_stats));
   if (explain)
   {
      explain_count = cmd_memory_diagnose_query(&opts, query_buf, limit, explain_rows, 64);
      cmd_memory_require_runtime(explain_count, "memory search --explain");
      kb_client_memory_stats(&explain_stats);
   }

   /* Search conversation windows */
   int max_results = limit < 64 ? limit : 64;
   search_result_t *results = calloc((size_t)max_results, sizeof(search_result_t));
   if (!results)
      fatal("out of memory");
   int win_count = kb_client_memory_search(clusters, cluster_count, limit, results, max_results);

   /* Optional as-of graph relations search */
   memory_relation_t as_of_rels[32];
   int as_of_count = 0;
   if (as_of && query_buf[0])
      as_of_count = kb_client_memory_search_graph_as_of(query_buf, as_of, limit < 32 ? limit : 32,
                                                        as_of_rels, 32);

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
         cJSON *rarr = cJSON_CreateArray();
         for (int i = 0; i < as_of_count; i++)
         {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "id", (double)as_of_rels[i].id);
            cJSON_AddStringToObject(r, "src_entity", as_of_rels[i].src_entity);
            cJSON_AddStringToObject(r, "relation", as_of_rels[i].relation);
            cJSON_AddStringToObject(r, "dst_entity", as_of_rels[i].dst_entity);
            cJSON_AddStringToObject(r, "valid_at", as_of_rels[i].valid_at);
            cJSON_AddStringToObject(r, "invalid_at", as_of_rels[i].invalid_at);
            cJSON_AddItemToArray(rarr, r);
         }
         cJSON_AddItemToObject(obj, "as_of_relations", rarr);
      }

      if (explain)
      {
         cJSON *timing = cJSON_CreateObject();
         jo_add_num(timing, "elapsed_ms", explain_stats.pagerank_last_ms);
         jo_add_num(timing, "avg_ms", explain_stats.pagerank_avg_ms);
         jo_add_num(timing, "max_ms", explain_stats.pagerank_max_ms);
         jo_add_num(timing, "samples", explain_stats.pagerank_samples);
         jo_add_num(timing, "candidates", explain_stats.pagerank_last_candidates);
         jo_add_num(timing, "edges", explain_stats.pagerank_last_edges);
         cJSON_AddItemToObject(obj, "pagerank_timing", timing);

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
      printf("PageRank: elapsed=%.3fms avg=%.3fms max=%.3fms samples=%d candidates=%d edges=%d\n",
             explain_stats.pagerank_last_ms, explain_stats.pagerank_avg_ms,
             explain_stats.pagerank_max_ms, explain_stats.pagerank_samples,
             explain_stats.pagerank_last_candidates, explain_stats.pagerank_last_edges);
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
         for (int i = 0; i < as_of_count; i++)
            printf("  %s -[%s]-> %s  (valid_at=%s)\n", as_of_rels[i].src_entity,
                   as_of_rels[i].relation, as_of_rels[i].dst_entity,
                   as_of_rels[i].valid_at[0] ? as_of_rels[i].valid_at : "(any)");
      }
   }
   else if (as_of_count > 0)
   {
      printf("Graph relations as-of %s:\n", as_of);
      for (int i = 0; i < as_of_count; i++)
         printf("  %s -[%s]-> %s  (valid_at=%s)\n", as_of_rels[i].src_entity,
                as_of_rels[i].relation, as_of_rels[i].dst_entity,
                as_of_rels[i].valid_at[0] ? as_of_rels[i].valid_at : "(any)");
   }
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
   memory_stats_t stats;
   kb_client_memory_stats(&stats);
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "total", stats.total);
      cJSON_AddNumberToObject(j, "conflicts", stats.conflicts);
      cJSON *tiers = cJSON_AddObjectToObject(j, "tiers");
      cJSON_AddNumberToObject(tiers, "L0", stats.tier_counts[0]);
      cJSON_AddNumberToObject(tiers, "L1", stats.tier_counts[1]);
      cJSON_AddNumberToObject(tiers, "L2", stats.tier_counts[2]);
      cJSON_AddNumberToObject(tiers, "L3", stats.tier_counts[3]);
      cJSON_AddNumberToObject(tiers, "L4", stats.tier_counts[4]);
      cJSON_AddNumberToObject(tiers, "L5", stats.tier_counts[5]);

      cJSON *pagerank = cJSON_AddObjectToObject(j, "pagerank");
      cJSON_AddNumberToObject(pagerank, "last_ms", stats.pagerank_last_ms);
      cJSON_AddNumberToObject(pagerank, "avg_ms", stats.pagerank_avg_ms);
      cJSON_AddNumberToObject(pagerank, "max_ms", stats.pagerank_max_ms);
      cJSON_AddNumberToObject(pagerank, "samples", stats.pagerank_samples);
      cJSON_AddNumberToObject(pagerank, "last_candidates", stats.pagerank_last_candidates);
      cJSON_AddNumberToObject(pagerank, "last_edges", stats.pagerank_last_edges);

      effectiveness_stats_t estats;
      if (kb_client_memory_effectiveness_stats(&estats) == 0)
      {
         cJSON *eff = cJSON_AddObjectToObject(j, "effectiveness");
         cJSON_AddNumberToObject(eff, "avg_effectiveness", estats.avg_effectiveness);
         cJSON_AddNumberToObject(eff, "low_effectiveness", estats.low_effectiveness_count);
         cJSON_AddNumberToObject(eff, "high_impact", estats.high_impact_count);
         cJSON_AddNumberToObject(eff, "never_surfaced_l2", estats.never_surfaced_l2);
      }

      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Memory Stats:\n");
      printf("  Total:              %d\n", stats.total);
      printf("  Conflicts:          %d\n", stats.conflicts);
      printf("  Tiers:              L0=%d L1=%d L2=%d L3=%d L4=%d L5=%d\n", stats.tier_counts[0],
             stats.tier_counts[1], stats.tier_counts[2], stats.tier_counts[3], stats.tier_counts[4],
             stats.tier_counts[5]);
      printf("  PageRank latency:   last=%.3fms avg=%.3fms max=%.3fms samples=%d candidates=%d "
             "edges=%d\n",
             stats.pagerank_last_ms, stats.pagerank_avg_ms, stats.pagerank_max_ms,
             stats.pagerank_samples, stats.pagerank_last_candidates, stats.pagerank_last_edges);
   }
}

void mem_scan(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   char dirs[8][MAX_PATH_LEN];
   int dir_count = config_conversation_dirs(&s_mem_cfg, dirs, 8);
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
   const char *embed_cmd = config_embedding_command(&s_mem_cfg, NULL);

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

   /* Legacy monolithic command path. Cognify crosses DB1 and DB2; the server
    * RPC port must split DB1 queue ownership from DB2 memory reads instead
    * of running this from a DB-owning client or auxiliary process. */
   memory_cognify_queue_stats_t cog_stats;
   memset(&cog_stats, 0, sizeof(cog_stats));
   if (s_mem_cfg.memory_cognify_enabled && s_mem_cfg.memory_cognify_command[0])
      (void)memory_cognify_drain(&s_mem_cfg, timeout, &cog_stats);

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "processed", stats.processed);
      cJSON_AddNumberToObject(j, "pending", stats.pending);
      cJSON_AddNumberToObject(j, "running", stats.running);
      cJSON_AddNumberToObject(j, "done", stats.done);
      cJSON_AddNumberToObject(j, "failed", stats.failed);
      cJSON_AddNumberToObject(j, "total", stats.total);
      cJSON *cog = cJSON_CreateObject();
      cJSON_AddNumberToObject(cog, "processed", cog_stats.processed);
      cJSON_AddNumberToObject(cog, "pending", cog_stats.pending);
      cJSON_AddNumberToObject(cog, "running", cog_stats.running);
      cJSON_AddNumberToObject(cog, "done", cog_stats.done);
      cJSON_AddNumberToObject(cog, "failed", cog_stats.failed);
      cJSON_AddNumberToObject(cog, "retried", cog_stats.retried);
      cJSON_AddItemToObject(j, "cognify", cog);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("Async memory queue drained: processed=%d pending=%d running=%d failed=%d\n",
          stats.processed, stats.pending, stats.running, stats.failed);
   if (s_mem_cfg.memory_cognify_enabled)
      printf("Cognify queue: processed=%d pending=%d failed=%d retried=%d\n", cog_stats.processed,
             cog_stats.pending, cog_stats.failed, cog_stats.retried);
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
   memory_health_t health;
   kb_client_memory_query_health(&health);
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "cycles", health.cycles);
      cJSON_AddNumberToObject(j, "contradiction_rate", health.contradiction_rate);
      cJSON_AddNumberToObject(j, "promotion_rate", health.promotion_rate);
      cJSON_AddNumberToObject(j, "demotion_rate", health.demotion_rate);
      cJSON_AddNumberToObject(j, "staleness", health.staleness);
      cJSON_AddNumberToObject(j, "total_contradictions", health.total_contradictions);
      cJSON_AddNumberToObject(j, "total_promotions", health.total_promotions);
      cJSON_AddNumberToObject(j, "total_demotions", health.total_demotions);
      cJSON_AddNumberToObject(j, "total_expirations", health.total_expirations);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Memory Health (last 7 days, %d cycles):\n", health.cycles);
      printf("  Contradiction rate: %.1f%% (%d detected)\n", health.contradiction_rate * 100,
             health.total_contradictions);
      printf("  Promotion rate:     %.1f%% (%d promoted)\n", health.promotion_rate * 100,
             health.total_promotions);
      printf("  Demotion rate:      %.1f%% (%d demoted)\n", health.demotion_rate * 100,
             health.total_demotions);
      printf("  Staleness:          %.1f%% of L2 facts unused in 30+ days\n",
             health.staleness * 100);
      printf("  Expirations:        %d\n", health.total_expirations);
   }
}

void mem_provenance(app_ctx_t *ctx, int argc, char **argv)
{
   /* --stale flag: show memories with suspicious provenance */
   if (argc >= 1 && strcmp(argv[0], "--stale") == 0)
   {
      db2_memory_unused_l2_row_t unused_rows[256];
      int unused_n = kb_client_memory_list_unused_l2(14, unused_rows, 256);
      db2_memory_superseded_row_t sup_rows[256];
      int sup_n = kb_client_memory_list_superseded_keys(3, sup_rows, 256);

      if (ctx->json_output)
      {
         cJSON *root = cJSON_CreateObject();
         cJSON *unused = cJSON_CreateArray();
         for (int i = 0; i < unused_n; i++)
         {
            cJSON *m = cJSON_CreateObject();
            jo_add_i64(m, "id", unused_rows[i].id);
            jo_add_str(m, "key", unused_rows[i].key);
            cJSON_AddItemToArray(unused, m);
         }
         cJSON_AddItemToObject(root, "never_used", unused);

         cJSON *superseded = cJSON_CreateArray();
         for (int i = 0; i < sup_n; i++)
         {
            cJSON *m = cJSON_CreateObject();
            jo_add_str(m, "base_key", sup_rows[i].base_key);
            jo_add_i64(m, "versions", sup_rows[i].versions);
            cJSON_AddItemToArray(superseded, m);
         }
         cJSON_AddItemToObject(root, "frequently_superseded", superseded);

         emit_json_ctx(root, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("Never-used L2 memories (>14 days old):\n");
         for (int i = 0; i < unused_n; i++)
            printf("  #%-6lld %s\n", (long long)unused_rows[i].id, unused_rows[i].key);
         if (unused_n == 0)
            printf("  (none)\n");

         printf("\nFrequently superseded keys (3+ versions):\n");
         for (int i = 0; i < sup_n; i++)
            printf("  %-40s %d versions\n", sup_rows[i].base_key, sup_rows[i].versions);
         if (sup_n == 0)
            printf("  (none)\n");
      }
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

   provenance_entry_t entries[MAX_PROVENANCE_ENTRIES];
   int count = kb_client_memory_get_provenance(id, entries, MAX_PROVENANCE_ENTRIES);

   if (ctx->json_output)
   {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddNumberToObject(root, "memory_id", (double)id);
      cJSON_AddStringToObject(root, "key", mem.key);
      cJSON_AddStringToObject(root, "tier", mem.tier);
      cJSON_AddStringToObject(root, "kind", mem.kind);
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *e = cJSON_CreateObject();
         cJSON_AddStringToObject(e, "created_at", entries[i].created_at);
         cJSON_AddStringToObject(e, "action", entries[i].action);
         cJSON_AddStringToObject(e, "session_id", entries[i].session_id);
         if (entries[i].details[0])
            cJSON_AddStringToObject(e, "details", entries[i].details);
         cJSON_AddItemToArray(arr, e);
      }
      cJSON_AddItemToObject(root, "provenance", arr);
      emit_json_ctx(root, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Memory #%lld: %s (%s, %s, confidence: %.2f)\n", (long long)id, mem.key, mem.tier,
             mem.kind, mem.confidence);
      if (count == 0)
      {
         printf("  (no provenance records)\n");
         return;
      }
      for (int i = 0; i < count; i++)
      {
         /* Show date part only (first 10 chars of ISO 8601) */
         char date[11] = {0};
         snprintf(date, sizeof(date), "%.10s", entries[i].created_at);
         printf("  %s  %-10s session:%.8s", date, entries[i].action, entries[i].session_id);
         if (entries[i].details[0])
            printf("  \"%s\"", entries[i].details);
         printf("\n");
      }
   }
}

static unsigned int mem_maintain_parse_modes(const char *csv)
{
   if (!csv || !csv[0])
      return 0;
   unsigned int modes = 0;
   const char *p = csv;
   while (*p)
   {
      while (*p == ' ' || *p == ',')
         p++;
      if (!*p)
         break;
      const char *start = p;
      while (*p && *p != ',' && *p != ' ')
         p++;
      size_t len = (size_t)(p - start);
      if (len == 6 && strncmp(start, "replay", 6) == 0)
         modes |= MEMORY_MAINTENANCE_MODE_REPLAY;
      else if (len == 7 && strncmp(start, "compact", 7) == 0)
         modes |= MEMORY_MAINTENANCE_MODE_COMPACT;
      else if (len == 5 && strncmp(start, "prune", 5) == 0)
         modes |= MEMORY_MAINTENANCE_MODE_PRUNE;
      else if (len == 9 && strncmp(start, "summarize", 9) == 0)
         modes |= MEMORY_MAINTENANCE_MODE_SUMMARIZE;
      else if (len == 5 && strncmp(start, "drift", 5) == 0)
         modes |= MEMORY_MAINTENANCE_MODE_DRIFT;
   }
   return modes;
}

void mem_maintain(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   unsigned int modes = mem_maintain_parse_modes(opt_get(&opts, "modes"));
   int dry_run = opt_get_flag(&opts, "dry-run");
   int force = opt_get_flag(&opts, "force");
   int watch_secs = opt_get_int(&opts, "watch", 0);

watch_iter:;
   /* Run maintenance inside the knowledge service and parse the
    * summary back out of the response envelope. */
   char *envelope = kb_client_memory_maintenance_run_json(modes, force, dry_run);
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   cJSON *summary_j = resp ? cJSON_GetObjectItemCaseSensitive(resp, "summary") : NULL;

   memory_maintenance_summary_t maint_summary;
   memset(&maint_summary, 0, sizeof(maint_summary));
   if (cJSON_IsObject(summary_j))
   {
#define PICK_INT(field)                                                                            \
   do                                                                                              \
   {                                                                                               \
      cJSON *v = cJSON_GetObjectItemCaseSensitive(summary_j, #field);                              \
      if (cJSON_IsNumber(v))                                                                       \
         maint_summary.field = (int)v->valuedouble;                                                \
   } while (0)
      PICK_INT(promoted);
      PICK_INT(demoted);
      PICK_INT(expired);
      PICK_INT(skipped);
      PICK_INT(dry_run);
      PICK_INT(lifecycle_archived);
      PICK_INT(merged);
      PICK_INT(rescored);
      PICK_INT(modes_run);
#undef PICK_INT
      cJSON *elapsed = cJSON_GetObjectItemCaseSensitive(summary_j, "elapsed_ms");
      if (cJSON_IsNumber(elapsed))
         maint_summary.elapsed_ms = elapsed->valuedouble;
   }

   int promoted = maint_summary.promoted;
   int demoted = maint_summary.demoted;
   int expired = maint_summary.expired;

   const int vector_maintenance_handoff = (!dry_run && modes == 0 && !maint_summary.skipped);

   if (ctx->json_output)
   {
      cJSON *j = summary_j ? cJSON_Duplicate(summary_j, 1) : cJSON_CreateObject();
      cJSON_AddStringToObject(j, "status", "ok");
      cJSON_AddStringToObject(j, "vector_maintenance_owner", "knowledge-service");
      cJSON_AddBoolToObject(j, "vector_maintenance_skipped_here", vector_maintenance_handoff);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (maint_summary.skipped)
         printf("Maintenance cycle skipped (idle guard).\n");
      else
         printf("Maintenance: promoted=%d demoted=%d expired=%d archived=%d merged=%d "
                "rescored=%d elapsed_ms=%.2f%s\n",
                promoted, demoted, expired, maint_summary.lifecycle_archived, maint_summary.merged,
                maint_summary.rescored, maint_summary.elapsed_ms, dry_run ? " (dry-run)" : "");
      if (vector_maintenance_handoff)
         printf("Vector maintenance skipped here; ownership belongs to the knowledge service.\n");
   }

   cJSON_Delete(resp);

   if (watch_secs > 0)
   {
      sleep((unsigned)watch_secs);
      goto watch_iter;
   }
}

void mem_briefing(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   int limit_tokens = opt_get_int(&opts, "limit-tokens", MEMORY_BRIEFING_DEFAULT_LIMIT_TOKENS);

   cJSON *bundle = kb_client_memory_briefing(limit_tokens);
   if (!bundle)
      fatal("memory briefing failed");

   if (ctx->json_output)
   {
      emit_json_ctx(bundle, ctx->json_fields, ctx->response_profile);
      return;
   }

   cJSON *key_facts = cJSON_GetObjectItemCaseSensitive(bundle, "key_facts");
   cJSON *recent = cJSON_GetObjectItemCaseSensitive(bundle, "recent_activity");
   cJSON *entities = cJSON_GetObjectItemCaseSensitive(bundle, "active_entities");

   printf("# Session Briefing\n\n");
   printf("## Key Facts (%d)\n", cJSON_GetArraySize(key_facts));
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, key_facts)
   {
      const char *tier = cJSON_GetStringValue(cJSON_GetObjectItem(it, "tier"));
      const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(it, "kind"));
      const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
      long long id = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "memory_id"));
      printf("  - [%s/%s #%lld] %s\n", tier ? tier : "", kind ? kind : "", id, text ? text : "");
   }

   printf("\n## Recent Activity (%d)\n", cJSON_GetArraySize(recent));
   cJSON_ArrayForEach(it, recent)
   {
      const char *sess = cJSON_GetStringValue(cJSON_GetObjectItem(it, "session_id"));
      const char *summary = cJSON_GetStringValue(cJSON_GetObjectItem(it, "summary"));
      printf("  - %s: %s\n", sess ? sess : "", summary ? summary : "");
   }

   printf("\n## Active Entities (%d)\n", cJSON_GetArraySize(entities));
   cJSON_ArrayForEach(it, entities)
   {
      const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
      int mentions = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "mentions"));
      printf("  - %s (mentions=%d)\n", name ? name : "", mentions);
   }

   cJSON *tok = cJSON_GetObjectItemCaseSensitive(bundle, "approx_tokens");
   cJSON *cap = cJSON_GetObjectItemCaseSensitive(bundle, "limit_tokens");
   printf("\napprox_tokens=%d / limit_tokens=%d\n", (int)cJSON_GetNumberValue(tok),
          (int)cJSON_GetNumberValue(cap));
   cJSON_Delete(bundle);
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
   char *envelope =
       kb_client_memory_prospective_create_json(when, doit, entity, file, recur, valid_until);
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
      char *envelope = kb_client_memory_prospective_complete_json(id);
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
      int n = kb_client_memory_prospective_sweep_expired();
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

   char *envelope = kb_client_memory_prospective_list_json(state, limit);
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

   char *envelope = kb_client_memory_alerts_json(since);
   cJSON *bundle = mem_prospective_detach_from_envelope(envelope, "alerts");
   free(envelope);
   if (!bundle)
      fatal("memory alerts failed");

   if (ctx->json_output)
   {
      emit_json_ctx(bundle, ctx->json_fields, ctx->response_profile);
      return;
   }

   cJSON *stale = cJSON_GetObjectItemCaseSensitive(bundle, "stale_pending");
   cJSON *conflicts = cJSON_GetObjectItemCaseSensitive(bundle, "unresolved_contradictions");
   cJSON *superseded = cJSON_GetObjectItemCaseSensitive(bundle, "newly_superseded");

   printf("# Memory Alerts\n\n");
   printf("## Stale Pending (%d)\n", cJSON_GetArraySize(stale));
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, stale)
   {
      long long id = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "memory_id"));
      double age = cJSON_GetNumberValue(cJSON_GetObjectItem(it, "age_days"));
      double window = cJSON_GetNumberValue(cJSON_GetObjectItem(it, "window_days"));
      const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
      printf("  - #%lld age=%.1fd/%.1fd: %s\n", id, age, window, text ? text : "");
   }
   printf("\n## Unresolved Contradictions (%d)\n", cJSON_GetArraySize(conflicts));
   cJSON_ArrayForEach(it, conflicts)
   {
      const char *topic = cJSON_GetStringValue(cJSON_GetObjectItem(it, "topic"));
      const char *a = cJSON_GetStringValue(cJSON_GetObjectItem(it, "a"));
      const char *b = cJSON_GetStringValue(cJSON_GetObjectItem(it, "b"));
      printf("  - %s\n    A: %s\n    B: %s\n", topic ? topic : "", a ? a : "", b ? b : "");
   }
   printf("\n## Newly Superseded (%d)\n", cJSON_GetArraySize(superseded));
   cJSON_ArrayForEach(it, superseded)
   {
      long long id = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "memory_id"));
      const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(it, "key"));
      const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
      const char *ts = cJSON_GetStringValue(cJSON_GetObjectItem(it, "superseded_at"));
      printf("  - #%lld [%s] %s (at %s)\n", id, key ? key : "", text ? text : "", ts ? ts : "");
   }
   double ms = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(bundle, "elapsed_ms"));
   printf("\nassembled in %.2fms\n", ms);
   cJSON_Delete(bundle);
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

   cJSON *resp = directive_rpc_unwrap(
       kb_client_memory_directive_create_json(question, topic, entity, file, cause, priority,
                                              session, valid_until),
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
   memory_directive_t row;
   if (!d || memory_directive_from_json(d, &row) != 0)
   {
      cJSON_Delete(resp);
      fatal("memory directive create: unexpected response");
   }

   if (ctx->json_output)
      emit_json_ctx(memory_directive_to_json(&row), ctx->json_fields, ctx->response_profile);
   else
      printf("Opened directive #%lld [%s, p%d]: %s\n", (long long)row.id, row.cause, row.priority,
             row.question);
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
      cJSON *resp =
          directive_rpc_unwrap(kb_client_memory_directive_resolve_json(id, with_memory, note), err);
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
      cJSON *resp = directive_rpc_unwrap(kb_client_memory_directive_suppress_json(id), err);
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
      cJSON *resp = directive_rpc_unwrap(kb_client_memory_directive_sweep_expired_json(),
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

   cJSON *resp = directive_rpc_unwrap(kb_client_memory_directive_list_json(state, cause, limit),
                                      "directive list failed");
   cJSON *arr_src = cJSON_GetObjectItemCaseSensitive(resp, "directives");
   int count = cJSON_IsArray(arr_src) ? cJSON_GetArraySize(arr_src) : 0;
   memory_directive_t rows[256];
   int n = 0;
   for (int i = 0; i < count && n < 256; i++)
   {
      cJSON *item = cJSON_GetArrayItem(arr_src, i);
      if (memory_directive_from_json(item, &rows[n]) == 0)
         n++;
   }
   cJSON_Delete(resp);

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < n; i++)
         cJSON_AddItemToArray(arr, memory_directive_to_json(&rows[i]));
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      return;
   }
   if (n == 0)
   {
      printf("No directives%s%s.\n", state && state[0] ? " in state " : "",
             state && state[0] ? state : "");
      return;
   }
   printf("%d directive(s):\n", n);
   for (int i = 0; i < n; i++)
   {
      printf("  #%lld [%s, %s, p%d] %s\n", (long long)rows[i].id, rows[i].state, rows[i].cause,
             rows[i].priority, rows[i].question);
      if (rows[i].topic[0])
         printf("        topic=%s surfaced=%d%s%s\n", rows[i].topic, rows[i].surfaced_count,
                rows[i].valid_until[0] ? " valid_until=" : "", rows[i].valid_until);
   }
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
   memory_t mems[64];
   int count = kb_client_memory_fact_history(argv[0], mems, 64);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
         cJSON_AddItemToArray(arr, memory_to_json(&mems[i]));
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
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
      tasks_checkpoint_restore(id, session);
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
   if (kb_client_memory_tag_scope(id, scope_type, scope_value) != 0)
      fatal("failed to tag memory %lld", (long long)id);
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      printf("Tagged memory %lld with %s '%s'\n", (long long)id, scope_type, scope_value);
}
