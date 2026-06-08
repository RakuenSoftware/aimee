/* cmd_optimize.c: `aimee optimize` — operator surface for the bandit
 * optimization loop (decision-point registry + off-policy inspection).
 *
 * Thin-client command (special-cased in cli_main.c like `persona`/`manuscript`).
 * points/baseline/replay dispatch `optimize.export` (GET /v1/optimize/export);
 * replay-record dispatches `optimize.replay_record`; run/compare dispatch the
 * `memory.benchmark` suite (the offline-suite adapter). All via
 * cli_v1_dispatch_local to first-class /v1 routes — no kb_client in the thin
 * client. See docs/proposals/pending/optimization-surface.md (P1/P2). */
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
   cJSON *resp = cli_v1_dispatch_local(req, 30000);
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

/* Read an entire file into a malloc'd NUL-terminated buffer (caller frees). */
static char *optimize_slurp(const char *path)
{
   FILE *fp = fopen(path, "rb");
   if (!fp)
      return NULL;
   fseek(fp, 0, SEEK_END);
   long n = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (n < 0)
   {
      fclose(fp);
      return NULL;
   }
   char *buf = malloc((size_t)n + 1);
   if (!buf)
   {
      fclose(fp);
      return NULL;
   }
   size_t got = fread(buf, 1, (size_t)n, fp);
   fclose(fp);
   buf[got] = '\0';
   return buf;
}

/* aimee optimize replay-record --point <p> --file <f>: record an off-policy
 * replay result (output of tools/bandit_replay.py) as a benchmark_trace. */
static int optimize_cmd_replay_record(int argc, char **argv, int json_output)
{
   const char *point = optimize_arg_point(argc, argv);
   const char *file = NULL;
   for (int i = 0; i < argc; i++)
      if (strcmp(argv[i], "--file") == 0 && i + 1 < argc)
         file = argv[i + 1];
   if (!point || !file)
   {
      fprintf(stderr, "usage: aimee optimize replay-record --point <decision_point> --file <result.json>\n");
      return 2;
   }
   char *raw = optimize_slurp(file);
   if (!raw)
   {
      fprintf(stderr, "optimize: cannot read %s\n", file);
      return 1;
   }
   cJSON *result = cJSON_Parse(raw);
   free(raw);
   if (!cJSON_IsObject(result))
   {
      cJSON_Delete(result);
      fprintf(stderr, "optimize: %s must contain a JSON object (replay-tool output)\n", file);
      return 1;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "optimize.replay_record");
   cJSON_AddStringToObject(req, "decision_point", point);
   cJSON_AddItemToObject(req, "result", result);
   cJSON *resp = cli_v1_dispatch_local(req, 30000);
   cJSON_Delete(req);
   if (!resp)
   {
      fprintf(stderr, "optimize: no response from aimee-server\n");
      return 1;
   }

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (json_output)
   {
      optimize_print_json(resp);
   }
   else if (ok)
   {
      cJSON *aid = cJSON_GetObjectItemCaseSensitive(resp, "artifact_id");
      printf("recorded benchmark_trace %s for decision_point=%s\n",
             cJSON_IsString(aid) ? aid->valuestring : "?", point);
   }
   else
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      fprintf(stderr, "optimize replay-record: %s\n",
              cJSON_IsString(msg) ? msg->valuestring : "unknown error");
   }
   cJSON_Delete(resp);
   return ok ? 0 : 1;
}

/* ── offline benchmark suite (optimize run / compare) ─────────────────────── */

