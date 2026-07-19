/* _GNU_SOURCE: strcasestr/memmem are GNU extensions; declare them before any
 * libc header so gcc-12 (the container toolchain) does not implicit-decl + -Werror. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* agent_eval.c: eval harness, feedback loop, and core retrieval metrics. */
/* agent_eval_load_tasks() is implemented in posix/agent_eval.c (POSIX) and
 * windows/agent_eval.c (Windows stub). */
#include "aimee.h"
#include "db1.h"
#include "agent_eval.h"
#include "agent_eval_internal.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "config.h"
#include "memory.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *const k_ablation_presets[] = {
    "full", "no_rescue", "no_respond", "no_sampling", "no_normalize", "no_retry", "bare", NULL};

int agent_eval_ablation_preset(const char *preset, agent_ablation_flags_t *out)
{
   if (!out)
      return -1;
   const char *name = (preset && preset[0]) ? preset : "full";
   memset(out, 0, sizeof(*out));
   out->configured = 1;
   out->rescue = 1;
   out->respond_tool = 1;
   out->sampling_defaults = 1;
   out->normalize = 1;
   out->retry = 1;

   if (strcmp(name, "full") == 0)
      return 0;
   if (strcmp(name, "no_rescue") == 0)
      out->rescue = 0;
   else if (strcmp(name, "no_respond") == 0)
      out->respond_tool = 0;
   else if (strcmp(name, "no_sampling") == 0)
      out->sampling_defaults = 0;
   else if (strcmp(name, "no_normalize") == 0)
      out->normalize = 0;
   else if (strcmp(name, "no_retry") == 0)
      out->retry = 0;
   else if (strcmp(name, "bare") == 0)
   {
      out->rescue = 0;
      out->respond_tool = 0;
      out->sampling_defaults = 0;
      out->normalize = 0;
      out->retry = 0;
   }
   else
   {
      memset(out, 0, sizeof(*out));
      return -1;
   }
   return 0;
}

static int eval_check_success(const eval_task_t *task, const agent_result_t *result)
{
   if (!result->success || !result->response)
      return 0;

   if (task->success_check_type[0] == '\0')
      return result->success; /* no check defined, use agent success */

   if (strcmp(task->success_check_type, "contains") == 0)
      return strstr(result->response, task->success_check_value) != NULL;

   if (strcmp(task->success_check_type, "exit_code") == 0)
      return result->success; /* agent already checks exit codes */

   return result->success;
}

static int eval_tool_failures(const agent_result_t *result, int passed)
{
   if (!result || result->tool_calls <= 0 || passed)
      return 0;
   return result->tool_calls;
}

static double eval_tool_success_rate(const agent_result_t *result, int failures)
{
   if (!result || result->tool_calls <= 0)
      return 1.0;
   int ok = result->tool_calls - failures;
   if (ok < 0)
      ok = 0;
   return (double)ok / (double)result->tool_calls;
}

static void eval_store_result(const char *suite, const eval_task_t *task,
                              const agent_result_t *result, int passed, const char *ablation,
                              unsigned int seed)
{
   int tool_failures = eval_tool_failures(result, passed);
   db1_eval_result_row_t row = {
       .suite = suite,
       .task_name = task->name,
       .agent_name = result->agent_name,
       .ablation = ablation ? ablation : "full",
       .success = passed,
       .turns = result->turns,
       .tool_calls = result->tool_calls,
       .tool_call_failures = tool_failures,
       .rescue_recoveries = result->rescue_recoveries,
       .prompt_tokens = result->prompt_tokens,
       .completion_tokens = result->completion_tokens,
       .latency_ms = result->latency_ms,
       .response = result->response,
       .error = result->error[0] ? result->error : NULL,
       .dataset_hash = "",
       .target_hash = "",
       .harness_version = "1",
       .hardware_profile = "",
       .seed = (int)seed,
   };
   (void)db1_eval_result_insert(&row);
}

