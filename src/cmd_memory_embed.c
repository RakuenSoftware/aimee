/* cmd_memory_embed.c: embed, reembed, diagnose, answer, reflect, audit,
 * calibrate, and benchmark subcommand handlers. Includes the
 * memory_score_parts_to_json helper (used by the search / explain paths
 * in cmd_memory_core.c too). */
#include "aimee.h"
#include "cmd_memory_internal.h"
#include "db1.h"
#include "db2/collab_rules.h"
#include "db2/memory_query.h"
#include "kb_client.h"
#include "platform_process.h"
#include "kb.h"
#include "memory_graph_fusion.h"
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
   const char *embed_cmd = s_mem_cfg.embedding_command[0] ? s_mem_cfg.embedding_command : "builtin";

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
      cJSON *resp = mem_rpc_unwrap(kb_client_memory_embed_json(0, single_id, NULL, embed_cmd),
                                   "memory embed failed");
      cJSON_Delete(resp);
      printf("Embedded memory %lld\n", (long long)single_id);
      return;
   }

   const char *embed_ver = s_mem_cfg.embedding_model[0] ? s_mem_cfg.embedding_model : embed_cmd;
   cJSON *resp = mem_rpc_unwrap(kb_client_memory_embed_json(1, 0, embed_ver, embed_cmd),
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
   int do_start = 0, do_status = 0, do_cutover = 0;
   const char *rollback_version = NULL;
   const char *target_version = NULL;

   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--start") == 0)
         do_start = 1;
      else if (strcmp(argv[i], "--status") == 0)
         do_status = 1;
      else if (strcmp(argv[i], "--cutover") == 0)
         do_cutover = 1;
      else if (strcmp(argv[i], "--rollback") == 0 && i + 1 < argc)
         rollback_version = argv[++i];
      else if (strcmp(argv[i], "--version") == 0 && i + 1 < argc)
         target_version = argv[++i];
   }

   if (!do_start && !do_status && !do_cutover && !rollback_version)
      fatal("usage: aimee memory reembed --start [--version <v>] | --status | --cutover | "
            "--rollback <version>");

   if (rollback_version)
   {
      cJSON *resp =
          mem_rpc_unwrap(kb_client_memory_reembed_rollback_json(rollback_version), "rollback");
      int count = 0, rebuilt = 0, rebuild_failed = 0;
      cJSON *n = cJSON_GetObjectItemCaseSensitive(resp, "embedding_count");
      if (cJSON_IsNumber(n))
         count = (int)n->valuedouble;
      n = cJSON_GetObjectItemCaseSensitive(resp, "rebuilt");
      if (cJSON_IsNumber(n))
         rebuilt = (int)n->valuedouble;
      n = cJSON_GetObjectItemCaseSensitive(resp, "rebuild_failed");
      if (cJSON_IsNumber(n))
         rebuild_failed = (int)n->valuedouble;
      cJSON_Delete(resp);
      printf("Active embedder set to '%s' (%d embeddings, %d vector points rebuilt, %d failed)\n",
             rollback_version, count, rebuilt, rebuild_failed);
      return;
   }

   if (do_status)
   {
      cJSON *resp = mem_rpc_unwrap(kb_client_memory_reembed_status_json(), "reembed status");
      cJSON *av = cJSON_GetObjectItemCaseSensitive(resp, "active_version");
      const char *active_ver = (cJSON_IsString(av) && av->valuestring) ? av->valuestring : "";
      printf("Active embedder version: %s\n", active_ver[0] ? active_ver : "(unset)");

      cJSON *job = cJSON_GetObjectItemCaseSensitive(resp, "job");
      if (job)
      {
         cJSON *tv = cJSON_GetObjectItemCaseSensitive(job, "target_version");
         cJSON *lj = cJSON_GetObjectItemCaseSensitive(job, "last_id");
         cJSON *tj = cJSON_GetObjectItemCaseSensitive(job, "total");
         cJSON *dj = cJSON_GetObjectItemCaseSensitive(job, "done");
         cJSON *sj = cJSON_GetObjectItemCaseSensitive(job, "started_at");
         cJSON *fj = cJSON_GetObjectItemCaseSensitive(job, "finished_at");
         printf("Job target: %s\n", cJSON_IsString(tv) ? tv->valuestring : "");
         printf("Progress:   %d / %d (last_id=%d)\n", cJSON_IsNumber(dj) ? (int)dj->valuedouble : 0,
                cJSON_IsNumber(tj) ? (int)tj->valuedouble : 0,
                cJSON_IsNumber(lj) ? (int)lj->valuedouble : 0);
         printf("Started:    %s\n", cJSON_IsString(sj) ? sj->valuestring : "");
         const char *finished = cJSON_IsString(fj) ? fj->valuestring : "";
         printf("Finished:   %s\n", (finished && finished[0]) ? finished : "(in progress)");
      }
      else
      {
         printf("No re-embed job recorded.\n");
      }
      cJSON_Delete(resp);
      return;
   }

   if (do_cutover)
   {
      cJSON *resp = mem_rpc_unwrap(kb_client_memory_reembed_cutover_json(), "reembed cutover");
      cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "version");
      const char *tv_buf = (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
      int done_count = 0, rebuilt = 0, rebuild_failed = 0;
      cJSON *n = cJSON_GetObjectItemCaseSensitive(resp, "done");
      if (cJSON_IsNumber(n))
         done_count = (int)n->valuedouble;
      n = cJSON_GetObjectItemCaseSensitive(resp, "rebuilt");
      if (cJSON_IsNumber(n))
         rebuilt = (int)n->valuedouble;
      n = cJSON_GetObjectItemCaseSensitive(resp, "rebuild_failed");
      if (cJSON_IsNumber(n))
         rebuild_failed = (int)n->valuedouble;
      printf("Cutover complete. Active embedder is now '%s' (%d embeddings, %d vector points "
             "rebuilt, %d failed).\n",
             tv_buf, done_count, rebuilt, rebuild_failed);
      cJSON_Delete(resp);
      return;
   }

   /* --start */
   const char *embed_cmd = s_mem_cfg.embedding_command[0] ? s_mem_cfg.embedding_command : "builtin";
   char ver_buf[256];
   if (target_version)
      snprintf(ver_buf, sizeof(ver_buf), "%s", target_version);
   else
   {
      config_t rcfg;
      config_load(&rcfg);
      if (rcfg.embedding_model[0])
         snprintf(ver_buf, sizeof(ver_buf), "%s", rcfg.embedding_model);
      else
         snprintf(ver_buf, sizeof(ver_buf), "%s", embed_cmd);
   }

   cJSON *resp =
       mem_rpc_unwrap(kb_client_memory_reembed_start_json(ver_buf, embed_cmd), "reembed --start");
   int success = 0, fail = 0, total = 0, resume = 0;
   cJSON *n = cJSON_GetObjectItemCaseSensitive(resp, "embedded");
   if (cJSON_IsNumber(n))
      success = (int)n->valuedouble;
   n = cJSON_GetObjectItemCaseSensitive(resp, "failed");
   if (cJSON_IsNumber(n))
      fail = (int)n->valuedouble;
   n = cJSON_GetObjectItemCaseSensitive(resp, "total");
   if (cJSON_IsNumber(n))
      total = (int)n->valuedouble;
   n = cJSON_GetObjectItemCaseSensitive(resp, "resume_last_id");
   if (cJSON_IsNumber(n))
      resume = (int)n->valuedouble;
   cJSON_Delete(resp);

   if (resume > 0)
      printf("Resumed re-embed for version '%s' from id %d (%d total memories).\n", ver_buf, resume,
             total);
   else
      printf("Started re-embed for version '%s' (%d memories).\n", ver_buf, total);

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "version", ver_buf);
      cJSON_AddNumberToObject(j, "embedded", success);
      cJSON_AddNumberToObject(j, "failed", fail);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Re-embed complete: %d embedded, %d failed (version '%s').\n"
             "Run 'aimee memory reembed --cutover' to activate this index.\n",
             success, fail, ver_buf);
   }
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
   if (parts->cross_encoder != 0.0)
      jo_add_num(j, "cross_encoder", parts->cross_encoder);
   if (parts->hybrid_total != 0.0 || parts->blended_total != 0.0)
   {
      jo_add_num(j, "hybrid_total", parts->hybrid_total);
      jo_add_num(j, "blended_total", parts->blended_total);
   }
   if (parts->rerank_mix > 0.0)
      jo_add_num(j, "rerank_mix", parts->rerank_mix);
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
      if (rows[i].parts.cross_encoder != 0.0)
         printf("    total=%.3f hybrid=%.3f cross_encoder=%.3f lexical=%.3f coverage=%.3f "
                "entity=%.3f temporal=%.3f evidence=%.3f semantic=%.3f state=%.3f intent=%.3f "
                "salience=%.3f surprise=%.3f pagerank=%.3f\n",
                rows[i].parts.total, rows[i].parts.total - rows[i].parts.cross_encoder,
                rows[i].parts.cross_encoder, rows[i].parts.lexical, rows[i].parts.coverage,
                rows[i].parts.entity, rows[i].parts.temporal, rows[i].parts.evidence,
                rows[i].parts.semantic, rows[i].parts.state, rows[i].parts.intent,
                rows[i].parts.salience, rows[i].parts.surprise, rows[i].parts.pagerank);
      else
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

   memory_answer_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = cmd_memory_ask(&opts, query_buf, limit, &result);
   if (rc < 0)
      fatal("%s", result.error[0] ? result.error : "memory ask failed");

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      jo_add_str(obj, "query", query_buf);
      jo_add_str(obj, "answer", result.answer);
      cJSON_AddNumberToObject(obj, "confidence", result.confidence);
      cJSON_AddBoolToObject(obj, "no_answer", result.no_answer);
      cJSON_AddBoolToObject(obj, "low_confidence", result.low_confidence);
      cJSON *trace = cJSON_AddObjectToObject(obj, "evidence_trace");
      if (trace)
      {
         cJSON_AddStringToObject(trace, "decision",
                                 memory_answer_evidence_decision_str(&result.evidence));
         cJSON_AddStringToObject(trace, "reason",
                                 memory_answer_evidence_reason_str(&result.evidence));
         cJSON *ids = cJSON_AddArrayToObject(trace, "candidate_ids");
         for (int i = 0; ids && i < result.evidence.candidate_id_count; i++)
            cJSON_AddItemToArray(ids, cJSON_CreateNumber((double)result.evidence.candidate_ids[i]));
         cJSON_AddNumberToObject(trace, "ranked_count", result.evidence.ranked_count);
         cJSON_AddNumberToObject(trace, "anchor_id", (double)result.evidence.anchor_id);
         cJSON_AddNumberToObject(trace, "anchor_rank", result.evidence.anchor_rank);
         cJSON_AddNumberToObject(trace, "topk_grounding", result.evidence.topk_grounding);
         cJSON_AddNumberToObject(trace, "anchor_coverage", result.evidence.anchor_coverage);
         cJSON_AddNumberToObject(trace, "cluster_coverage", result.evidence.cluster_coverage);
         cJSON_AddNumberToObject(trace, "threshold", result.evidence.threshold);
         cJSON_AddNumberToObject(trace, "chunk_floor", result.evidence.chunk_floor);
         cJSON_AddBoolToObject(trace, "structural", result.evidence.structural);
         cJSON_AddBoolToObject(trace, "exempt", result.evidence.exempt);
         cJSON_AddBoolToObject(trace, "trace_truncated", result.evidence.trace_truncated);
      }
      cJSON *citations = cJSON_AddArrayToObject(obj, "citations");
      for (int i = 0; i < result.citation_count; i++)
      {
         cJSON *citation = cJSON_CreateObject();
         cJSON_AddNumberToObject(citation, "memory_id", (double)result.citation_ids[i]);
         if (explain)
         {
            memory_t mem;
            memset(&mem, 0, sizeof(mem));
            if (kb_client_memory_get(result.citation_ids[i], &mem) == 0)
               cJSON_AddNumberToObject(citation, "effective_importance",
                                       memory_effective_importance(&mem, 0));
         }
         cJSON_AddItemToArray(citations, citation);
      }
      if (explain)
         cJSON_AddItemToObject(obj, "filter_contract", memory_filter_to_json(&filter));
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (result.no_answer)
         printf("No confident answer for \"%s\"\n", query_buf);
      else
         printf("%s\n", result.answer);
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
}

