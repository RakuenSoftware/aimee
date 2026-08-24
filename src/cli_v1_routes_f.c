/* ===================================================================
 * /v1 thin-client routing: the eval.* family — its marshallers and its
 * printers — moved out of cli_v1_routes_b.c and cli_v1_routes_c.c, which had
 * both reached the 2500-line hard limit when the regression-candidate surface
 * landed. Split by command family rather than at an arbitrary cut, the same
 * way cli_v1_routes_e.c holds memory.*, so an eval change stays in one file.
 * =================================================================== */

#include "cli_v1_routes_internal.h"
#include "cJSON.h"
#include "cli_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Unconditional, as in every sibling cli_v1_routes*.c: MinGW supplies unistd.h
 * and getcwd, and guarding it out on Windows removed the declaration while
 * leaving the call -- which is exactly how this file broke the Windows build. */
#include <unistd.h>

cJSON *marshal_eval_run(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("eval.run");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "suite_dir", opts.positional[0]);
   const char *ablation = cli_args_get(&opts, "ablation");
   if (ablation)
      cJSON_AddStringToObject(req, "ablation", ablation);
   const char *runs = cli_args_get(&opts, "runs");
   if (runs)
      cJSON_AddNumberToObject(req, "runs", atoi(runs));
   const char *seed = cli_args_get(&opts, "seed");
   if (seed)
      cJSON_AddNumberToObject(req, "seed", strtoul(seed, NULL, 10));
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   return req;
}

cJSON *marshal_eval_results(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("eval.results");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "suite", opts.positional[0]);
   return req;
}

cJSON *marshal_eval_candidates(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("eval.candidates");
   const char *state = cli_args_get(&opts, "state");
   if (state)
      cJSON_AddStringToObject(req, "state", state);
   const char *limit = cli_args_get(&opts, "limit");
   if (limit)
      cJSON_AddNumberToObject(req, "limit", atoi(limit));
   return req;
}

cJSON *marshal_eval_candidates_update(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("eval.candidates-update");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "op", opts.positional[0]);
   const char *dir = cli_args_get(&opts, "suite-dir");
   if (dir)
      cJSON_AddStringToObject(req, "suite_dir", dir);
   const char *suite = cli_args_get(&opts, "suite");
   if (suite)
      cJSON_AddStringToObject(req, "suite", suite);
   const char *id = cli_args_get(&opts, "id");
   if (id)
      cJSON_AddNumberToObject(req, "id", atoi(id));
   const char *reason = cli_args_get(&opts, "reason");
   if (reason)
      cJSON_AddStringToObject(req, "reason", reason);
   const char *by = cli_args_get(&opts, "by");
   if (by)
      cJSON_AddStringToObject(req, "by", by);
   const char *min_occ = cli_args_get(&opts, "min-occurrences");
   if (min_occ)
      cJSON_AddNumberToObject(req, "min_occurrences", atoi(min_occ));
   const char *windows = cli_args_get(&opts, "retire-windows");
   if (windows)
      cJSON_AddNumberToObject(req, "retire_windows", atoi(windows));
   const char *window_days = cli_args_get(&opts, "window-days");
   if (window_days)
      cJSON_AddNumberToObject(req, "window_days", atoi(window_days));
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   return req;
}

cJSON *marshal_learning_approaches(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("learning.approaches");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "goal", opts.positional[0]);
   return req;
}

void pt_print_learning_approaches(const char *method, cJSON *resp)
{
   (void)method;
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "approaches");
   if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0)
   {
      printf("No approach has failed against a goal like this one.\n");
      return;
   }
   const char *advisory = json_str(resp, "advisory");
   if (advisory && advisory[0])
      printf("%s\n\n", advisory);
   printf("%-6s %-9s %s\n", "SEEN", "SIMILAR", "APPROACH -> FAILURE");
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, arr)
   {
      cJSON *occ = cJSON_GetObjectItemCaseSensitive(it, "occurrences");
      cJSON *sim = cJSON_GetObjectItemCaseSensitive(it, "similarity");
      printf("%-6d %-9.2f %s -> %s\n", cJSON_IsNumber(occ) ? occ->valueint : 0,
             cJSON_IsNumber(sim) ? sim->valuedouble : 0.0, json_str(it, "approach"),
             json_str(it, "failure_mode"));
   }
}