/* Emit a hard-negative artefact when an eval task fails: write a JSONL record
 * with the task name, expected answer, agent response, and top memory candidates
 * (with score breakdowns) to the configured log file. */
static void eval_log_hard_negative(const char *suite_name, const eval_task_t *task,
                                   const agent_result_t *result)
{
   config_t cfg;
   if (config_load(&cfg) != 0 || !cfg.memory_hard_negative_log[0])
      return;

   FILE *fp = fopen(cfg.memory_hard_negative_log, "a");
   if (!fp)
      return;

   /* Fetch top memory candidates for the failed query */
   memory_t candidates[8];
   int cand_count = 0;
   if (task->prompt[0])
      cand_count = memory_find_facts(task->prompt, 5, candidates, 8);
   const char *memory_error = NULL;
   if (cand_count < 0)
   {
      cand_count = 0;
      memory_error = "memory retrieval index unavailable";
   }

   cJSON *rec = cJSON_CreateObject();
   if (!rec)
   {
      fclose(fp);
      return;
   }

   char ts[32];
   time_t now = time(NULL);
   struct tm tm_buf;
   gmtime_r(&now, &tm_buf);
   strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

   cJSON_AddStringToObject(rec, "ts", ts);
   cJSON_AddStringToObject(rec, "suite", suite_name ? suite_name : "");
   cJSON_AddStringToObject(rec, "task", task->name);
   cJSON_AddStringToObject(rec, "query", task->prompt);
   cJSON_AddStringToObject(rec, "expected",
                           task->success_check_value[0] ? task->success_check_value : "");
   cJSON_AddStringToObject(rec, "got", result->response ? result->response : "");
   cJSON_AddStringToObject(rec, "error", result->error[0] ? result->error : "");
   if (memory_error)
      cJSON_AddStringToObject(rec, "memory_error", memory_error);

   cJSON *cands = cJSON_CreateArray();
   for (int i = 0; i < cand_count; i++)
   {
      cJSON *c = cJSON_CreateObject();
      cJSON_AddNumberToObject(c, "id", (double)candidates[i].id);
      cJSON_AddStringToObject(c, "key", candidates[i].key);
      cJSON_AddStringToObject(c, "content", candidates[i].content);
      cJSON_AddNumberToObject(c, "confidence", candidates[i].confidence);
      cJSON_AddNumberToObject(c, "salience", candidates[i].salience);
      cJSON_AddStringToObject(c, "tier", candidates[i].tier);
      cJSON_AddStringToObject(c, "kind", candidates[i].kind);
      cJSON_AddItemToArray(cands, c);
   }
   cJSON_AddItemToObject(rec, "top_candidates", cands);

   char *line = cJSON_PrintUnformatted(rec);
   if (line)
   {
      fprintf(fp, "%s\n", line);
      free(line);
   }
   cJSON_Delete(rec);
   fclose(fp);
}