/* --- reflect ---
 *
 * Deep-reasoning command: retrieve across all visible scopes, detect
 * contradictions between memories with the same key, show cited results,
 * and optionally propose a draft L4 rule if the --draft-rule flag is set.
 *
 * Usage:
 *   aimee memory reflect <query> [--scope auto|global|workspace|project]
 *                                [--limit N] [--draft-rule] [--synthesize]
 *
 * --scope auto (default): uses memory_find_facts_visible, which
 *   evaluates all scope tags and ranks by visibility proximity.
 * --scope global|workspace|project: restricts to that scope via
 *   memory_find_facts_scoped.
 * --draft-rule: if ≥ REFLECT_RULE_THRESHOLD results are returned, propose
 *   a draft L4 rule summarising the recurring pattern for human approval.
 *   The draft rule is never activated automatically.
 * --synthesize: after retrieval, run an LLM synthesis pass that produces a
 *   cited narrative answer, a contradiction explanation, and a confidence
 *   score. Requires memory.cognify.enabled=true and a cognify command.
 */

#define REFLECT_MAX_RESULTS    32
#define REFLECT_RULE_THRESHOLD 5

typedef struct
{
   char narrative[2048];                 /* cited narrative answer */
   char contradiction_explanation[1024]; /* why contradictions exist */
   char rule_proposal[512];              /* proposed rule text, if any */
   double confidence;                    /* synthesis confidence [0,1] */
} reflect_synthesis_result_t;

/* Build a JSON payload for the cognify command describing the reflect task,
 * call it, and parse the synthesis response.  Returns 0 on success. */
static int reflect_call_synthesis_agent(const char *query, const memory_t *facts, int count,
                                        int conflict_a, int conflict_b, int nconflicts,
                                        const config_t *cfg, reflect_synthesis_result_t *out)
{
   (void)conflict_a;
   (void)conflict_b;
   memset(out, 0, sizeof(*out));
   out->confidence = 0.0;

   if (!cfg || !cfg->memory_cognify_enabled || !cfg->memory_cognify_command[0])
      return -1;

   /* Build memories array */
   cJSON *mems_arr = cJSON_CreateArray();
   if (!mems_arr)
      return -1;
   for (int i = 0; i < count; i++)
   {
      cJSON *m = cJSON_CreateObject();
      cJSON_AddNumberToObject(m, "id", (double)facts[i].id);
      cJSON_AddStringToObject(m, "key", facts[i].key);
      cJSON_AddStringToObject(m, "content", facts[i].content);
      cJSON_AddStringToObject(m, "tier", facts[i].tier);
      cJSON_AddStringToObject(m, "kind", facts[i].kind);
      cJSON_AddNumberToObject(m, "confidence", facts[i].confidence);
      cJSON_AddItemToArray(mems_arr, m);
   }

   cJSON *input = cJSON_CreateObject();
   if (!input)
   {
      cJSON_Delete(mems_arr);
      return -1;
   }
   cJSON_AddStringToObject(input, "task", "reflect_synthesis");
   cJSON_AddStringToObject(input, "query", query);
   cJSON_AddItemToObject(input, "memories", mems_arr);
   cJSON_AddNumberToObject(input, "nconflicts", (double)nconflicts);
   cJSON_AddStringToObject(
       input, "instruction",
       "Produce a cited narrative answer synthesizing the retrieved memories. "
       "For any contradictions, explain why they might exist (different sessions, "
       "projects, or outdated data). Return JSON with fields: "
       "narrative (string, cite memory IDs as [#N]), "
       "contradiction_explanation (string, empty if none), "
       "rule_proposal (string, proposed standing rule if pattern is clear, else empty), "
       "confidence (number 0-1).");

   char *input_str = cJSON_PrintUnformatted(input);
   cJSON_Delete(input);
   if (!input_str)
      return -1;

   char *resp = NULL;
   size_t resp_len = 0;
   int rc = platform_exec_pipe(cfg->memory_cognify_command, input_str, strlen(input_str), &resp,
                               &resp_len);
   free(input_str);
   if (rc != 0 || !resp || resp_len == 0)
   {
      free(resp);
      return -1;
   }

   cJSON *j = cJSON_Parse(resp);
   free(resp);
   if (!j)
      return -1;

   cJSON *narr = cJSON_GetObjectItemCaseSensitive(j, "narrative");
   cJSON *expl = cJSON_GetObjectItemCaseSensitive(j, "contradiction_explanation");
   cJSON *rule = cJSON_GetObjectItemCaseSensitive(j, "rule_proposal");
   cJSON *conf = cJSON_GetObjectItemCaseSensitive(j, "confidence");

   if (cJSON_IsString(narr) && narr->valuestring[0])
      snprintf(out->narrative, sizeof(out->narrative), "%s", narr->valuestring);
   if (cJSON_IsString(expl) && expl->valuestring[0])
      snprintf(out->contradiction_explanation, sizeof(out->contradiction_explanation), "%s",
               expl->valuestring);
   if (cJSON_IsString(rule) && rule->valuestring[0])
      snprintf(out->rule_proposal, sizeof(out->rule_proposal), "%s", rule->valuestring);
   if (cJSON_IsNumber(conf))
      out->confidence = conf->valuedouble;

   cJSON_Delete(j);
   return out->narrative[0] ? 0 : -1;
}

