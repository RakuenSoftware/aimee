/* cmd_run.c: aimee run — non-interactive headless agent execution with structured output.
 *
 * Designed for scripts, CI, and automation where interactive confirmation prompts
 * would be disruptive. Supports text, JSON, and NDJSON output modes.
 *
 * Usage:
 *   aimee run --prompt "Review src/auth.c" [--role execute] [--output text|json|ndjson]
 *             [--max-turns N] [--max-cost N.NN] [--timeout N] [--system "..."]
 *             [--max-tokens N]
 */
#include "aimee.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "commands.h"
#include "prompts.h"
#include "role_templates.h"
#include "render.h"
#include "token_tracker.h"
#include "cJSON.h"
#include <time.h>
#include <unistd.h>

#include "cmd_run.h"

/* Exposed for unit tests */
run_output_mode_t run_parse_output_mode(const char *s)
{
   if (!s)
      return RUN_OUTPUT_TEXT;
   if (strcmp(s, "json") == 0)
      return RUN_OUTPUT_JSON;
   if (strcmp(s, "ndjson") == 0)
      return RUN_OUTPUT_NDJSON;
   return RUN_OUTPUT_TEXT;
}

static void print_help(void)
{
   fprintf(stderr, "Usage: aimee run --prompt \"task\" [options]\n"
                   "\n"
                   "Run an agent non-interactively. Suitable for scripts, CI, and automation.\n"
                   "\n"
                   "Options:\n"
                   "  --prompt TEXT      Task prompt (required)\n"
                   "  --role ROLE        Agent role to use (default: execute)\n"
                   "  --output MODE      Output format: text (default), json, ndjson\n"
                   "  --max-turns N      Maximum LLM turns (default: from config)\n"
                   "  --max-cost N.NN    Maximum cost in USD; exits with code 2 if exceeded\n"
                   "  --timeout N        Timeout in seconds (applied to HTTP calls)\n"
                   "  --system TEXT      System prompt override\n"
                   "  --max-tokens N     Maximum completion tokens\n"
                   "\n"
                   "Output modes:\n"
                   "  text    Final response text on stdout (default)\n"
                   "  json    Structured JSON result on stdout\n"
                   "  ndjson  Newline-delimited JSON events: start, then done\n"
                   "\n"
                   "Exit codes:\n"
                   "  0   Success\n"
                   "  1   Agent error\n"
                   "  2   Limit hit (max-cost or max-turns exceeded)\n"
                   "\n"
                   "Examples:\n"
                   "  aimee run --prompt \"Review src/auth.c for security issues\" --output json\n"
                   "  aimee run --prompt \"Add error handling\" --max-turns 10 --max-cost 0.50\n"
                   "  aimee run --prompt \"Summarize the changes\" --output ndjson | jq .\n");
}

/* Emit a single NDJSON event. obj ownership is consumed (freed). */
static void ndjson_emit(cJSON *obj)
{
   char *s = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   if (s)
   {
      printf("%s\n", s);
      fflush(stdout);
      free(s);
   }
}