int agent_eval_run_with_options(agent_config_t *cfg, const char *suite_dir,
                                const agent_eval_run_options_t *options, eval_result_t *results,
                                int max_results)
{
   if (!cfg || !suite_dir)
      return 0;

   eval_task_t tasks[AGENT_MAX_EVAL_TASKS];
   int task_count = agent_eval_load_tasks(suite_dir, tasks, AGENT_MAX_EVAL_TASKS);
   if (task_count == 0)
      return 0;

   /* Extract suite name from directory path */
   const char *suite_name = strrchr(suite_dir, '/');
   suite_name = suite_name ? suite_name + 1 : suite_dir;

   const char *requested =
       (options && options->ablation && options->ablation[0]) ? options->ablation : "full";
   if (strcmp(requested, "all") != 0)
   {
      agent_ablation_flags_t check_flags;
      if (agent_eval_ablation_preset(requested, &check_flags) != 0)
         return 0;
   }
   int runs = (options && options->runs > 0) ? options->runs : 1;
   unsigned int seed = options ? options->seed : 0;

   int passes = 0;
   int out_index = 0;
   for (int pi = 0; k_ablation_presets[pi]; pi++)
   {
      const char *preset = k_ablation_presets[pi];
      if (strcmp(requested, "all") != 0 && strcmp(requested, preset) != 0)
         continue;

      agent_ablation_flags_t flags;
      if (agent_eval_ablation_preset(preset, &flags) != 0)
         continue;

      for (int run = 0; run < runs; run++)
      {
         cfg->ablation = flags;
         for (int i = 0; i < task_count; i++)
         {
            agent_result_t ar;
            int rc = agent_run(cfg, tasks[i].role, NULL, tasks[i].prompt, 0, &ar);

            int passed = (rc == 0) && eval_check_success(&tasks[i], &ar);
            if (passed && tasks[i].max_latency_ms > 0 && ar.latency_ms > tasks[i].max_latency_ms)
               passed = 0;
            int tool_failures = eval_tool_failures(&ar, passed);

            eval_store_result(suite_name, &tasks[i], &ar, passed, preset, seed + (unsigned int)run);

            /* Emit hard-negative artefact on failure */
            if (!passed)
               eval_log_hard_negative(suite_name, &tasks[i], &ar);

            if (results && out_index < max_results)
            {
               eval_result_t *r = &results[out_index];
               memset(r, 0, sizeof(*r));
               snprintf(r->task_name, sizeof(r->task_name), "%s", tasks[i].name);
               snprintf(r->agent_name, sizeof(r->agent_name), "%s", ar.agent_name);
               snprintf(r->ablation, sizeof(r->ablation), "%s", preset);
               r->success = passed;
               r->turns = ar.turns;
               r->tool_calls = ar.tool_calls;
               r->tool_call_failures = tool_failures;
               r->rescue_recoveries = ar.rescue_recoveries;
               r->tool_call_success_rate = eval_tool_success_rate(&ar, tool_failures);
               r->prompt_tokens = ar.prompt_tokens;
               r->completion_tokens = ar.completion_tokens;
               r->latency_ms = ar.latency_ms;
               if (ar.error[0])
                  snprintf(r->error, sizeof(r->error), "%s", ar.error);
            }
            out_index++;

            if (passed)
               passes++;
            free(ar.response);
         }
      }
   }

   memset(&cfg->ablation, 0, sizeof(cfg->ablation));
   return passes;
}