void mem_reflect(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("usage: aimee memory reflect <query> [--scope auto|global|workspace|project] "
            "[--limit N] [--draft-rule] [--synthesize]");

   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   cmd_memory_apply_rerank_mode(&opts);

   int limit = opt_get_int(&opts, "limit", 10);
   if (limit < 1 || limit > REFLECT_MAX_RESULTS)
      limit = 10;

   int want_draft_rule = opt_get_flag(&opts, "draft-rule");
   int want_synthesize = opt_get_flag(&opts, "synthesize");

   /* Build query string from positional args */
   char query[2048];
   int qpos = 0;
   query[0] = '\0';
   for (int i = 0; i < opts.pos_count; i++)
   {
      if (i > 0)
         qpos = str_appendf(query, qpos, (int)sizeof(query), " ");
      qpos = str_appendf(query, qpos, (int)sizeof(query), "%s", opts.positional[i]);
   }
   if (!query[0])
      fatal("memory reflect requires query terms");

   /* Determine scope */
   const char *scope_str = opt_get(&opts, "scope");
   if (!scope_str || strcmp(scope_str, "auto") == 0)
      scope_str = NULL; /* auto → visible */

   /* Retrieve */
   memory_t facts[REFLECT_MAX_RESULTS];
   int count;

   if (scope_str == NULL)
   {
      /* Auto: use workspace/project from config if available, else NULL */
      const char *workspace = (s_mem_cfg.workspace_count > 0) ? s_mem_cfg.workspaces[0] : NULL;
      count = kb_client_memory_find_facts_visible(query, workspace, NULL, limit, facts,
                                                  REFLECT_MAX_RESULTS);
   }
   else
   {
      /* Explicit scope: treat scope value from --scope-value or fall back to the scope name itself
       */
      const char *scope_value = cmd_memory_scope_value(&opts);
      if (!scope_value || !scope_value[0])
         scope_value = scope_str;
      count = kb_client_memory_find_facts_scoped(query, scope_str, scope_value, limit, facts,
                                                 REFLECT_MAX_RESULTS);
   }
   cmd_memory_require_runtime(count, "memory reflect");

   /* Detect contradictions: memories with the same key but different content */
   typedef struct
   {
      int a;
      int b;
   } conflict_pair_t;
   conflict_pair_t conflicts[16];
   int nconflicts = 0;

   for (int i = 0; i < count && nconflicts < 16; i++)
      for (int j = i + 1; j < count && nconflicts < 16; j++)
         if (facts[i].key[0] && strcmp(facts[i].key, facts[j].key) == 0 &&
             strcmp(facts[i].content, facts[j].content) != 0)
         {
            conflicts[nconflicts].a = i;
            conflicts[nconflicts].b = j;
            nconflicts++;
         }

   /* Draft-rule: propose if enough evidence and flag is set */
   int rule_id = -1;
   if (want_draft_rule && count >= REFLECT_RULE_THRESHOLD)
   {
      char rule_text[COLLAB_RULE_TEXT_LEN + 1];
      char rule_reason[COLLAB_RULE_REASON_LEN + 1];
      snprintf(rule_text, sizeof(rule_text), "Recurring pattern detected for: %.120s", query);
      snprintf(rule_reason, sizeof(rule_reason),
               "reflect found %d memories matching \"%.*s\" — pattern may warrant a standing rule",
               count, 180, query);
      rule_id = kb_client_collab_rules_propose(rule_text, rule_reason, "reflect");
   }

   /* LLM synthesis pass (--synthesize) */
   reflect_synthesis_result_t synthesis;
   int have_synthesis = 0;
   if (want_synthesize)
   {
      config_t syn_cfg;
      if (config_load(&syn_cfg) == 0)
      {
         int ca = nconflicts > 0 ? conflicts[0].a : -1;
         int cb = nconflicts > 0 ? conflicts[0].b : -1;
         if (reflect_call_synthesis_agent(query, facts, count, ca, cb, nconflicts, &syn_cfg,
                                          &synthesis) == 0)
         {
            have_synthesis = 1;
            /* If synthesis produced a rule proposal and --draft-rule was set but
             * threshold not yet met, propose it now */
            if (want_draft_rule && rule_id < 0 && synthesis.rule_proposal[0])
            {
               char reason[COLLAB_RULE_REASON_LEN + 1];
               snprintf(reason, sizeof(reason), "reflect --synthesize proposed rule for: %.180s",
                        query);
               rule_id = kb_client_collab_rules_propose(synthesis.rule_proposal, reason, "reflect");
            }
         }
      }
   }

   /* Output */
   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      jo_add_str(obj, "query", query);
      cJSON *results_arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *entry = cJSON_CreateObject();
         cJSON_AddNumberToObject(entry, "id", (double)facts[i].id);
         cJSON_AddStringToObject(entry, "tier", facts[i].tier);
         cJSON_AddStringToObject(entry, "kind", facts[i].kind);
         cJSON_AddStringToObject(entry, "key", facts[i].key);
         cJSON_AddStringToObject(entry, "content", facts[i].content);
         cJSON_AddNumberToObject(entry, "confidence", facts[i].confidence);
         cJSON_AddItemToArray(results_arr, entry);
      }
      cJSON_AddItemToObject(obj, "results", results_arr);

      cJSON *confl_arr = cJSON_CreateArray();
      for (int k = 0; k < nconflicts; k++)
      {
         cJSON *c = cJSON_CreateObject();
         cJSON_AddNumberToObject(c, "id_a", (double)facts[conflicts[k].a].id);
         cJSON_AddNumberToObject(c, "id_b", (double)facts[conflicts[k].b].id);
         cJSON_AddStringToObject(c, "key", facts[conflicts[k].a].key);
         cJSON_AddItemToArray(confl_arr, c);
      }
      cJSON_AddItemToObject(obj, "contradictions", confl_arr);

      if (rule_id > 0)
         cJSON_AddNumberToObject(obj, "draft_rule_id", (double)rule_id);

      if (have_synthesis)
      {
         cJSON *syn = cJSON_CreateObject();
         cJSON_AddStringToObject(syn, "narrative", synthesis.narrative);
         cJSON_AddStringToObject(syn, "contradiction_explanation",
                                 synthesis.contradiction_explanation);
         cJSON_AddStringToObject(syn, "rule_proposal", synthesis.rule_proposal);
         cJSON_AddNumberToObject(syn, "confidence", synthesis.confidence);
         cJSON_AddItemToObject(obj, "synthesis", syn);
      }

      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      return;
   }

   /* Human-readable output */
   printf("Reflect: %s\n", query);
   printf("Found %d relevant %s\n\n", count, count == 1 ? "memory" : "memories");

   for (int i = 0; i < count; i++)
   {
      printf("[%d] #%lld  tier=%s  kind=%s\n", i + 1, (long long)facts[i].id, facts[i].tier,
             facts[i].kind);
      printf("    Key: %s\n", facts[i].key[0] ? facts[i].key : "(none)");
      printf("    %s\n\n", facts[i].content);
   }

   if (nconflicts > 0)
   {
      printf("CONTRADICTIONS DETECTED (%d)\n", nconflicts);
      for (int k = 0; k < nconflicts; k++)
      {
         int a = conflicts[k].a, b = conflicts[k].b;
         printf("  Key \"%s\" has conflicting entries:\n", facts[a].key);
         printf("    [#%lld] %s\n", (long long)facts[a].id, facts[a].content);
         printf("    [#%lld] %s\n\n", (long long)facts[b].id, facts[b].content);
      }
   }

   if (have_synthesis)
   {
      printf("\nSYNTHESIS (confidence=%.2f)\n%s\n", synthesis.confidence, synthesis.narrative);
      if (synthesis.contradiction_explanation[0])
         printf("\nContradiction explanation: %s\n", synthesis.contradiction_explanation);
      if (synthesis.rule_proposal[0] && rule_id > 0)
         printf("Draft rule proposed (id=%d) from synthesis — use `aimee rules approve %d` to "
                "activate.\n",
                rule_id, rule_id);
   }

   if (rule_id > 0 && !have_synthesis)
      printf("Draft rule proposed (id=%d) — use `aimee rules approve %d` to activate.\n", rule_id,
             rule_id);
   else if (want_draft_rule && count < REFLECT_RULE_THRESHOLD && !have_synthesis)
      printf("(--draft-rule: fewer than %d results, no rule proposed)\n", REFLECT_RULE_THRESHOLD);
}

typedef struct
{
   char query[1024];
   int64_t expected_id;
} memory_label_case_t;

typedef struct
{
   double lexical;
   double coverage;
   double entity;
   double temporal;
   double evidence;
   double semantic;
   double state;
   double intent;
   double confidence;
} memory_multiplier_profile_t;

static memory_multiplier_profile_t memory_multiplier_profile_default(void)
{
   memory_multiplier_profile_t p = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
   return p;
}

static int memory_load_label_cases(const char *path, memory_label_case_t *cases, int max_cases)
{
   FILE *fp = fopen(path, "r");
   if (!fp)
      fatal("failed to open labels file: %s", path);

   int count = 0;
   char line[4096];
   while (fgets(line, sizeof(line), fp) && count < max_cases)
   {
      char *p = line;
      while (*p && isspace((unsigned char)*p))
         p++;
      if (!*p || *p == '#')
         continue;
      char *tab = strchr(p, '\t');
      if (!tab)
         continue;
      *tab = '\0';
      char *id_str = tab + 1;
      char *nl = strchr(id_str, '\n');
      if (nl)
         *nl = '\0';
      while (*id_str && isspace((unsigned char)*id_str))
         id_str++;
      if (!p[0] || !id_str[0])
         continue;
      snprintf(cases[count].query, sizeof(cases[count].query), "%s", p);
      cases[count].expected_id = atoll(id_str);
      if (cases[count].expected_id > 0)
         count++;
   }
   fclose(fp);
   return count;
}