cJSON *marshal_learning_attribution(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("learning.attribution");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "suite", opts.positional[0]);
   return req;
}

void pt_print_learning_attribution(const char *method, cJSON *resp)
{
   (void)method;
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "arms");
   if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0)
   {
      printf("No ablation arms recorded. Run `aimee eval run <suite> --ablation all` first.\n");
      return;
   }
   printf("Baseline: %s (a capability needs %d paired tasks to carry a claim)\n\n",
          json_str(resp, "baseline"), json_int(resp, "min_tasks", 0));
   printf("%-14s %-7s %-8s %s\n", "REMOVED", "TASKS", "DELTA", "VERDICT");
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, arr)
   {
      cJSON *tasks = cJSON_GetObjectItemCaseSensitive(it, "tasks_compared");
      cJSON *delta = cJSON_GetObjectItemCaseSensitive(it, "delta");
      int attributable = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "attributable"));
      double d = cJSON_IsNumber(delta) ? delta->valuedouble : 0.0;
      const char *verdict = !attributable ? "not enough paired runs"
                            : d > 0.0     ? "removing it cost us"
                            : d < 0.0     ? "runs were better without it"
                                          : "no measured effect";
      printf("%-14s %-7d %-+8.3f %s\n", json_str(it, "ablation"),
             cJSON_IsNumber(tasks) ? tasks->valueint : 0, d, verdict);
   }
}

cJSON *marshal_learning_resolve(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("learning.resolve");
   const char *budget = cli_args_get(&opts, "budget");
   if (budget)
      cJSON_AddNumberToObject(req, "budget", atoi(budget));
   return req;
}

void pt_print_learning_resolve(const char *method, cJSON *resp)
{
   (void)method;
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "no_probe")))
   {
      printf("No evidence probe is installed, so nothing was closed.\n"
             "Closing gaps without a way to check them would empty the backlog by assertion.\n");
      return;
   }
   printf("resolved %d of %d considered (budget %d)\n", json_int(resp, "resolved", 0),
          json_int(resp, "considered", 0), json_int(resp, "budget", 0));
   printf("  %d still open, %d undecided, %d skipped as needing a judgement\n",
          json_int(resp, "still_open", 0), json_int(resp, "unknown", 0),
          json_int(resp, "skipped", 0));
}

cJSON *marshal_learning_fate(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("learning.fate");
   if (opts.pos_count > 0)
      cJSON_AddNumberToObject(req, "id", atoi(opts.positional[0]));
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "fate", opts.positional[1]);
   const char *reason = cli_args_get(&opts, "reason");
   if (reason)
      cJSON_AddStringToObject(req, "reason", reason);
   return req;
}

void pt_print_learning_fate(const char *method, cJSON *resp)
{
   (void)method;
   int regret = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "counts_as_regret"));
   printf("proposal %d recorded as %s (%s)\n", json_int(resp, "id", 0), json_str(resp, "fate"),
          regret ? "counts against the detector that raised it" : "no regret");
}

static void print_eval_run(cJSON *resp)
{
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "results");
   printf("%-30s %-12s %-12s %-6s %-6s %-8s %-10s\n", "Task", "Agent", "Ablation", "Pass", "Turns",
          "ToolOK", "Latency");
   if (cJSON_IsArray(rows))
   {
      cJSON *row;
      cJSON_ArrayForEach(row, rows)
      {
         double tool_ok =
             cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "tool_call_success_rate"));
         printf("%-30s %-12s %-12s %-6s %-6d %-7.2f%% %-10dms\n", json_str(row, "task_name"),
                json_str(row, "agent_name"), json_str(row, "ablation"),
                json_int(row, "success", 0) ? "PASS" : "FAIL", json_int(row, "turns", 0),
                tool_ok * 100.0, json_int(row, "latency_ms", 0));
      }
   }
   printf("\n%d/%d passed.\n", json_int(resp, "passes", 0), json_int(resp, "total", 0));
}