int eval_feedback_loop(void)
{
   /* All reads/writes go through DB1-typed helpers (db1_eval_*,
    * db2_rules_*, db2_agent_outcome_*, db2_anti_pattern_*) which
    * reach the owning tier internally; no caller-owned connection is needed. */

   int adjustments = 0;

   /* Find recently failed eval tasks and reinforce matching rules */
   db1_eval_failed_task_t fails[20];
   int n_fail = db1_eval_failed_tasks_recent(fails, 20);
   for (int f = 0; f < n_fail; f++)
   {
      const char *task_name = fails[f].task_name;
      const char *error = fails[f].error;
      if (!task_name || !task_name[0])
         continue;

      /* Search for rules whose title words overlap with the failed task */
      rule_t rules[32];
      int rcount = db2_rules_list(rules, 32);
      for (int i = 0; i < rcount; i++)
      {
         /* Check if rule title words appear in task name or error */
         char title_copy[256];
         snprintf(title_copy, sizeof(title_copy), "%s", rules[i].title);

         char *saveptr = NULL;
         char *word = strtok_r(title_copy, " \t", &saveptr);
         int matched = 0;
         int total = 0;

         while (word)
         {
            if (strlen(word) > 3)
            {
               total++;
               if ((task_name && strcasestr(task_name, word)) || (error && strcasestr(error, word)))
                  matched++;
            }
            word = strtok_r(NULL, " \t", &saveptr);
         }

         /* If >50% of rule words match the failure, bump weight */
         if (total > 0 && matched * 2 >= total && rules[i].weight < 100)
         {
            int new_weight = rules[i].weight + 10;
            if (new_weight > 100)
               new_weight = 100;
            db2_rules_update_weight(rules[i].id, new_weight);
            adjustments++;
         }
      }
   }

   /* Reinforce rules that correlate with recent successes */
   db1_eval_passed_task_t passes[20];
   int n_pass = db1_eval_passed_tasks_recent(passes, 20);
   for (int p = 0; p < n_pass; p++)
   {
      const char *task_name = passes[p].task_name;
      if (!task_name || !task_name[0])
         continue;

      rule_t rules[32];
      int rcount = db2_rules_list(rules, 32);
      for (int i = 0; i < rcount; i++)
      {
         char title_copy[256];
         snprintf(title_copy, sizeof(title_copy), "%s", rules[i].title);

         char *saveptr = NULL;
         char *word = strtok_r(title_copy, " \t", &saveptr);
         int matched = 0;
         int total = 0;

         while (word)
         {
            if (strlen(word) > 3)
            {
               total++;
               if (strcasestr(task_name, word))
                  matched++;
            }
            word = strtok_r(NULL, " \t", &saveptr);
         }

         /* Successful task correlated with rule: small reinforcement */
         if (total > 0 && matched * 2 >= total && rules[i].weight < 100)
         {
            int new_weight = rules[i].weight + 5;
            if (new_weight > 100)
               new_weight = 100;
            db2_rules_update_weight(rules[i].id, new_weight);
            adjustments++;
         }
      }
   }

   /* Also process agent_outcomes: failures and errors reinforce rules */
   db2_agent_outcome_failure_t outcome_fails[20];
   int outcome_fail_count = db2_agent_outcome_recent_failures(7, outcome_fails, 20);
   for (int f = 0; f < outcome_fail_count; f++)
   {
      const char *orole = outcome_fails[f].role;
      const char *reason = outcome_fails[f].reason;
      if (!reason[0])
         continue;

      rule_t rules[32];
      int rcount = db2_rules_list(rules, 32);
      for (int i = 0; i < rcount; i++)
      {
         char title_copy[256];
         snprintf(title_copy, sizeof(title_copy), "%s", rules[i].title);

         char *saveptr = NULL;
         char *word = strtok_r(title_copy, " \t", &saveptr);
         int matched = 0;
         int total = 0;

         while (word)
         {
            if (strlen(word) > 3)
            {
               total++;
               if ((orole[0] && strcasestr(orole, word)) || strcasestr(reason, word))
                  matched++;
            }
            word = strtok_r(NULL, " \t", &saveptr);
         }

         if (total > 0 && matched * 2 >= total && rules[i].weight < 100)
         {
            int new_weight = rules[i].weight + 10;
            if (new_weight > 100)
               new_weight = 100;
            db2_rules_update_weight(rules[i].id, new_weight);
            adjustments++;
         }
      }
   }

   /* Auto-extract anti-patterns from repeated tool error patterns.
    * If the same tool_error_pattern appears in 3+ outcomes, create an anti-pattern. */
   db2_agent_outcome_pattern_count_t patterns[32];
   int pat_count = db2_agent_outcome_repeated_error_patterns(3, patterns, 32);
   for (int p = 0; p < pat_count; p++)
   {
      const char *pattern = patterns[p].pattern;
      if (!pattern[0])
         continue;

      /* Check if this anti-pattern already exists */
      anti_pattern_t existing[1];
      int existing_count = db2_anti_pattern_check(NULL, pattern, existing, 1);
      if (existing_count == 0)
      {
         char desc[512];
         snprintf(desc, sizeof(desc), "Auto-detected from %d agent failures: %s", patterns[p].count,
                  pattern);
         db2_anti_pattern_insert(pattern, desc, "auto-outcome", "", 0.7, NULL);
      }
   }
   return adjustments;
}

/* --- IR Metrics --- */