static double memory_rescore_parts(const memory_score_parts_t *parts,
                                   const memory_multiplier_profile_t *profile)
{
   return parts->lexical * profile->lexical + parts->coverage * profile->coverage +
          parts->entity * profile->entity + parts->temporal * profile->temporal +
          parts->evidence * profile->evidence + parts->semantic * profile->semantic +
          parts->state * profile->state + parts->intent * profile->intent + parts->salience +
          parts->surprise + parts->pagerank + parts->confidence * profile->confidence;
}

static void memory_sort_diagnostics(memory_diagnostic_t *rows, int count,
                                    const memory_multiplier_profile_t *profile)
{
   for (int i = 0; i < count; i++)
      rows[i].parts.total = memory_rescore_parts(&rows[i].parts, profile);
   for (int i = 0; i < count; i++)
   {
      for (int j = i + 1; j < count; j++)
      {
         if (rows[j].parts.total > rows[i].parts.total)
         {
            memory_diagnostic_t tmp = rows[i];
            rows[i] = rows[j];
            rows[j] = tmp;
         }
      }
   }
}

static int memory_query_has_temporal_intent(const char *query)
{
   if (!query || !query[0])
      return 0;
   return strstr(query, "when") != NULL || strstr(query, "date") != NULL ||
          strstr(query, "before") != NULL || strstr(query, "after") != NULL ||
          strstr(query, "year") != NULL || strstr(query, "month") != NULL ||
          strstr(query, "today") != NULL || strstr(query, "yesterday") != NULL;
}

static const char *memory_bucket_failure(const char *query, const memory_diagnostic_t *expected)
{
   if (memory_query_has_temporal_intent(query) && expected->parts.temporal <= 0.0 &&
       (strstr(expected->memory.content, "20") || strstr(expected->memory.content, "Jan") ||
        strstr(expected->memory.content, "Feb") || strstr(expected->memory.content, "Mar") ||
        strstr(expected->memory.content, "Apr") || strstr(expected->memory.content, "May") ||
        strstr(expected->memory.content, "Jun") || strstr(expected->memory.content, "Jul") ||
        strstr(expected->memory.content, "Aug") || strstr(expected->memory.content, "Sep") ||
        strstr(expected->memory.content, "Oct") || strstr(expected->memory.content, "Nov") ||
        strstr(expected->memory.content, "Dec")))
      return "temporal_miss";
   if (expected->parts.entity <= 0.0)
      return "entity_miss";
   if (expected->parts.lexical <= 0.0 && expected->parts.semantic > 0.0)
      return "lexical_gap";
   if (expected->parts.semantic <= 0.0 && expected->parts.lexical < 1.5)
      return "semantic_gap";
   if (expected->parts.state < -0.1)
      return "state_penalty";
   if (expected->parts.coverage < 0.5)
      return "granularity_gap";
   return "ranking_gap";
}

static int memory_collect_candidates(const char *query, int candidate_limit, int64_t expected_id,
                                     memory_diagnostic_t *rows, int max_rows)
{
   int count = kb_client_memory_diagnose(query, candidate_limit, rows, max_rows);
   int have_expected = 0;
   for (int i = 0; i < count; i++)
   {
      if (rows[i].memory.id == expected_id)
      {
         have_expected = 1;
         break;
      }
   }
   if (!have_expected && expected_id > 0 && count < max_rows &&
       kb_client_memory_explain_match(query, expected_id, &rows[count]) == 0)
      count++;
   return count;
}

static int memory_find_expected_rank(memory_diagnostic_t *rows, int count,
                                     const memory_multiplier_profile_t *profile,
                                     int64_t expected_id)
{
   memory_sort_diagnostics(rows, count, profile);
   for (int i = 0; i < count; i++)
   {
      if (rows[i].memory.id == expected_id)
         return i + 1;
   }
   return 0;
}

static double memory_eval_labeled_cases(const memory_label_case_t *cases, int case_count, int limit,
                                        int candidate_limit,
                                        const memory_multiplier_profile_t *profile, int *hits_at_k)
{
   double mrr = 0.0;
   if (hits_at_k)
      *hits_at_k = 0;
   for (int i = 0; i < case_count; i++)
   {
      memory_diagnostic_t rows[40];
      int count = memory_collect_candidates(cases[i].query, candidate_limit, cases[i].expected_id,
                                            rows, 40);
      int rank = memory_find_expected_rank(rows, count, profile, cases[i].expected_id);
      if (rank > 0)
      {
         mrr += 1.0 / (double)rank;
         if (hits_at_k && rank <= limit)
            (*hits_at_k)++;
      }
   }
   return case_count > 0 ? mrr / (double)case_count : 0.0;
}

static void memory_write_profile_file(const char *path, const memory_multiplier_profile_t *profile)
{
   const char *profile_path = getenv("AIMEE_MEMORY_WEIGHT_PROFILE");
   double lexical_key = 2.0, lexical_content = 1.0, long_token = 0.5, coverage = 3.0;
   double entity = 1.0, temporal = 1.0, evidence = 1.0, semantic = 1.2, state = 1.0;
   double workflow_intent = 1.2, entity_intent = 0.5, temporal_intent = 0.6, confidence = 0.05;

   if (profile_path && profile_path[0])
   {
      FILE *in = fopen(profile_path, "r");
      if (in)
      {
         char line[256];
         while (fgets(line, sizeof(line), in))
         {
            char name[128];
            double value = 0.0;
            if (sscanf(line, " %127[^=]=%lf", name, &value) != 2)
               continue;
            if (strcmp(name, "lexical_key") == 0)
               lexical_key = value;
            else if (strcmp(name, "lexical_content") == 0)
               lexical_content = value;
            else if (strcmp(name, "long_token") == 0)
               long_token = value;
            else if (strcmp(name, "coverage") == 0)
               coverage = value;
            else if (strcmp(name, "entity") == 0)
               entity = value;
            else if (strcmp(name, "temporal") == 0)
               temporal = value;
            else if (strcmp(name, "evidence") == 0)
               evidence = value;
            else if (strcmp(name, "semantic") == 0)
               semantic = value;
            else if (strcmp(name, "state") == 0)
               state = value;
            else if (strcmp(name, "workflow_intent") == 0)
               workflow_intent = value;
            else if (strcmp(name, "entity_intent") == 0)
               entity_intent = value;
            else if (strcmp(name, "temporal_intent") == 0)
               temporal_intent = value;
            else if (strcmp(name, "confidence") == 0)
               confidence = value;
         }
         fclose(in);
      }
   }

   lexical_key = cmd_memory_env_weight("AIMEE_MEMORY_WEIGHT_LEXICAL_KEY", lexical_key);
   lexical_content = cmd_memory_env_weight("AIMEE_MEMORY_WEIGHT_LEXICAL_CONTENT", lexical_content);
   long_token = cmd_memory_env_weight("AIMEE_MEMORY_WEIGHT_LONG_TOKEN", long_token);
   coverage = cmd_memory_env_weight("AIMEE_MEMORY_WEIGHT_COVERAGE", coverage);
   entity = cmd_memory_env_weight("AIMEE_MEMORY_WEIGHT_ENTITY", entity);
   temporal = cmd_memory_env_weight("AIMEE_MEMORY_WEIGHT_TEMPORAL", temporal);
   evidence = cmd_memory_env_weight("AIMEE_MEMORY_WEIGHT_EVIDENCE", evidence);
   semantic = cmd_memory_env_weight("AIMEE_MEMORY_WEIGHT_SEMANTIC", semantic);
   state = cmd_memory_env_weight("AIMEE_MEMORY_WEIGHT_STATE", state);
   workflow_intent = cmd_memory_env_weight("AIMEE_MEMORY_WEIGHT_WORKFLOW_INTENT", workflow_intent);
   entity_intent = cmd_memory_env_weight("AIMEE_MEMORY_WEIGHT_ENTITY_INTENT", entity_intent);
   temporal_intent = cmd_memory_env_weight("AIMEE_MEMORY_WEIGHT_TEMPORAL_INTENT", temporal_intent);
   confidence = cmd_memory_env_weight("AIMEE_MEMORY_WEIGHT_CONFIDENCE", confidence);

   FILE *out = fopen(path, "w");
   if (!out)
      fatal("failed to write profile: %s", path);
   fprintf(out, "lexical_key=%.6f\n", lexical_key * profile->lexical);
   fprintf(out, "lexical_content=%.6f\n", lexical_content * profile->lexical);
   fprintf(out, "long_token=%.6f\n", long_token * profile->lexical);
   fprintf(out, "coverage=%.6f\n", coverage * profile->coverage);
   fprintf(out, "entity=%.6f\n", entity * profile->entity);
   fprintf(out, "temporal=%.6f\n", temporal * profile->temporal);
   fprintf(out, "evidence=%.6f\n", evidence * profile->evidence);
   fprintf(out, "semantic=%.6f\n", semantic * profile->semantic);
   fprintf(out, "state=%.6f\n", state * profile->state);
   fprintf(out, "workflow_intent=%.6f\n", workflow_intent * profile->intent);
   fprintf(out, "entity_intent=%.6f\n", entity_intent * profile->intent);
   fprintf(out, "temporal_intent=%.6f\n", temporal_intent * profile->intent);
   fprintf(out, "confidence=%.6f\n", confidence * profile->confidence);
   fclose(out);
}

