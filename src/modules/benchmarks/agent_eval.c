/* _GNU_SOURCE: strcasestr/memmem are GNU extensions; declare them before any
 * libc header so gcc-12 (the container toolchain) does not implicit-decl + -Werror. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* agent_eval.c: eval harness, feedback loop, and core retrieval metrics. */
/* agent_eval_load_tasks() is implemented in posix/agent_eval.c (POSIX) and
 * windows/agent_eval.c (Windows stub). */
#include "aimee.h"
#include "db1_client/db1.h"
#include "agent_eval.h"
#include "agent_eval_internal.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "config.h"
#include "memory.h"
#include "cJSON.h"
#include "module_commands.h"
#include "json_fluent.h"
#include "aimee_sha256.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>

static const char *const k_ablation_presets[] = {
    "full", "no_rescue", "no_respond", "no_sampling", "no_normalize", "no_retry", "bare", NULL};

static void eval_hardware_profile(char out[AGENT_EVAL_HARDWARE_LEN])
{
   const char *pinned = getenv("AIMEE_BENCHMARK_HARDWARE_PROFILE");
   if (pinned && pinned[0])
   {
      snprintf(out, AGENT_EVAL_HARDWARE_LEN, "%s", pinned);
      return;
   }
   struct utsname u;
   long cpus = sysconf(_SC_NPROCESSORS_ONLN);
   if (uname(&u) == 0)
      snprintf(out, AGENT_EVAL_HARDWARE_LEN, "%s/%s/cpus=%ld", u.sysname, u.machine,
               cpus > 0 ? cpus : 0);
   else
      snprintf(out, AGENT_EVAL_HARDWARE_LEN, "unknown/cpus=%ld", cpus > 0 ? cpus : 0);
}

int agent_eval_manifest_build(const char *suite, const eval_task_t *tasks, int task_count,
                              const char *agent_name, unsigned int seed, agent_eval_manifest_t *out)
{
   if (!out || !tasks || task_count <= 0 || !agent_name || !agent_name[0])
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *dataset = cJSON_CreateObject();
   cJSON *items = dataset ? cJSON_AddArrayToObject(dataset, "tasks") : NULL;
   if (!dataset || !items)
   {
      cJSON_Delete(dataset);
      return -1;
   }
   cJSON_AddStringToObject(dataset, "suite", suite ? suite : "");
   for (int i = 0; i < task_count; i++)
   {
      cJSON *item = cJSON_CreateObject();
      if (!item)
      {
         cJSON_Delete(dataset);
         return -1;
      }
      cJSON_AddStringToObject(item, "name", tasks[i].name);
      cJSON_AddStringToObject(item, "prompt", tasks[i].prompt);
      cJSON_AddStringToObject(item, "role", tasks[i].role);
      cJSON_AddStringToObject(item, "success_check_type", tasks[i].success_check_type);
      cJSON_AddStringToObject(item, "success_check_value", tasks[i].success_check_value);
      cJSON_AddNumberToObject(item, "max_turns", tasks[i].max_turns);
      cJSON_AddNumberToObject(item, "max_latency_ms", tasks[i].max_latency_ms);
      cJSON_AddItemToArray(items, item);
   }
   char *dataset_wire = cJSON_PrintUnformatted(dataset);
   cJSON_Delete(dataset);
   if (!dataset_wire ||
       aimee_sha256_hex(dataset_wire, strlen(dataset_wire), out->dataset_hash) != 0)
   {
      free(dataset_wire);
      memset(out, 0, sizeof(*out));
      return -1;
   }
   free(dataset_wire);

   cJSON *target = cJSON_CreateObject();
   if (!target)
      return -1;
   cJSON_AddStringToObject(target, "aimee_version", AIMEE_VERSION);
   cJSON_AddStringToObject(target, "agent", agent_name);
   char *target_wire = cJSON_PrintUnformatted(target);
   cJSON_Delete(target);
   if (!target_wire || aimee_sha256_hex(target_wire, strlen(target_wire), out->target_hash) != 0)
   {
      free(target_wire);
      memset(out, 0, sizeof(*out));
      return -1;
   }
   free(target_wire);
   snprintf(out->harness_version, sizeof(out->harness_version), "2");
   eval_hardware_profile(out->hardware_profile);
   out->seed = (int)seed;
   return 0;
}

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
   if (!result->success || aimee_ir_response_complete_text_valid(&result->canonical_response))
      return 0;
   char *authored = aimee_ir_response_text_alloc(&result->canonical_response);
   if (!authored)
      return 0;
   int success = 1;
   if (!strcmp(task->success_check_type, "contains"))
      success = strstr(authored, task->success_check_value) != NULL;
   free(authored);
   return success;
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
                              const eval_task_t *all_tasks, int task_count,
                              const agent_result_t *result, int passed, const char *ablation,
                              unsigned int seed)
{
   int tool_failures = eval_tool_failures(result, passed);
   agent_eval_manifest_t manifest;
   int have_manifest = agent_eval_manifest_build(suite, all_tasks, task_count, result->agent_name,
                                                 seed, &manifest) == 0;
   char *text = aimee_ir_response_text_alloc(&result->canonical_response);
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
       .response = text,
       .error = result->error[0] ? result->error : NULL,
       .dataset_hash = have_manifest ? manifest.dataset_hash : "",
       .target_hash = have_manifest ? manifest.target_hash : "",
       .harness_version = have_manifest ? manifest.harness_version : "",
       .hardware_profile = have_manifest ? manifest.hardware_profile : "",
       .seed = (int)seed,
   };
   (void)db1_eval_result_insert(&row);
   free(text);
}

/* The Go owner retrieves and serializes the artifact; the host owns the file. */
static void eval_log_hard_negative(const char *suite_name, const eval_task_t *task,
                                   const agent_result_t *result)
{
   if (!config_present() || !config_memory_hard_negative_log()[0])
      return;
   FILE *fp = fopen(config_memory_hard_negative_log(), "a");
   if (!fp)
      return;
   if (mem_eval_write_hard_negative(fp, suite_name, task, result) != 0)
      fprintf(stderr, "Unable to write memory hard-negative artifact\n");
   if (fclose(fp) != 0)
      fprintf(stderr, "Unable to close memory hard-negative log\n");
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
            agent_result_t ar = {0};
            aimee_request_t request = {0};
            int request_rc = aimee_ir_request_from_text(&request, NULL, tasks[i].prompt, 0, 0.3);
            int rc = request_rc ? AGENT_RC_CONTEXT_REFUSED
                                : agent_run(cfg, tasks[i].role, &request, &ar);
            aimee_request_free(&request);

            int passed = (rc == 0) && eval_check_success(&tasks[i], &ar);
            if (passed && tasks[i].max_latency_ms > 0 && ar.latency_ms > tasks[i].max_latency_ms)
               passed = 0;
            int tool_failures = eval_tool_failures(&ar, passed);

            eval_store_result(suite_name, &tasks[i], tasks, task_count, &ar, passed, preset,
                              seed + (unsigned int)run);

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
            agent_result_clear_response(&ar);
         }
      }
   }

   memset(&cfg->ablation, 0, sizeof(cfg->ablation));
   return passes;
}

/* --- IR Metrics --- */

#include <math.h>

/* --- Memory Retrieval Eval --- */

double elapsed_ms(const struct timespec *start, const struct timespec *end);

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

/* Corpus/QA benchmark helpers live in agent_eval_memory_support.c. */