#include <math.h>

static int is_relevant(int64_t id, const int64_t *relevant, int n_relevant)
{
   for (int i = 0; i < n_relevant; i++)
      if (relevant[i] == id)
         return 1;
   return 0;
}

double ir_mrr(const int64_t *retrieved, int n_retrieved, const int64_t *relevant, int n_relevant)
{
   for (int i = 0; i < n_retrieved; i++)
   {
      if (is_relevant(retrieved[i], relevant, n_relevant))
         return 1.0 / (i + 1);
   }
   return 0.0;
}

double ir_ndcg_at_k(const int64_t *retrieved, int n_retrieved, const int64_t *relevant,
                    int n_relevant, int k)
{
   if (k <= 0 || n_relevant == 0)
      return 0.0;
   int limit = n_retrieved < k ? n_retrieved : k;

   /* DCG */
   double dcg = 0.0;
   for (int i = 0; i < limit; i++)
   {
      if (is_relevant(retrieved[i], relevant, n_relevant))
         dcg += 1.0 / log2(i + 2.0);
   }

   /* Ideal DCG */
   int ideal_limit = n_relevant < k ? n_relevant : k;
   double idcg = 0.0;
   for (int i = 0; i < ideal_limit; i++)
      idcg += 1.0 / log2(i + 2.0);

   return idcg > 0.0 ? dcg / idcg : 0.0;
}

double ir_recall_at_k(const int64_t *retrieved, int n_retrieved, const int64_t *relevant,
                      int n_relevant, int k)
{
   if (n_relevant == 0)
      return 0.0;
   int limit = n_retrieved < k ? n_retrieved : k;
   int found = 0;
   for (int i = 0; i < limit; i++)
   {
      if (is_relevant(retrieved[i], relevant, n_relevant))
         found++;
   }
   return (double)found / n_relevant;
}

/* --- Memory Retrieval Eval --- */

int mem_eval_run(mem_eval_case_t *cases, int n_cases, mem_eval_scores_t *out)
{
   return mem_eval_run_with_latency(cases, n_cases, out, NULL);
}

double elapsed_ms(const struct timespec *start, const struct timespec *end);

static int mem_eval_find_rank(const int64_t *retrieved, int n_retrieved, const int64_t *relevant,
                              int n_relevant)
{
   for (int i = 0; i < n_retrieved; i++)
   {
      for (int j = 0; j < n_relevant; j++)
      {
         if (retrieved[i] == relevant[j])
            return i + 1;
      }
   }
   return 0;
}

static int mem_eval_query_has_temporal_intent(const char *query)
{
   if (!query || !query[0])
      return 0;
   return strstr(query, "when") != NULL || strstr(query, "date") != NULL ||
          strstr(query, "before") != NULL || strstr(query, "after") != NULL ||
          strstr(query, "year") != NULL || strstr(query, "month") != NULL ||
          strstr(query, "today") != NULL || strstr(query, "yesterday") != NULL;
}