void mem_audit(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *labels = opt_get(&opts, "labels");
   int limit = opt_get_int(&opts, "limit", 10);
   int candidate_limit = opt_get_int(&opts, "candidate-limit", 24);
   if (!labels)
      fatal("memory audit requires --labels <query<TAB>memory_id file>");

   memory_label_case_t cases[4096];
   int case_count = memory_load_label_cases(labels, cases, 4096);
   if (case_count <= 0)
      fatal("no valid label cases in %s", labels);

   memory_multiplier_profile_t profile = memory_multiplier_profile_default();
   double mrr = 0.0;
   int hits = 0;
   int temporal_miss = 0, entity_miss = 0, lexical_gap = 0, semantic_gap = 0;
   int state_penalty = 0, granularity_gap = 0, ranking_gap = 0, missing = 0;

   for (int i = 0; i < case_count; i++)
   {
      memory_diagnostic_t rows[40];
      int count = memory_collect_candidates(cases[i].query, candidate_limit, cases[i].expected_id,
                                            rows, 40);
      int rank = memory_find_expected_rank(rows, count, &profile, cases[i].expected_id);
      if (rank > 0)
      {
         mrr += 1.0 / (double)rank;
         if (rank <= limit)
            hits++;
      }
      else
      {
         missing++;
      }

      memory_diagnostic_t expected;
      if (kb_client_memory_explain_match(cases[i].query, cases[i].expected_id, &expected) == 0)
      {
         const char *bucket = memory_bucket_failure(cases[i].query, &expected);
         if (strcmp(bucket, "temporal_miss") == 0)
            temporal_miss++;
         else if (strcmp(bucket, "entity_miss") == 0)
            entity_miss++;
         else if (strcmp(bucket, "lexical_gap") == 0)
            lexical_gap++;
         else if (strcmp(bucket, "semantic_gap") == 0)
            semantic_gap++;
         else if (strcmp(bucket, "state_penalty") == 0)
            state_penalty++;
         else if (strcmp(bucket, "granularity_gap") == 0)
            granularity_gap++;
         else
            ranking_gap++;
      }
   }

   mrr /= (double)case_count;
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      jo_add_str(j, "labels", labels);
      jo_add_num(j, "mrr", mrr);
      jo_add_num(j, "recall_at_k", (double)hits / (double)case_count);
      jo_add_i64(j, "case_count", case_count);
      cJSON *b = cJSON_CreateObject();
      jo_add_i64(b, "temporal_miss", temporal_miss);
      jo_add_i64(b, "entity_miss", entity_miss);
      jo_add_i64(b, "lexical_gap", lexical_gap);
      jo_add_i64(b, "semantic_gap", semantic_gap);
      jo_add_i64(b, "state_penalty", state_penalty);
      jo_add_i64(b, "granularity_gap", granularity_gap);
      jo_add_i64(b, "ranking_gap", ranking_gap);
      jo_add_i64(b, "missing", missing);
      cJSON_AddItemToObject(j, "buckets", b);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("Audit labels: %s\n", labels);
   printf("Cases: %d\n", case_count);
   printf("MRR: %.4f\n", mrr);
   printf("Recall@%d: %.4f\n", limit, (double)hits / (double)case_count);
   printf("Miss buckets: temporal=%d entity=%d lexical=%d semantic=%d state=%d granularity=%d "
          "ranking=%d missing=%d\n",
          temporal_miss, entity_miss, lexical_gap, semantic_gap, state_penalty, granularity_gap,
          ranking_gap, missing);
}

void mem_calibrate(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *labels = opt_get(&opts, "labels");
   const char *write_path = opt_get(&opts, "write");
   int apply_config = opt_get_flag(&opts, "apply-config");
   int limit = opt_get_int(&opts, "limit", 10);
   int candidate_limit = opt_get_int(&opts, "candidate-limit", 24);
   int rounds = opt_get_int(&opts, "rounds", 2);
   if (!labels)
      fatal("memory calibrate requires --labels <query<TAB>memory_id file>");

   memory_label_case_t cases[4096];
   int case_count = memory_load_label_cases(labels, cases, 4096);
   if (case_count <= 0)
      fatal("no valid label cases in %s", labels);

   memory_multiplier_profile_t best = memory_multiplier_profile_default();
   double best_mrr =
       memory_eval_labeled_cases(cases, case_count, limit, candidate_limit, &best, NULL);
   double candidates[] = {0.50, 0.75, 1.00, 1.25, 1.50, 1.75, 2.00};

   for (int round = 0; round < rounds; round++)
   {
      double *fields[] = {&best.lexical,  &best.coverage, &best.entity,
                          &best.temporal, &best.evidence, &best.semantic,
                          &best.state,    &best.intent,   &best.confidence};
      int field_count = (int)(sizeof(fields) / sizeof(fields[0]));
      for (int f = 0; f < field_count; f++)
      {
         double original = *fields[f];
         double local_best_value = original;
         double local_best_mrr = best_mrr;
         for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
         {
            *fields[f] = candidates[i];
            double trial =
                memory_eval_labeled_cases(cases, case_count, limit, candidate_limit, &best, NULL);
            if (trial > local_best_mrr + 1e-9)
            {
               local_best_mrr = trial;
               local_best_value = candidates[i];
            }
         }
         *fields[f] = local_best_value;
         best_mrr = local_best_mrr;
      }
   }

   int hits = 0;
   double final_mrr =
       memory_eval_labeled_cases(cases, case_count, limit, candidate_limit, &best, &hits);
   if (write_path && write_path[0])
   {
      memory_write_profile_file(write_path, &best);
      if (apply_config)
      {
         config_t cfg;
         if (config_load(&cfg) != 0)
            fatal("failed to load config for apply-config");
         snprintf(cfg.memory_weight_profile, sizeof(cfg.memory_weight_profile), "%s", write_path);
         if (config_save(&cfg) != 0)
            fatal("failed to save config for apply-config");
      }
   }
   else if (apply_config)
   {
      fatal("--apply-config requires --write <profile>");
   }

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      jo_add_str(j, "labels", labels);
      jo_add_num(j, "mrr", final_mrr);
      jo_add_num(j, "recall_at_k", (double)hits / (double)case_count);
      if (write_path && write_path[0])
         jo_add_str(j, "profile_path", write_path);
      jo_add_bool(j, "applied_config", apply_config && write_path && write_path[0]);
      cJSON *p = cJSON_CreateObject();
      jo_add_num(p, "lexical", best.lexical);
      jo_add_num(p, "coverage", best.coverage);
      jo_add_num(p, "entity", best.entity);
      jo_add_num(p, "temporal", best.temporal);
      jo_add_num(p, "evidence", best.evidence);
      jo_add_num(p, "semantic", best.semantic);
      jo_add_num(p, "state", best.state);
      jo_add_num(p, "intent", best.intent);
      jo_add_num(p, "confidence", best.confidence);
      cJSON_AddItemToObject(j, "multipliers", p);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("Calibrated on %d cases from %s\n", case_count, labels);
   printf("MRR: %.4f\n", final_mrr);
   printf("Recall@%d: %.4f\n", limit, (double)hits / (double)case_count);
   printf("Multipliers: lexical=%.2f coverage=%.2f entity=%.2f temporal=%.2f evidence=%.2f "
          "semantic=%.2f state=%.2f intent=%.2f confidence=%.2f\n",
          best.lexical, best.coverage, best.entity, best.temporal, best.evidence, best.semantic,
          best.state, best.intent, best.confidence);
   if (write_path && write_path[0])
      printf("Wrote profile: %s\n", write_path);
   if (apply_config && write_path && write_path[0])
      printf("Applied profile in aimee config\n");
}