static void print_eval_results(cJSON *resp)
{
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "results");
   if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) == 0)
   {
      printf("No eval results.\n");
      return;
   }
   printf("%-15s %-25s %-12s %-12s %-6s %-6s %-6s %-10s %s\n", "Suite", "Task", "Agent", "Ablation",
          "Pass", "Turns", "Tools", "Latency", "Time");
   cJSON *row;
   cJSON_ArrayForEach(row, rows)
   {
      printf("%-15s %-25s %-12s %-12s %-6s %-6d %-6d %-10dms %s\n", json_str(row, "suite"),
             json_str(row, "task_name"), json_str(row, "agent_name"), json_str(row, "ablation"),
             json_int(row, "success", 0) ? "PASS" : "FAIL", json_int(row, "turns", 0),
             json_int(row, "tool_calls", 0), json_int(row, "latency_ms", 0),
             json_str(row, "created_at"));
   }
}

void pt_print_eval_run(const char *method, cJSON *resp)
{
   print_eval_run(resp);
}
void pt_print_eval_results(const char *method, cJSON *resp)
{
   print_eval_results(resp);
}
void pt_print_eval_candidates(const char *method, cJSON *resp)
{
   (void)method;
   cJSON *gate = cJSON_GetObjectItemCaseSensitive(resp, "gate");
   if (gate)
   {
      const char *state = json_str(gate, "state");
      cJSON *total = cJSON_GetObjectItemCaseSensitive(gate, "committed_total");
      cJSON *ratio = cJSON_GetObjectItemCaseSensitive(gate, "exogenous_ratio");
      printf("admission gate: %s", (state && state[0]) ? state : "unknown");
      if (cJSON_IsNumber(total) && total->valuedouble > 0 && cJSON_IsNumber(ratio))
         printf(" (%.0f%% of %.0f committed proposals exogenous)", ratio->valuedouble * 100.0,
                total->valuedouble);
      else if (state && strcmp(state, "unavailable") == 0)
         printf(" (the knowledge service did not answer, so this is not a measurement)");
      else
         printf(" (no settled proposals yet)");
      printf("\n\n");
   }

   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "candidates");
   if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0)
   {
      printf("No regression candidates.\n");
      return;
   }
   printf("%-6s %-10s %-28s %5s %5s %s\n", "ID", "STATE", "TASK", "SEEN", "SESS", "ORIGIN");
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, arr)
   {
      cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
      cJSON *occ = cJSON_GetObjectItemCaseSensitive(it, "occurrences");
      cJSON *sess = cJSON_GetObjectItemCaseSensitive(it, "distinct_sessions");
      printf("%-6d %-10s %-28s %5d %5d %s\n", cJSON_IsNumber(id) ? id->valueint : 0,
             json_str(it, "state"), json_str(it, "task_name"),
             cJSON_IsNumber(occ) ? occ->valueint : 0, cJSON_IsNumber(sess) ? sess->valueint : 0,
             json_str(it, "origin_ref"));
   }
}

void pt_print_eval_candidates_update(const char *method, cJSON *resp)
{
   (void)method;
   const char *op = json_str(resp, "op");
   static const char *const counters[] = {"observed", "admitted", "retired", "rejected", NULL};
   for (int i = 0; counters[i]; i++)
   {
      cJSON *n = cJSON_GetObjectItemCaseSensitive(resp, counters[i]);
      if (cJSON_IsNumber(n))
         printf("%s: %d %s\n", (op && op[0]) ? op : "done", n->valueint, counters[i]);
   }
   cJSON *jobs = cJSON_GetObjectItemCaseSensitive(resp, "jobs_seen");
   if (cJSON_IsNumber(jobs))
   {
      cJSON *sig = cJSON_GetObjectItemCaseSensitive(resp, "signals_seen");
      cJSON *bad = cJSON_GetObjectItemCaseSensitive(resp, "rejected_text");
      printf("  scanned %d failed job(s), %d correction signal(s); %d refused for unsafe text\n",
             jobs->valueint, cJSON_IsNumber(sig) ? sig->valueint : 0,
             cJSON_IsNumber(bad) ? bad->valueint : 0);
   }
   const char *dir = json_str(resp, "suite_dir");
   if (dir && dir[0])
      printf("  suite: %s\n", dir);
}
