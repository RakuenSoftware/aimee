/* cmd_graph.c: `aimee graph` CLI — code-graph projection sync and explain.
 *
 * Both subcommands reach KB_STORE (entity_edges / entity_nodes / code projection)
 * through the kb_client RPC boundary; this CLI never opens KB_STORE directly. */

#include "aimee.h"
#include "cJSON.h"
#include "commands.h"
#include "kb_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* aimee graph sync-code <project> */
static void graph_sync_code(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("graph sync-code requires a project name");
   const char *project = argv[0];

   kb_client_graph_sync_result_t res;
   if (kb_client_graph_sync_code(project, &res) != 0)
      fatal("graph sync-code failed (kb unreachable or sync error)");

   if (ctx->json_output)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "status", "ok");
      cJSON_AddStringToObject(o, "project", res.project);
      cJSON_AddNumberToObject(o, "generation_id", (double)res.generation_id);
      cJSON_AddNumberToObject(o, "edge_count", (double)res.edge_count);
      char *s = cJSON_PrintUnformatted(o);
      printf("%s\n", s ? s : "{}");
      free(s);
      cJSON_Delete(o);
   }
   else
   {
      printf("graph sync-code: project=%s generation=%lld edges=%lld (published)\n", res.project,
             (long long)res.generation_id, (long long)res.edge_count);
   }
}

/* aimee graph explain <file-or-symbol-or-node-key> [--limit N] */
static void graph_explain(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("graph explain requires a file path, symbol, or canonical node key");
   const char *entity = argv[0];
   int limit = 40;
   for (int i = 1; i < argc - 1; i++)
      if (strcmp(argv[i], "--limit") == 0)
         limit = atoi(argv[i + 1]);

   char *json = kb_client_graph_explain_json(entity, limit);
   if (!json)
      fatal("graph explain failed (kb unreachable)");

   if (ctx->json_output)
   {
      printf("%s\n", json);
      free(json);
      return;
   }

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      fatal("graph explain: malformed response");

   cJSON *node = cJSON_GetObjectItemCaseSensitive(resp, "node");
   printf("graph explain: %s\n", entity);
   if (cJSON_IsObject(node))
   {
      cJSON *origin = cJSON_GetObjectItemCaseSensitive(node, "node_origin");
      cJSON *proj = cJSON_GetObjectItemCaseSensitive(node, "project");
      printf("  canonical node: origin=%s project=%s\n",
             cJSON_IsString(origin) ? origin->valuestring : "",
             cJSON_IsString(proj) ? proj->valuestring : "");
   }
   printf("  (weights are provisional random-walk priors)\n");

   cJSON *edges = cJSON_GetObjectItemCaseSensitive(resp, "edges");
   int n = 0;
   if (cJSON_IsArray(edges))
   {
      cJSON *e;
      cJSON_ArrayForEach(e, edges)
      {
         cJSON *src = cJSON_GetObjectItemCaseSensitive(e, "source");
         cJSON *rel = cJSON_GetObjectItemCaseSensitive(e, "relation");
         cJSON *tgt = cJSON_GetObjectItemCaseSensitive(e, "target");
         cJSON *w = cJSON_GetObjectItemCaseSensitive(e, "weight");
         cJSON *sw = cJSON_GetObjectItemCaseSensitive(e, "structural_weight");
         cJSON *util = cJSON_GetObjectItemCaseSensitive(e, "utility_score");
         cJSON *orig = cJSON_GetObjectItemCaseSensitive(e, "edge_origin");
         printf("  %-24s --%s--> %-24s  w=%g sw=%g util=%g origin=%s\n",
                cJSON_IsString(src) ? src->valuestring : "",
                cJSON_IsString(rel) ? rel->valuestring : "",
                cJSON_IsString(tgt) ? tgt->valuestring : "",
                cJSON_IsNumber(w) ? w->valuedouble : 0.0,
                cJSON_IsNumber(sw) ? sw->valuedouble : 0.0,
                cJSON_IsNumber(util) ? util->valuedouble : 0.0,
                cJSON_IsString(orig) ? orig->valuestring : "");
         n++;
      }
   }
   if (n == 0)
      printf("  (no incident edges)\n");
   cJSON_Delete(resp);
}

static const subcmd_t graph_subcmds[] = {
    {"sync-code", "Project the code index into the memory graph (<project>)", graph_sync_code},
    {"explain", "Show graph edges incident to a file/symbol/node (<entity> [--limit N])",
     graph_explain},
};

void cmd_graph(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      subcmd_usage("graph", graph_subcmds);
      exit(1);
   }
   const char *sub = argv[0];
   argc--;
   argv++;
   if (subcmd_dispatch(graph_subcmds, sub, ctx, argc, argv) != 0)
      fatal("unknown graph subcommand: %s", sub);
}