static void mem_print_eval_report(const char *title, const mem_eval_scores_t *scores,
                                  const mem_eval_latency_t *latency)
{
   printf("%s (%d cases)\n", title, scores->n_cases);
   printf("  MRR:       %.4f\n", scores->mrr);
   printf("  NDCG@5:    %.4f\n", scores->ndcg_5);
   printf("  NDCG@10:   %.4f\n", scores->ndcg_10);
   printf("  Recall@5:  %.4f\n", scores->recall_5);
   printf("  Recall@10: %.4f\n", scores->recall_10);
   if (latency && latency->n_queries > 0)
   {
      printf("  Latency:   p50=%.3fms p95=%.3fms p99=%.3fms min=%.3fms max=%.3fms (%d queries)\n",
             latency->p50_ms, latency->p95_ms, latency->p99_ms, latency->min_ms, latency->max_ms,
             latency->n_queries);
   }
   printf("  Route Buckets:\n");
   for (int i = 0; i < MEM_EVAL_ROUTE_BUCKET_COUNT; i++)
   {
      if (scores->route_buckets[i].cases <= 0)
         continue;
      printf("    %s: cases=%d mrr=%.4f recall@10=%.4f\n",
             memory_query_route_name((memory_query_route_t)i), scores->route_buckets[i].cases,
             scores->route_buckets[i].metric_a, scores->route_buckets[i].metric_b);
   }
   printf("  Shape Buckets:\n");
   for (int i = 0; i < MEM_EVAL_SHAPE_BUCKET_COUNT; i++)
   {
      if (scores->shape_buckets[i].cases <= 0)
         continue;
      printf("    %s: cases=%d mrr=%.4f recall@10=%.4f\n",
             memory_query_shape_name((memory_query_shape_t)i), scores->shape_buckets[i].cases,
             scores->shape_buckets[i].metric_a, scores->shape_buckets[i].metric_b);
   }
}

static void mem_print_eval_qa_report(const char *title, const mem_eval_qa_scores_t *scores,
                                     const mem_eval_latency_t *latency)
{
   printf("%s (%d cases)\n", title, scores->n_cases);
   printf("  Accuracy:              %.4f\n", scores->accuracy);
   printf("  Hallucination Rate:    %.4f\n", scores->hallucination_rate);
   printf("  Exact Match:           %.4f\n", scores->exact_match);
   printf("  Citation Coverage:     %.4f\n", scores->citation_coverage);
   printf("  Citation Miss Rate:    %.4f\n", scores->citation_miss_rate);
   printf("  Answered Cases:        %d\n", scores->answered_cases);
   printf("  Judged Cases:          %d\n", scores->judged_cases);
   printf("  Cited Answers:         %d\n", scores->cited_answers);
   printf("  Uncited Answers:       %d\n", scores->uncited_answers);
   printf("  Low-Confidence Answers:%d\n", scores->low_confidence_answers);
   printf("  Avg Retrieved Tokens:  %.1f\n", scores->avg_retrieved_tokens);
   printf("  Answer Tokens:         prompt=%d completion=%d\n", scores->total_answer_prompt_tokens,
          scores->total_answer_completion_tokens);
   printf("  Judge Tokens:          prompt=%d completion=%d\n", scores->total_judge_prompt_tokens,
          scores->total_judge_completion_tokens);
   if (latency && latency->n_queries > 0)
   {
      printf("  End-to-End Latency:    p50=%.3fms p95=%.3fms p99=%.3fms min=%.3fms max=%.3fms (%d "
             "cases)\n",
             latency->p50_ms, latency->p95_ms, latency->p99_ms, latency->min_ms, latency->max_ms,
             latency->n_queries);
   }
   printf("  Route Buckets:\n");
   for (int i = 0; i < MEM_EVAL_ROUTE_BUCKET_COUNT; i++)
   {
      if (scores->route_buckets[i].cases <= 0)
         continue;
      printf("    %s: cases=%d accuracy=%.4f exact_match=%.4f\n",
             memory_query_route_name((memory_query_route_t)i), scores->route_buckets[i].cases,
             scores->route_buckets[i].metric_a, scores->route_buckets[i].metric_b);
   }
   printf("  Shape Buckets:\n");
   for (int i = 0; i < MEM_EVAL_SHAPE_BUCKET_COUNT; i++)
   {
      if (scores->shape_buckets[i].cases <= 0)
         continue;
      printf("    %s: cases=%d accuracy=%.4f exact_match=%.4f\n",
             memory_query_shape_name((memory_query_shape_t)i), scores->shape_buckets[i].cases,
             scores->shape_buckets[i].metric_a, scores->shape_buckets[i].metric_b);
   }
}

static cJSON *mem_eval_bucket_json(const mem_eval_bucket_scores_t *buckets, int count, int is_route,
                                   const char *metric_a_name, const char *metric_b_name)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   for (int i = 0; i < count; i++)
   {
      if (buckets[i].cases <= 0)
         continue;
      const char *name = is_route ? memory_query_route_name((memory_query_route_t)i)
                                  : memory_query_shape_name((memory_query_shape_t)i);
      cJSON *row = cJSON_CreateObject();
      if (!row)
         continue;
      jo_add_i64(row, "cases", buckets[i].cases);
      jo_add_num(row, metric_a_name, buckets[i].metric_a);
      jo_add_num(row, metric_b_name, buckets[i].metric_b);
      cJSON_AddItemToObject(obj, name, row);
   }
   return obj;
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

   config_t cfg;
   if (config_load(&cfg) == 0 && cfg.memory_weight_profile[0] && active && active_len > 0)
      snprintf(active, active_len, "%s", cfg.memory_weight_profile);
}

static void mem_benchmark_restore_weight_profile(const char *previous)
{
   platform_setenv("AIMEE_MEMORY_WEIGHT_PROFILE", (previous && previous[0]) ? previous : "");
}

static void mem_benchmark_print_weight_profile(const char *weight_profile)
{
   if (weight_profile && weight_profile[0])
      printf("Weight profile: %s\n", weight_profile);
}

void mem_emit_eval_json(app_ctx_t *ctx, const char *suite, const char *dataset,
                        const mem_eval_scores_t *scores, const mem_eval_latency_t *latency,
                        const char *weight_profile)
{
   cJSON *obj = cJSON_CreateObject();
   jo_add_str(obj, "suite", suite);
   if (dataset && dataset[0])
      jo_add_str(obj, "dataset", dataset);
   if (weight_profile && weight_profile[0])
      jo_add_str(obj, "weight_profile", weight_profile);
   cJSON *metrics = cJSON_CreateObject();
   jo_add_num(metrics, "mrr", scores->mrr);
   jo_add_num(metrics, "ndcg_5", scores->ndcg_5);
   jo_add_num(metrics, "ndcg_10", scores->ndcg_10);
   jo_add_num(metrics, "recall_5", scores->recall_5);
   jo_add_num(metrics, "recall_10", scores->recall_10);
   jo_add_i64(metrics, "cases", scores->n_cases);
   cJSON_AddItemToObject(obj, "metrics", metrics);
   cJSON_AddItemToObject(obj, "route_buckets",
                         mem_eval_bucket_json(scores->route_buckets, MEM_EVAL_ROUTE_BUCKET_COUNT, 1,
                                              "mrr", "recall_10"));
   cJSON_AddItemToObject(obj, "shape_buckets",
                         mem_eval_bucket_json(scores->shape_buckets, MEM_EVAL_SHAPE_BUCKET_COUNT, 0,
                                              "mrr", "recall_10"));
   if (latency && latency->n_queries > 0)
   {
      cJSON *lat = cJSON_CreateObject();
      jo_add_num(lat, "p50_ms", latency->p50_ms);
      jo_add_num(lat, "p95_ms", latency->p95_ms);
      jo_add_num(lat, "p99_ms", latency->p99_ms);
      jo_add_num(lat, "min_ms", latency->min_ms);
      jo_add_num(lat, "max_ms", latency->max_ms);
      jo_add_i64(lat, "queries", latency->n_queries);
      cJSON_AddItemToObject(obj, "latency", lat);
   }
   emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
}

