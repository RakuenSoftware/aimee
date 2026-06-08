/* cmd_optimize.c: `aimee optimize` — operator surface for the bandit
 * optimization loop (decision-point registry + off-policy inspection).
 *
 * Thin-client command (special-cased in cli_main.c like `persona`/`manuscript`).
 * Every subcommand dispatches the `optimize.export` method to its first-class
 * /v1 route (GET /v1/optimize/export) via cli_v1_rpc_local; aimee-server proxies
 * to the kb intelligence export, which carries the declared `registry`, the
 * per-point `arm_stats` baseline, and the closed-decision log for replay.
 * See docs/proposals/pending/optimization-surface.md (P1). */
#include "cJSON.h"
#include "cli_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Dispatch optimize.export and return the parsed response (caller frees), or
 * NULL after printing an error. */
static cJSON *optimize_fetch(void)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return NULL;
   cJSON_AddStringToObject(req, "method", "optimize.export");
   cJSON *resp = cli_v1_rpc_local(req, 30000);
   cJSON_Delete(req);
   if (!resp)
   {
      fprintf(stderr, "optimize: no response from aimee-server\n");
      return NULL;
   }
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      fprintf(stderr, "optimize: %s\n", cJSON_IsString(msg) ? msg->valuestring : "unknown error");
      cJSON_Delete(resp);
      return NULL;
   }
   return resp;
}

static void optimize_print_json(cJSON *node)
{
   char *s = node ? cJSON_PrintUnformatted(node) : NULL;
   if (s)
   {
      puts(s);
      free(s);
   }
}

static cJSON *optimize_find_point(cJSON *resp, const char *point)
{
   cJSON *points = cJSON_GetObjectItemCaseSensitive(resp, "points");
   if (!cJSON_IsArray(points) || !point)
      return NULL;
   cJSON *pt;
   cJSON_ArrayForEach(pt, points)
   {
      cJSON *dp = cJSON_GetObjectItemCaseSensitive(pt, "decision_point");
      if (cJSON_IsString(dp) && strcmp(dp->valuestring, point) == 0)
         return pt;
   }
   return NULL;
}

static int optimize_decisions_count(cJSON *resp, const char *point)
{
   cJSON *pt = optimize_find_point(resp, point);
   cJSON *d = pt ? cJSON_GetObjectItemCaseSensitive(pt, "decisions") : NULL;
   return cJSON_IsArray(d) ? cJSON_GetArraySize(d) : 0;
}

static const char *optimize_arg_point(int argc, char **argv)
{
   for (int i = 0; i < argc; i++)
      if (strcmp(argv[i], "--point") == 0 && i + 1 < argc)
         return argv[i + 1];
   return NULL;
}

static int optimize_cmd_points(int json_output)
{
   cJSON *resp = optimize_fetch();
   if (!resp)
      return 1;
   cJSON *registry = cJSON_GetObjectItemCaseSensitive(resp, "registry");

   if (json_output)
   {
      optimize_print_json(registry);
      cJSON_Delete(resp);
      return 0;
   }
   if (!cJSON_IsArray(registry) || cJSON_GetArraySize(registry) == 0)
   {
      printf("optimize: no decision points registered\n");
      cJSON_Delete(resp);
      return 0;
   }
   printf("Decision points:\n");
   cJSON *e;
   cJSON_ArrayForEach(e, registry)
   {
      cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "decision_point");
      cJSON *status = cJSON_GetObjectItemCaseSensitive(e, "status");
      cJSON *reward = cJSON_GetObjectItemCaseSensitive(e, "reward_fn");
      cJSON *arms = cJSON_GetObjectItemCaseSensitive(e, "arms");
      const char *id_s = cJSON_IsString(id) ? id->valuestring : "?";
      printf("  %-28s [%s] reward=%s decisions=%d\n", id_s,
             cJSON_IsString(status) ? status->valuestring : "?",
             cJSON_IsString(reward) ? reward->valuestring : "?",
             optimize_decisions_count(resp, id_s));
      if (cJSON_IsArray(arms))
      {
         printf("      arms:");
         cJSON *a;
         cJSON_ArrayForEach(a, arms) printf(" %s", cJSON_IsString(a) ? a->valuestring : "?");
         printf("\n");
      }
   }
   cJSON_Delete(resp);
   return 0;
}