/* Read a numeric metric from a benchmark response's `metrics` object. */
static double opt_metric(cJSON *resp, const char *key)
{
   cJSON *m = resp ? cJSON_GetObjectItemCaseSensitive(resp, "metrics") : NULL;
   cJSON *v = m ? cJSON_GetObjectItemCaseSensitive(m, key) : NULL;
   return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

/* Read a latency percentile from the `latency` object. */
static double opt_latency(cJSON *resp, const char *key)
{
   cJSON *l = resp ? cJSON_GetObjectItemCaseSensitive(resp, "latency") : NULL;
   cJSON *v = l ? cJSON_GetObjectItemCaseSensitive(l, key) : NULL;
   return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

/* Run one benchmark arm via the memory.benchmark dispatch method (async, polled
 * to completion). Returns the parsed response (caller frees) or NULL after
 * printing an error. The benchmark is long-running, hence the generous timeout. */
static cJSON *optimize_run_arm(const char *suite, const char *arm, const char *corpus,
                               const char *matrix)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "memory.benchmark");
   if (suite)
      cJSON_AddStringToObject(req, "suite", suite);
   if (arm)
      cJSON_AddStringToObject(req, "arm", arm);
   if (corpus)
      cJSON_AddStringToObject(req, "corpus", corpus);
   if (matrix)
      cJSON_AddStringToObject(req, "matrix", matrix);
   cJSON *resp = cli_v1_dispatch_local(req, 600000);
   cJSON_Delete(req);
   if (!resp)
   {
      fprintf(stderr, "optimize: no response from aimee-server (is it running?)\n");
      return NULL;
   }
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      fprintf(stderr, "optimize: benchmark failed: %s\n",
              cJSON_IsString(msg) ? msg->valuestring : "unknown error");
      cJSON_Delete(resp);
      return NULL;
   }
   return resp;
}

static void optimize_print_arm_metrics(const char *label, cJSON *resp)
{
   printf("  %-10s ndcg@10=%.4f mrr=%.4f recall@10=%.4f  p50=%.1fms p95=%.1fms\n", label,
          opt_metric(resp, "ndcg_10"), opt_metric(resp, "mrr"), opt_metric(resp, "recall_10"),
          opt_latency(resp, "p50_ms"), opt_latency(resp, "p95_ms"));
}

/* aimee optimize run --suite <s> [--arm A] [--corpus C] [--matrix M]:
 * run the offline benchmark for one arm, or (no --arm) run baseline vs on and
 * rank by ndcg@10. */
static int optimize_cmd_run(int argc, char **argv, int json_output)
{
   const char *suite = "code-graph-fusion";
   const char *arm = NULL, *corpus = NULL, *matrix = NULL;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--suite") == 0 && i + 1 < argc)
         suite = argv[++i];
      else if (strcmp(argv[i], "--arm") == 0 && i + 1 < argc)
         arm = argv[++i];
      else if (strcmp(argv[i], "--corpus") == 0 && i + 1 < argc)
         corpus = argv[++i];
      else if (strcmp(argv[i], "--matrix") == 0 && i + 1 < argc)
         matrix = argv[++i];
   }

   /* Single explicit arm. */
   if (arm)
   {
      cJSON *resp = optimize_run_arm(suite, arm, corpus, matrix);
      if (!resp)
         return 1;
      if (json_output)
         optimize_print_json(resp);
      else
      {
         printf("Suite %s, arm %s:\n", suite, arm);
         optimize_print_arm_metrics(arm, resp);
      }
      cJSON_Delete(resp);
      return 0;
   }

   /* No arm: run baseline vs on and rank. */
   cJSON *base = optimize_run_arm(suite, "baseline", corpus, matrix);
   if (!base)
      return 1;
   cJSON *on = optimize_run_arm(suite, "on", corpus, matrix);
   if (!on)
   {
      cJSON_Delete(base);
      return 1;
   }
   if (json_output)
   {
      cJSON *out = cJSON_CreateObject();
      cJSON_AddItemToObject(out, "baseline", cJSON_Duplicate(base, 1));
      cJSON_AddItemToObject(out, "on", cJSON_Duplicate(on, 1));
      optimize_print_json(out);
      cJSON_Delete(out);
   }
   else
   {
      double b = opt_metric(base, "ndcg_10"), o = opt_metric(on, "ndcg_10");
      printf("Suite %s (ranking by ndcg@10):\n", suite);
      optimize_print_arm_metrics("baseline", base);
      optimize_print_arm_metrics("on", on);
      printf("  winner: %s (ndcg@10 %+.4f)\n", o >= b ? "on" : "baseline", o - b);
   }
   cJSON_Delete(base);
   cJSON_Delete(on);
   return 0;
}