void mem_emit_eval_qa_json(app_ctx_t *ctx, const char *suite, const char *dataset,
                           const mem_eval_qa_scores_t *scores, const mem_eval_latency_t *latency,
                           const char *weight_profile)
{
   cJSON *obj = cJSON_CreateObject();
   jo_add_str(obj, "suite", suite);
   if (dataset && dataset[0])
      jo_add_str(obj, "dataset", dataset);
   if (weight_profile && weight_profile[0])
      jo_add_str(obj, "weight_profile", weight_profile);
   cJSON *metrics = cJSON_CreateObject();
   jo_add_num(metrics, "accuracy", scores->accuracy);
   jo_add_num(metrics, "hallucination_rate", scores->hallucination_rate);
   jo_add_num(metrics, "exact_match", scores->exact_match);
   jo_add_num(metrics, "citation_coverage", scores->citation_coverage);
   jo_add_num(metrics, "citation_miss_rate", scores->citation_miss_rate);
   jo_add_num(metrics, "avg_retrieved_tokens", scores->avg_retrieved_tokens);
   jo_add_i64(metrics, "cases", scores->n_cases);
   jo_add_i64(metrics, "answered_cases", scores->answered_cases);
   jo_add_i64(metrics, "judged_cases", scores->judged_cases);
   jo_add_i64(metrics, "cited_answers", scores->cited_answers);
   jo_add_i64(metrics, "uncited_answers", scores->uncited_answers);
   jo_add_i64(metrics, "low_confidence_answers", scores->low_confidence_answers);
   jo_add_i64(metrics, "answer_prompt_tokens", scores->total_answer_prompt_tokens);
   jo_add_i64(metrics, "answer_completion_tokens", scores->total_answer_completion_tokens);
   jo_add_i64(metrics, "judge_prompt_tokens", scores->total_judge_prompt_tokens);
   jo_add_i64(metrics, "judge_completion_tokens", scores->total_judge_completion_tokens);
   cJSON_AddItemToObject(obj, "metrics", metrics);
   cJSON_AddItemToObject(obj, "route_buckets",
                         mem_eval_bucket_json(scores->route_buckets, MEM_EVAL_ROUTE_BUCKET_COUNT, 1,
                                              "accuracy", "exact_match"));
   cJSON_AddItemToObject(obj, "shape_buckets",
                         mem_eval_bucket_json(scores->shape_buckets, MEM_EVAL_SHAPE_BUCKET_COUNT, 0,
                                              "accuracy", "exact_match"));
   if (latency && latency->n_queries > 0)
   {
      cJSON *lat = cJSON_CreateObject();
      jo_add_num(lat, "p50_ms", latency->p50_ms);
      jo_add_num(lat, "p95_ms", latency->p95_ms);
      jo_add_num(lat, "p99_ms", latency->p99_ms);
      jo_add_num(lat, "min_ms", latency->min_ms);
      jo_add_num(lat, "max_ms", latency->max_ms);
      jo_add_i64(lat, "queries", latency->n_queries);
      cJSON_AddItemToObject(obj, "latency", lat);
   }
   emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
}

static int mem_load_live_cases(mem_eval_case_t *cases, int max_cases, char *basis, size_t basis_len)
{
   if (max_cases <= 0)
   {
      if (basis && basis_len > 0)
         basis[0] = '\0';
      return 0;
   }

   memory_t rows[100];
   int row_cap = max_cases < (int)(sizeof(rows) / sizeof(rows[0]))
                     ? max_cases
                     : (int)(sizeof(rows) / sizeof(rows[0]));
   int n_rows = kb_client_memory_load_eval_corpus(rows, row_cap, basis, basis_len);
   if (n_rows <= 0)
      return 0;

   memset(cases, 0, (size_t)max_cases * sizeof(*cases));
   int n_cases = 0;
   for (int i = 0; i < n_rows && n_cases < max_cases; i++)
   {
      const char *key = rows[i].key;
      const char *content = rows[i].content;
      snprintf(cases[n_cases].query, sizeof(cases[n_cases].query), "%s",
               key && key[0] ? key : (content ? content : ""));
      if (!cases[n_cases].query[0])
         continue;
      cases[n_cases].expected_ids[0] = rows[i].id;
      cases[n_cases].n_expected = 1;
      n_cases++;
   }
   return n_cases;
}