static int optimize_cmd_baseline(int argc, char **argv, int json_output)
{
   const char *point = optimize_arg_point(argc, argv);
   if (!point)
   {
      fprintf(stderr, "usage: aimee optimize baseline --point <decision_point>\n");
      return 2;
   }
   cJSON *resp = optimize_fetch();
   if (!resp)
      return 1;
   cJSON *pt = optimize_find_point(resp, point);

   if (json_output)
   {
      optimize_print_json(pt ? pt : resp);
      cJSON_Delete(resp);
      return 0;
   }
   if (!pt)
   {
      printf("optimize: %s has no logged decisions yet (baseline empty)\n", point);
      cJSON_Delete(resp);
      return 0;
   }
   cJSON *arm_stats = cJSON_GetObjectItemCaseSensitive(pt, "arm_stats");
   cJSON *decisions = cJSON_GetObjectItemCaseSensitive(pt, "decisions");
   printf("Baseline for %s (%d closed decisions):\n", point,
          cJSON_IsArray(decisions) ? cJSON_GetArraySize(decisions) : 0);
   if (cJSON_IsArray(arm_stats) && cJSON_GetArraySize(arm_stats) > 0)
   {
      cJSON *entry;
      cJSON_ArrayForEach(entry, arm_stats)
      {
         cJSON *arm = cJSON_GetObjectItemCaseSensitive(entry, "arm_id");
         cJSON *nd = cJSON_GetObjectItemCaseSensitive(entry, "n_decisions");
         cJSON *nr = cJSON_GetObjectItemCaseSensitive(entry, "n_rewards");
         cJSON *alpha = cJSON_GetObjectItemCaseSensitive(entry, "posterior_alpha");
         cJSON *beta = cJSON_GetObjectItemCaseSensitive(entry, "posterior_beta");
         double a = cJSON_IsNumber(alpha) ? alpha->valuedouble : 1.0;
         double b = cJSON_IsNumber(beta) ? beta->valuedouble : 1.0;
         double mean = (a + b > 0.0) ? a / (a + b) : 0.0;
         printf("  %-24s decisions=%lld rewards=%lld posterior_mean=%.3f (alpha=%.2f beta=%.2f)\n",
                cJSON_IsString(arm) ? arm->valuestring : "?",
                cJSON_IsNumber(nd) ? (long long)nd->valuedouble : 0LL,
                cJSON_IsNumber(nr) ? (long long)nr->valuedouble : 0LL, mean, a, b);
      }
   }
   else
   {
      printf("  (no arm stats)\n");
   }
   cJSON_Delete(resp);
   return 0;
}

static int optimize_cmd_replay(int argc, char **argv, int json_output)
{
   const char *point = optimize_arg_point(argc, argv);
   if (!point)
   {
      fprintf(stderr, "usage: aimee optimize replay --point <decision_point>\n");
      return 2;
   }
   cJSON *resp = optimize_fetch();
   if (!resp)
      return 1;
   cJSON *pt = optimize_find_point(resp, point);
   cJSON *decisions = pt ? cJSON_GetObjectItemCaseSensitive(pt, "decisions") : NULL;
   int n = cJSON_IsArray(decisions) ? cJSON_GetArraySize(decisions) : 0;

   if (json_output)
   {
      optimize_print_json(cJSON_IsArray(decisions) ? decisions : resp);
      cJSON_Delete(resp);
      return 0;
   }
   printf("%s: %d closed decision(s) available for off-policy replay.\n", point, n);
   if (n == 0)
      printf("  (enable bandit_live_decision_enabled and accrue traffic, or check the point id)\n");
   else
      printf("  Pipe `aimee --json optimize replay --point %s` into tools/bandit_replay.py "
             "for an IPW estimate.\n",
             point);
   cJSON_Delete(resp);
   return 0;
}

static void optimize_usage(void)
{
   fprintf(stderr, "Usage: aimee optimize <subcommand> [options]\n\nSubcommands:\n"
                   "  points                       List registered decision points\n"
                   "  baseline --point <name>      Show current arm posteriors for a point\n"
                   "  replay --point <name>        Emit a point's closed-decision log for replay\n");
}

int cmd_optimize_run(int argc, char **argv, int json_output)
{
   if (argc < 1)
   {
      optimize_usage();
      return 2;
   }
   const char *sub = argv[0];
   if (strcmp(sub, "points") == 0)
      return optimize_cmd_points(json_output);
   if (strcmp(sub, "baseline") == 0)
      return optimize_cmd_baseline(argc - 1, argv + 1, json_output);
   if (strcmp(sub, "replay") == 0)
      return optimize_cmd_replay(argc - 1, argv + 1, json_output);
   if (strcmp(sub, "--help") == 0 || strcmp(sub, "-h") == 0 || strcmp(sub, "help") == 0)
   {
      optimize_usage();
      return 0;
   }
   fprintf(stderr, "Unknown optimize subcommand: %s\n", sub);
   optimize_usage();
   return 2;
}