static const char *mem_eval_bucket_failure(const char *query, const memory_diagnostic_t *expected)
{
   if (!expected)
      return "missing";
   if (mem_eval_query_has_temporal_intent(query) && expected->parts.temporal <= 0.0 &&
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

static void mem_eval_increment_bucket(const char *bucket, int *temporal_miss, int *entity_miss,
                                      int *lexical_gap, int *semantic_gap, int *state_penalty,
                                      int *granularity_gap, int *ranking_gap, int *missing)
{
   if (!bucket)
   {
      (*missing)++;
      return;
   }
   if (strcmp(bucket, "temporal_miss") == 0)
      (*temporal_miss)++;
   else if (strcmp(bucket, "entity_miss") == 0)
      (*entity_miss)++;
   else if (strcmp(bucket, "lexical_gap") == 0)
      (*lexical_gap)++;
   else if (strcmp(bucket, "semantic_gap") == 0)
      (*semantic_gap)++;
   else if (strcmp(bucket, "state_penalty") == 0)
      (*state_penalty)++;
   else if (strcmp(bucket, "granularity_gap") == 0)
      (*granularity_gap)++;
   else if (strcmp(bucket, "ranking_gap") == 0)
      (*ranking_gap)++;
   else
      (*missing)++;
}

static void mem_eval_append_miss_progress_row(FILE *fp, const char *dataset, const char *query,
                                              int cases_scanned, int misses_found, int is_miss,
                                              int rank, const char *bucket)
{
   if (!fp || !dataset || !query)
      return;

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "dataset", dataset);
   cJSON_AddStringToObject(root, "phase", "miss_report");
   cJSON_AddStringToObject(root, "query", query);
   cJSON_AddNumberToObject(root, "cases_scanned", cases_scanned);
   cJSON_AddNumberToObject(root, "misses_found", misses_found);
   cJSON_AddBoolToObject(root, "is_miss", is_miss != 0);
   if (rank > 0)
      cJSON_AddNumberToObject(root, "rank", rank);
   else
      cJSON_AddStringToObject(root, "rank", "missing");
   cJSON_AddStringToObject(root, "bucket", bucket ? bucket : "hit");

   char *line = cJSON_PrintUnformatted(root);
   if (line)
   {
      fprintf(fp, "%s\n", line);
      fflush(fp);
      free(line);
   }
   cJSON_Delete(root);
}

static void mem_eval_bucket_add(mem_eval_bucket_scores_t *bucket, double metric_a, double metric_b)
{
   if (!bucket)
      return;
   bucket->cases++;
   bucket->metric_a += metric_a;
   bucket->metric_b += metric_b;
}

static void mem_eval_bucket_finalize(mem_eval_bucket_scores_t *bucket)
{
   if (!bucket || bucket->cases <= 0)
      return;
   bucket->metric_a /= bucket->cases;
   bucket->metric_b /= bucket->cases;
}

static void mem_eval_finalize_bucket_array(mem_eval_bucket_scores_t *buckets, int count)
{
   if (!buckets || count <= 0)
      return;
   for (int i = 0; i < count; i++)
      mem_eval_bucket_finalize(&buckets[i]);
}

static int mem_eval_route_bucket_index(memory_query_route_t route)
{
   switch (route)
   {
   case MEM_ROUTE_LEXICAL:
      return 0;
   case MEM_ROUTE_SEMANTIC:
      return 1;
   case MEM_ROUTE_GRAPH:
      return 2;
   case MEM_ROUTE_HYBRID:
   default:
      return 3;
   }
}

static int mem_eval_shape_bucket_index(memory_query_shape_t shape)
{
   switch (shape)
   {
   case MEM_SHAPE_UNKNOWN:
      return 0;
   case MEM_SHAPE_FACTOID:
      return 1;
   case MEM_SHAPE_LIST:
      return 2;
   case MEM_SHAPE_YES_NO:
      return 3;
   case MEM_SHAPE_WHEN:
      return 4;
   case MEM_SHAPE_HOW:
      return 5;
   case MEM_SHAPE_WHY:
      return 6;
   case MEM_SHAPE_QUANTITATIVE:
      return 7;
   case MEM_SHAPE_TEMPORAL_INTERVAL:
      return 8;
   default:
      return 0;
   }
}

void mem_eval_print_miss_report(FILE *fp, mem_eval_case_t *cases, int n_cases, int limit,
                                int max_misses, int *reported_io, int *misses_io,
                                int *temporal_miss_io, int *entity_miss_io, int *lexical_gap_io,
                                int *semantic_gap_io, int *state_penalty_io,
                                int *granularity_gap_io, int *ranking_gap_io, int *missing_io,
                                FILE *progress_fp, const char *dataset, int *cases_scanned_io)
{
   if (!fp || !cases || n_cases <= 0)
      return;

   for (int c = 0; c < n_cases; c++)
   {
      memory_t results[20];
      int n_results = memory_find_facts(cases[c].query, 20, results, 20);
      if (n_results < 0)
      {
         fprintf(fp,
                 "ERROR query=%s memory retrieval index unavailable; server-side maintenance is "
                 "required\n",
                 cases[c].query);
         return;
      }
      int64_t retrieved[20];
      for (int i = 0; i < n_results; i++)
         retrieved[i] = results[i].id;
      int rank =
          mem_eval_find_rank(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected);
      if (cases_scanned_io)
         (*cases_scanned_io)++;
      if (rank > 0 && rank <= limit)
      {
         mem_eval_append_miss_progress_row(progress_fp, dataset ? dataset : "unknown",
                                           cases[c].query, cases_scanned_io ? *cases_scanned_io : 0,
                                           misses_io ? *misses_io : 0, 0, rank, "hit");
         continue;
      }

      (*misses_io)++;
      int64_t expected_id = cases[c].expected_ids[0];
      memory_diagnostic_t expected;
      const char *bucket = "missing";
      memory_t expected_mem;
      memset(&expected, 0, sizeof(expected));
      memset(&expected_mem, 0, sizeof(expected_mem));

      if (expected_id > 0 && memory_explain_match(cases[c].query, expected_id, &expected) == 0)
      {
         bucket = mem_eval_bucket_failure(cases[c].query, &expected);
         expected_mem = expected.memory;
      }
      else if (expected_id > 0)
      {
         (void)memory_get(expected_id, &expected_mem);
      }

      mem_eval_increment_bucket(bucket, temporal_miss_io, entity_miss_io, lexical_gap_io,
                                semantic_gap_io, state_penalty_io, granularity_gap_io,
                                ranking_gap_io, missing_io);
      mem_eval_append_miss_progress_row(progress_fp, dataset ? dataset : "unknown", cases[c].query,
                                        cases_scanned_io ? *cases_scanned_io : 0,
                                        misses_io ? *misses_io : 0, 1, rank, bucket);

      if (*reported_io >= max_misses)
         continue;

      if (rank > 0)
         fprintf(fp, "MISS rank=%d bucket=%s query=%s\n", rank, bucket, cases[c].query);
      else
         fprintf(fp, "MISS rank=missing bucket=%s query=%s\n", bucket, cases[c].query);
      if (expected_mem.id > 0)
      {
         fprintf(fp, "  expected_id=%lld key=%s\n", (long long)expected_mem.id, expected_mem.key);
         fprintf(fp, "  expected_content=%s\n", expected_mem.content);
      }
      if (n_results > 0)
      {
         fprintf(fp, "  top_results:");
         int shown = n_results < 5 ? n_results : 5;
         for (int i = 0; i < shown; i++)
            fprintf(fp, " [%d]%lld:%s", i + 1, (long long)results[i].id, results[i].key);
         fprintf(fp, "\n");
      }
      (*reported_io)++;
   }
}

double elapsed_ms(const struct timespec *start, const struct timespec *end)
{
   long sec = end->tv_sec - start->tv_sec;
   long nsec = end->tv_nsec - start->tv_nsec;
   if (nsec < 0)
   {
      sec--;
      nsec += 1000000000L;
   }
   return (double)sec * 1000.0 + (double)nsec / 1000000.0;
}

void mem_eval_latency_finalize(const double *samples, int n_samples, mem_eval_latency_t *out)
{
   if (!out)
      return;

   memset(out, 0, sizeof(*out));
   out->n_queries = n_samples;
   if (!samples || n_samples <= 0)
      return;

   double *sorted = malloc((size_t)n_samples * sizeof(double));
   if (!sorted)
      return;
   memcpy(sorted, samples, (size_t)n_samples * sizeof(double));
   for (int i = 0; i < n_samples - 1; i++)
   {
      for (int j = i + 1; j < n_samples; j++)
      {
         if (sorted[j] < sorted[i])
         {
            double tmp = sorted[i];
            sorted[i] = sorted[j];
            sorted[j] = tmp;
         }
      }
   }

   out->min_ms = sorted[0];
   out->max_ms = sorted[n_samples - 1];
   out->p50_ms = sorted[n_samples * 50 / 100];
   out->p95_ms = sorted[n_samples * 95 / 100];
   out->p99_ms = sorted[n_samples * 99 / 100];
   free(sorted);
}

int mem_eval_run_with_latency(mem_eval_case_t *cases, int n_cases, mem_eval_scores_t *out,
                              mem_eval_latency_t *latency_out)
{
   if (!cases || !out || n_cases <= 0)
      return -1;

   memset(out, 0, sizeof(*out));
   out->n_cases = n_cases;
   if (latency_out)
      memset(latency_out, 0, sizeof(*latency_out));

   double total_mrr = 0, total_ndcg5 = 0, total_ndcg10 = 0;
   double total_recall5 = 0, total_recall10 = 0;
   double *latencies = latency_out ? calloc((size_t)n_cases, sizeof(double)) : NULL;

   for (int c = 0; c < n_cases; c++)
   {
      memory_query_plan_t plan;
      memset(&plan, 0, sizeof(plan));
      (void)memory_query_plan(cases[c].query, 10, 96, &plan);
      int route_bucket = mem_eval_route_bucket_index(plan.route);
      int shape_bucket = mem_eval_shape_bucket_index(plan.shape);

      /* Run memory search */
      struct timespec ts0, ts1;
      if (latency_out)
         clock_gettime(CLOCK_MONOTONIC, &ts0);
      memory_t results[20];
      int n_results = memory_find_facts(cases[c].query, 20, results, 20);
      if (n_results < 0)
      {
         free(latencies);
         return -1;
      }
      if (latency_out)
      {
         clock_gettime(CLOCK_MONOTONIC, &ts1);
         latencies[c] = elapsed_ms(&ts0, &ts1);
      }

      /* Extract retrieved IDs */
      int64_t retrieved[20];
      memset(retrieved, 0, sizeof(retrieved));
      for (int i = 0; i < n_results; i++)
         retrieved[i] = results[i].id;

      double case_mrr = ir_mrr(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected);
      double case_recall5 =
          ir_recall_at_k(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected, 5);
      double case_recall10 =
          ir_recall_at_k(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected, 10);
      total_mrr += case_mrr;
      total_ndcg5 +=
          ir_ndcg_at_k(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected, 5);
      total_ndcg10 +=
          ir_ndcg_at_k(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected, 10);
      total_recall5 += case_recall5;
      total_recall10 += case_recall10;
      if (route_bucket >= 0 && route_bucket < MEM_EVAL_ROUTE_BUCKET_COUNT)
         mem_eval_bucket_add(&out->route_buckets[route_bucket], case_mrr, case_recall10);
      if (shape_bucket >= 0 && shape_bucket < MEM_EVAL_SHAPE_BUCKET_COUNT)
         mem_eval_bucket_add(&out->shape_buckets[shape_bucket], case_mrr, case_recall10);
   }

   out->mrr = total_mrr / n_cases;
   out->ndcg_5 = total_ndcg5 / n_cases;
   out->ndcg_10 = total_ndcg10 / n_cases;
   out->recall_5 = total_recall5 / n_cases;
   out->recall_10 = total_recall10 / n_cases;
   mem_eval_finalize_bucket_array(out->route_buckets, MEM_EVAL_ROUTE_BUCKET_COUNT);
   mem_eval_finalize_bucket_array(out->shape_buckets, MEM_EVAL_SHAPE_BUCKET_COUNT);
   if (latency_out)
      mem_eval_latency_finalize(latencies, n_cases, latency_out);
   free(latencies);

   return 0;
}

/* Corpus/QA benchmark helpers live in agent_eval_memory_support.c. */