void cmd_run(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc == 0 || strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0)
   {
      print_help();
      return;
   }

   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *prompt = opt_get(&opts, "prompt");
   const char *role = opt_get(&opts, "role");
   const char *output_str = opt_get(&opts, "output");
   int max_turns = opt_get_int(&opts, "max-turns", 0);
   double max_cost = 0.0;
   const char *max_cost_str = opt_get(&opts, "max-cost");
   int timeout_secs = opt_get_int(&opts, "timeout", 0);
   const char *sys_prompt_override = opt_get(&opts, "system");
   int max_tokens = opt_get_int(&opts, "max-tokens", 0);

   if (max_cost_str)
      max_cost = atof(max_cost_str);

   if (!prompt || !prompt[0])
   {
      fprintf(stderr, "aimee: run: --prompt is required\n\n");
      print_help();
      exit(RUN_EXIT_ERROR);
   }

   if (!role || !role[0])
      role = "execute";

   run_output_mode_t output_mode = run_parse_output_mode(output_str);

   /* Load agent config */
   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
   {
      fprintf(stderr, "aimee: run: failed to load agent config\n");
      exit(RUN_EXIT_ERROR);
   }

   /* Apply per-run max-turns limit */
   if (max_turns > 0)
   {
      agent_t *target = agent_route(&acfg, role);
      if (target)
         target->max_turns = max_turns;
   }

   /* Apply per-run timeout (convert seconds to milliseconds) */
   if (timeout_secs > 0)
   {
      agent_t *target = agent_route(&acfg, role);
      if (target)
         target->timeout_ms = timeout_secs * 1000;
   }

   /* Build system prompt: role template → tier default → caller override */
   char *built_sys_prompt = NULL;
   const char *sys_prompt = sys_prompt_override;
   if (!sys_prompt)
   {
      char cwd_buf[MAX_PATH_LEN];
      if (!getcwd(cwd_buf, sizeof(cwd_buf)))
         cwd_buf[0] = '\0';
      built_sys_prompt = role_template_build(cwd_buf, role, prompt, NULL);
      if (built_sys_prompt)
         sys_prompt = built_sys_prompt;
   }
   if (!sys_prompt)
   {
      char cwd_buf[MAX_PATH_LEN];
      if (!getcwd(cwd_buf, sizeof(cwd_buf)))
         cwd_buf[0] = '\0';
      prompt_tier_t dtier = config_delegate_prompt_tier()[0]
                                ? prompt_tier_from_string(config_delegate_prompt_tier())
                                : PROMPT_MINIMAL;
      built_sys_prompt = prompt_build(dtier, cwd_buf, NULL);
      if (built_sys_prompt)
         sys_prompt = built_sys_prompt;
   }
   if (built_sys_prompt)
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      if (config_load(&cfg) == 0)
      {
         char *with_dispositions = prompt_apply_dispositions(built_sys_prompt);
         if (with_dispositions)
         {
            free(built_sys_prompt);
            built_sys_prompt = with_dispositions;
            sys_prompt = built_sys_prompt;
         }
      }
   }

   /* Emit NDJSON start event */
   if (output_mode == RUN_OUTPUT_NDJSON)
   {
      cJSON *start = cJSON_CreateObject();
      cJSON_AddStringToObject(start, "type", "start");
      cJSON_AddStringToObject(start, "role", role);
      cJSON_AddStringToObject(start, "prompt", prompt);
      ndjson_emit(start);
   }

   /* Run agent */
   agent_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = agent_run_with_tools(&acfg, role, sys_prompt, prompt, max_tokens, &result);

   free(built_sys_prompt);

   /* Check max-cost limit */
   int cost_exceeded = 0;
   double actual_cost = 0.0;
   if (max_cost > 0.0 && rc == 0)
   {
      agent_t *target = agent_route(&acfg, role);
      const char *model = (target && target->model[0]) ? target->model : "";
      token_usage_t usage = {
          .input_tokens = result.prompt_tokens,
          .output_tokens = result.completion_tokens,
          .cache_write_tokens = result.cache_write_tokens,
          .cache_read_tokens = result.cache_read_tokens,
      };
      actual_cost = token_estimate_cost(model, &usage);
      if (actual_cost > max_cost)
         cost_exceeded = 1;
   }

   /* Produce output */
   int exit_code = RUN_EXIT_OK;

   if (output_mode == RUN_OUTPUT_JSON)
   {
      cJSON *obj = agent_result_to_json(&result);
      if (cost_exceeded)
      {
         cJSON_AddStringToObject(obj, "limit_hit", "cost");
         cJSON_AddNumberToObject(obj, "actual_cost_usd", actual_cost);
         cJSON_AddNumberToObject(obj, "max_cost_usd", max_cost);
      }
      char *json = cJSON_Print(obj);
      cJSON_Delete(obj);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
   }
   else if (output_mode == RUN_OUTPUT_NDJSON)
   {
      cJSON *done = agent_result_to_json(&result);
      cJSON_AddStringToObject(done, "type", "done");
      if (cost_exceeded)
      {
         cJSON_AddStringToObject(done, "limit_hit", "cost");
         cJSON_AddNumberToObject(done, "actual_cost_usd", actual_cost);
         cJSON_AddNumberToObject(done, "max_cost_usd", max_cost);
      }
      ndjson_emit(done);
   }
   else
   {
      /* text mode */
      if (rc == 0 && result.response)
         printf("%s\n", result.response);
      else if (rc != 0)
         fprintf(stderr, "aimee: run: %s\n", result.error[0] ? result.error : "agent error");
   }

   if (rc != 0)
      exit_code = RUN_EXIT_ERROR;
   else if (cost_exceeded)
      exit_code = RUN_EXIT_LIMIT_HIT;

   free(result.response);

   if (exit_code != RUN_EXIT_OK)
      exit(exit_code);
}