/* aimee optimize compare --baseline A --candidate B [--suite S]:
 * run two arms and show the per-metric delta (candidate - baseline). */
static int optimize_cmd_compare(int argc, char **argv, int json_output)
{
   const char *suite = "code-graph-fusion";
   const char *base_arm = NULL, *cand_arm = NULL, *corpus = NULL, *matrix = NULL;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--suite") == 0 && i + 1 < argc)
         suite = argv[++i];
      else if (strcmp(argv[i], "--baseline") == 0 && i + 1 < argc)
         base_arm = argv[++i];
      else if (strcmp(argv[i], "--candidate") == 0 && i + 1 < argc)
         cand_arm = argv[++i];
      else if (strcmp(argv[i], "--corpus") == 0 && i + 1 < argc)
         corpus = argv[++i];
      else if (strcmp(argv[i], "--matrix") == 0 && i + 1 < argc)
         matrix = argv[++i];
   }
   if (!base_arm || !cand_arm)
   {
      fprintf(stderr, "usage: aimee optimize compare --baseline <arm> --candidate <arm> "
                      "[--suite <s>]\n");
      return 2;
   }

   cJSON *base = optimize_run_arm(suite, base_arm, corpus, matrix);
   if (!base)
      return 1;
   cJSON *cand = optimize_run_arm(suite, cand_arm, corpus, matrix);
   if (!cand)
   {
      cJSON_Delete(base);
      return 1;
   }

   if (json_output)
   {
      cJSON *out = cJSON_CreateObject();
      cJSON_AddItemToObject(out, "baseline", cJSON_Duplicate(base, 1));
      cJSON_AddItemToObject(out, "candidate", cJSON_Duplicate(cand, 1));
      optimize_print_json(out);
      cJSON_Delete(out);
   }
   else
   {
      static const char *keys[] = {"ndcg_10", "mrr", "recall_10", "ndcg_5", "recall_5", NULL};
      printf("Suite %s: %s (baseline) vs %s (candidate)\n", suite, base_arm, cand_arm);
      for (int i = 0; keys[i]; i++)
      {
         double b = opt_metric(base, keys[i]), c = opt_metric(cand, keys[i]);
         printf("  %-10s %.4f -> %.4f  (%+.4f)\n", keys[i], b, c, c - b);
      }
   }
   cJSON_Delete(base);
   cJSON_Delete(cand);
   return 0;
}

static void optimize_usage(void)
{
   fprintf(stderr,
           "Usage: aimee optimize <subcommand> [options]\n\nSubcommands:\n"
           "  points                              List registered decision points\n"
           "  baseline --point <name>             Show current arm posteriors for a point\n"
           "  replay --point <name>               Emit a point's closed-decision log for replay\n"
           "  replay-record --point <name> --file <f>  Record a replay result (benchmark_trace)\n"
           "  run [--suite <s>] [--arm <a>]       Run the offline benchmark suite (ranks baseline vs on)\n"
           "  compare --baseline <a> --candidate <b> [--suite <s>]  Per-metric delta between two arms\n");
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
   if (strcmp(sub, "replay-record") == 0)
      return optimize_cmd_replay_record(argc - 1, argv + 1, json_output);
   if (strcmp(sub, "run") == 0)
      return optimize_cmd_run(argc - 1, argv + 1, json_output);
   if (strcmp(sub, "compare") == 0)
      return optimize_cmd_compare(argc - 1, argv + 1, json_output);
   if (strcmp(sub, "--help") == 0 || strcmp(sub, "-h") == 0 || strcmp(sub, "help") == 0)
   {
      optimize_usage();
      return 0;
   }
   fprintf(stderr, "Unknown optimize subcommand: %s\n", sub);
   optimize_usage();
   return 2;
}