/* Ablation-arm resolution lives in agent_eval_memory_support.c
 * (mem_eval_fusion_arm_resolve) so the server's memory.benchmark handler shares
 * it; the CLI runner below calls the same function. */

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

   if (strcmp(suite, "corpus") == 0 || strcmp(suite, "memory-retrieval") == 0)
   {
      const char *corpus_path = opt_get(&opts, "corpus");
      const char *baseline_path = opt_get(&opts, "baseline");
      int update_baseline = opt_get_flag(&opts, "update-baseline");
      int compare_rerank = opt_get_flag(&opts, "compare-rerank");
      if (!corpus_path)
         corpus_path = "tests/eval/memory_retrieval_corpus.json";
      if (!baseline_path)
         baseline_path = "tests/eval/memory_retrieval_baseline.json";

      mem_eval_case_t corpus_cases[MEM_CORPUS_MAX_CASES];
      int n_corpus = mem_eval_load_corpus(corpus_path, corpus_cases, MEM_CORPUS_MAX_CASES);
      if (n_corpus <= 0)
         fatal("memory benchmark corpus failed for %s", corpus_path);

      mem_eval_scores_t scores;
      mem_eval_latency_t latency;
      mem_eval_run_with_latency(corpus_cases, n_corpus, &scores, &latency);

      /* Per-stage delta: re-run with the cross-encoder force-disabled so
       * operators can see the quality / latency contribution of rerank
       * in isolation.  Other stages (hybrid retrieval, lexical + semantic
       * expansion) remain enabled for both runs — rerank is the only
       * toggle. */
      mem_eval_scores_t scores_no_rerank;
      mem_eval_latency_t latency_no_rerank;
      int have_no_rerank = 0;
      if (compare_rerank)
      {
         platform_setenv("AIMEE_MEMORY_RERANK_FORCE_OFF", "1");
         mem_eval_run_with_latency(corpus_cases, n_corpus, &scores_no_rerank, &latency_no_rerank);
         platform_setenv("AIMEE_MEMORY_RERANK_FORCE_OFF", "");
         have_no_rerank = 1;
      }

      mem_eval_close_temp_db();

      if (update_baseline)
      {
         if (mem_eval_save_baseline(baseline_path, &scores, 5.0) != 0)
            fatal("failed to update benchmark baseline: %s", baseline_path);
      }

      if (ctx->json_output)
      {
         mem_emit_eval_json(ctx, "corpus", corpus_path, &scores, &latency,
                            benchmark_weight_profile);
         BENCHMARK_RETURN;
      }

      char title[1024];
      snprintf(title, sizeof(title), "Memory Benchmark — corpus: %s", corpus_path);
      mem_print_eval_report(title, &scores, &latency);
      mem_benchmark_print_weight_profile(benchmark_weight_profile);
      if (have_no_rerank)
      {
         char no_rerank_title[1024];
         snprintf(no_rerank_title, sizeof(no_rerank_title),
                  "Memory Benchmark — corpus (rerank OFF): %s", corpus_path);
         mem_print_eval_report(no_rerank_title, &scores_no_rerank, &latency_no_rerank);
         printf("\n# Per-stage delta (rerank on vs off)\n");
         printf("  MRR       : %+0.4f  (on %.4f, off %.4f)\n", scores.mrr - scores_no_rerank.mrr,
                scores.mrr, scores_no_rerank.mrr);
         printf("  recall@5  : %+0.4f  (on %.4f, off %.4f)\n",
                scores.recall_5 - scores_no_rerank.recall_5, scores.recall_5,
                scores_no_rerank.recall_5);
         printf("  recall@10 : %+0.4f  (on %.4f, off %.4f)\n",
                scores.recall_10 - scores_no_rerank.recall_10, scores.recall_10,
                scores_no_rerank.recall_10);
         printf("  ndcg@5    : %+0.4f  (on %.4f, off %.4f)\n",
                scores.ndcg_5 - scores_no_rerank.ndcg_5, scores.ndcg_5, scores_no_rerank.ndcg_5);
         printf("  ndcg@10   : %+0.4f  (on %.4f, off %.4f)\n",
                scores.ndcg_10 - scores_no_rerank.ndcg_10, scores.ndcg_10,
                scores_no_rerank.ndcg_10);
         printf("  p50_ms    : %+0.2f  (on %.2f, off %.2f)\n",
                latency.p50_ms - latency_no_rerank.p50_ms, latency.p50_ms,
                latency_no_rerank.p50_ms);
         printf("  p95_ms    : %+0.2f  (on %.2f, off %.2f)\n",
                latency.p95_ms - latency_no_rerank.p95_ms, latency.p95_ms,
                latency_no_rerank.p95_ms);
         printf("  p99_ms    : %+0.2f  (on %.2f, off %.2f)\n",
                latency.p99_ms - latency_no_rerank.p99_ms, latency.p99_ms,
                latency_no_rerank.p99_ms);
      }
      if (update_baseline)
         printf("Baseline updated: %s\n", baseline_path);
      else
      {
         mem_eval_scores_t baseline;
         double threshold_pct = 5.0;
         if (mem_eval_load_baseline(baseline_path, &baseline, &threshold_pct) == 0)
         {
            if (mem_eval_check_regression(&scores, &baseline, threshold_pct) != 0)
               fatal("memory benchmark regression detected vs %s", baseline_path);
            printf("OK: no regression vs baseline (%s, threshold %.1f%%)\n", baseline_path,
                   threshold_pct);
         }
      }
      BENCHMARK_RETURN;
   }

   if (strcmp(suite, "locomo") == 0)
   {
      const char *dataset_path = opt_get(&opts, "dataset");
      int max_cases = opt_get_int(&opts, "max-cases", 0);
      if (!dataset_path)
         dataset_path = "data/locomo/locomo10.json";
      mem_eval_scores_t scores;
      mem_eval_latency_t latency;
      int samples = 0;
      if (mem_eval_run_locomo(dataset_path, max_cases, &scores, &latency, &samples, NULL) != 0)
         fatal("LoCoMo benchmark failed for %s", dataset_path);
      if (ctx->json_output)
      {
         mem_emit_eval_json(ctx, "locomo", dataset_path, &scores, &latency,
                            benchmark_weight_profile);
         BENCHMARK_RETURN;
      }
      char title[1024];
      if (max_cases > 0)
         snprintf(title, sizeof(title),
                  "Memory Benchmark — LoCoMo: %s (%d conversations, capped at %d)", dataset_path,
                  samples, max_cases);
      else
         snprintf(title, sizeof(title), "Memory Benchmark — LoCoMo: %s (%d conversations)",
                  dataset_path, samples);
      mem_print_eval_report(title, &scores, &latency);
      mem_benchmark_print_weight_profile(benchmark_weight_profile);
      BENCHMARK_RETURN;
   }

   if (strcmp(suite, "longmemeval") == 0)
   {
      const char *dataset_path = opt_get(&opts, "dataset");
      int max_cases = opt_get_int(&opts, "max-cases", 0);
      if (!dataset_path)
         dataset_path = "data/longmemeval/longmemeval_s_cleaned.json";
      mem_eval_scores_t scores;
      mem_eval_latency_t latency;
      int cases = 0;
      if (mem_eval_run_longmemeval(dataset_path, max_cases, &scores, &latency, &cases, NULL) != 0)
         fatal("LongMemEval benchmark failed for %s", dataset_path);
      if (ctx->json_output)
      {
         mem_emit_eval_json(ctx, "longmemeval", dataset_path, &scores, &latency,
                            benchmark_weight_profile);
         BENCHMARK_RETURN;
      }
      char title[1024];
      if (max_cases > 0)
         snprintf(title, sizeof(title), "Memory Benchmark — LongMemEval: %s (%d cases)",
                  dataset_path, cases);
      else
         snprintf(title, sizeof(title), "Memory Benchmark — LongMemEval: %s", dataset_path);
      mem_print_eval_report(title, &scores, &latency);
      mem_benchmark_print_weight_profile(benchmark_weight_profile);
      BENCHMARK_RETURN;
   }

   if (strcmp(suite, "locomo-qa") == 0)
   {
      const char *dataset_path = opt_get(&opts, "dataset");
      int max_cases = opt_get_int(&opts, "max-cases", 0);
      int top_k = opt_get_int(&opts, "top-k", 10);
      int token_budget = opt_get_int(&opts, "token-budget", 2000);
      int report_failures = opt_get_flag(&opts, "report-failures");
      int max_failures = opt_get_int(&opts, "max-failures", 5);
      if (!dataset_path)
         dataset_path = "data/locomo/locomo10.json";
      mem_eval_qa_scores_t scores;
      mem_eval_latency_t latency;
      int samples = 0;
      if (mem_eval_run_locomo_qa(dataset_path, max_cases, top_k, token_budget, &scores, &latency,
                                 &samples) != 0)
         fatal("LoCoMo QA benchmark failed for %s", dataset_path);
      if (ctx->json_output)
      {
         mem_emit_eval_qa_json(ctx, "locomo-qa", dataset_path, &scores, &latency,
                               benchmark_weight_profile);
         BENCHMARK_RETURN;
      }
      char title[1024];
      snprintf(title, sizeof(title),
               "Memory Benchmark — LoCoMo QA: %s (%d conversations, top_k=%d token_budget=%d)",
               dataset_path, samples, top_k, token_budget);
      mem_print_eval_qa_report(title, &scores, &latency);
      mem_benchmark_print_weight_profile(benchmark_weight_profile);
      if (report_failures)
         (void)mem_eval_report_locomo_qa_failures(dataset_path, max_cases, top_k, token_budget,
                                                  max_failures, stdout);
      BENCHMARK_RETURN;
   }

   if (strcmp(suite, "longmemeval-qa") == 0)
   {
      const char *dataset_path = opt_get(&opts, "dataset");
      int max_cases = opt_get_int(&opts, "max-cases", 0);
      int top_k = opt_get_int(&opts, "top-k", 10);
      int token_budget = opt_get_int(&opts, "token-budget", 2000);
      int report_failures = opt_get_flag(&opts, "report-failures");
      int max_failures = opt_get_int(&opts, "max-failures", 5);
      if (!dataset_path)
         dataset_path = "data/longmemeval/longmemeval_s_cleaned.json";
      mem_eval_qa_scores_t scores;
      mem_eval_latency_t latency;
      int cases = 0;
      if (mem_eval_run_longmemeval_qa(dataset_path, max_cases, top_k, token_budget, &scores,
                                      &latency, &cases) != 0)
         fatal("LongMemEval QA benchmark failed for %s", dataset_path);
      if (ctx->json_output)
      {
         mem_emit_eval_qa_json(ctx, "longmemeval-qa", dataset_path, &scores, &latency,
                               benchmark_weight_profile);
         BENCHMARK_RETURN;
      }
      char title[1024];
      snprintf(title, sizeof(title),
               "Memory Benchmark — LongMemEval QA: %s (%d cases, top_k=%d token_budget=%d)",
               dataset_path, cases, top_k, token_budget);
      mem_print_eval_qa_report(title, &scores, &latency);
      mem_benchmark_print_weight_profile(benchmark_weight_profile);
      if (report_failures)
         (void)mem_eval_report_longmemeval_qa_failures(dataset_path, max_cases, top_k, token_budget,
                                                       max_failures, stdout);
      BENCHMARK_RETURN;
   }

   if (strcmp(suite, "live") == 0)
   {
      mem_eval_case_t cases[100];
      char basis[64];
      int n_cases = mem_load_live_cases(cases, 100, basis, sizeof(basis));
      if (n_cases == 0)
         fatal("no suitable memories available for live benchmark");

      mem_eval_scores_t scores;
      mem_eval_latency_t latency;
      mem_eval_run_with_latency(cases, n_cases, &scores, &latency);
      if (ctx->json_output)
      {
         mem_emit_eval_json(ctx, "live", NULL, &scores, &latency, benchmark_weight_profile);
         BENCHMARK_RETURN;
      }
      char title[128];
      snprintf(title, sizeof(title), "Memory Benchmark — live DB (%s)",
               basis[0] ? basis : "seeded");
      mem_print_eval_report(title, &scores, &latency);
      mem_benchmark_print_weight_profile(benchmark_weight_profile);
      BENCHMARK_RETURN;
   }

   if (strcmp(suite, "code-graph-fusion") == 0)
   {
      const char *corpus_path = opt_get(&opts, "corpus");
      if (!corpus_path)
         corpus_path = "benchmarks/code-vector-graph/production-corpus.json";
      const char *arm = opt_get(&opts, "arm");
      const char *matrix = opt_get(&opts, "ablation-matrix");
      if (!matrix)
         matrix = "benchmarks/code-vector-graph/ablation-matrix.json";

      /* Resolve the arm's wired knobs from the ablation matrix: fusion state +
       * the utility_scoring / code_projection sub-gates. Arms the matrix doesn't
       * name fall back to baseline=off / else=on; --fusion-state overrides the
       * state. (code_vectors_enabled, utility_cap, and code_structural_factor
       * are not yet plumbed, so arms differing only on those still score
       * identically.) */
      char arm_state[16] = "";
      int utility = 1, projection = 1;
      int have_arm = (arm && mem_eval_fusion_arm_resolve(matrix, arm, arm_state, sizeof(arm_state),
                                                         &utility, &projection) == 0);
      const char *fstate = opt_get(&opts, "fusion-state");
      if (!fstate)
      {
         if (have_arm && arm_state[0])
            fstate = arm_state;
         else
            fstate = (arm && strcmp(arm, "baseline") == 0) ? "off" : "on";
      }

      mem_eval_case_t cases[200];
      int n_cases = mem_eval_load_production_corpus(corpus_path, cases,
                                                    (int)(sizeof(cases) / sizeof(cases[0])));
      if (n_cases <= 0)
         fatal("code-graph-fusion: failed to load corpus %s", corpus_path);
      int labelled = 0;
      for (int i = 0; i < n_cases; i++)
         if (cases[i].n_expected > 0)
            labelled++;

      memory_fusion_state_set(fstate);
      memory_fusion_gates_set(utility, projection);
      mem_eval_scores_t scores;
      mem_eval_latency_t latency;
      mem_eval_run_with_latency(cases, n_cases, &scores, &latency);
      memory_fusion_state_clear();

      if (ctx->json_output)
      {
         mem_emit_eval_json(ctx, "code-graph-fusion", corpus_path, &scores, &latency,
                            benchmark_weight_profile);
         BENCHMARK_RETURN;
      }
      char title[256];
      snprintf(title, sizeof(title),
               "Code-Graph Fusion — arm=%s fusion=%s utility=%d projection=%d (%d queries, %d "
               "labelled)",
               arm ? arm : "(default)", fstate, utility, projection, n_cases, labelled);
      mem_print_eval_report(title, &scores, &latency);
      if (labelled < n_cases)
         printf("  note: %d/%d queries have no expected_ids yet — recall undercounts until the "
                "corpus is labelled\n",
                n_cases - labelled, n_cases);
      mem_benchmark_print_weight_profile(benchmark_weight_profile);
      BENCHMARK_RETURN;
   }

#undef BENCHMARK_RETURN
   fatal("unknown benchmark suite: %s", suite);
}
