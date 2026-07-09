/* server_eval.c: split from server.c into a real translation unit
 * (was server_eval.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_internal.h"
#include "aimee.h"
#include "harness_memory_audit.h"  /* hmem_audit */
#include "harness_memory_common.h" /* hmem_resolve_project / hmem_project_key_ok */
#include "harness_memory_scope.h"  /* hmem_scope_for_client */
#include "json_fluent.h"           /* jo_ok */
#include "memory_redirect.h"       /* memory_redirect_classify / _bash_targets / _rematerialize */
#include "server.h"
#include "turn_registry.h"
#include "server_http.h" /* server_http_api_status_report */
#include "config.h"      /* config_t / config_load for api.status, api.enable */
#include "delegate_backend_docker.h"
#include "delegate_backend_local.h"
#include "delegate_backend_ssh.h"
#include "server_delegate_monitor.h"
#include "server_coord_dispatcher.h"
#include "server_skill.h"
#include "server_compute_impl.h"
#include "skill_review.h"
#include "trigger_scheduler.h"
#include "wfe_live_delegate.h"
#include "wfe_scheduler.h"
#include "server_trigger.h"
#include "server_cron.h"
#include "server_pipeline.h" /* roundtable authoring pipeline (pipeline.*) */
#include "commands.h"
#include "agent.h"
#include "agent_exec.h"     /* agent_audit_async_flush — drain audit queue at shutdown */
#include "webuser_editor.h" /* webuser_editor_shutdown — reap editors at shutdown (WP-I) */
#include "agent_config.h"
#include "provider_catalog.h"
#include "delegate_credentials.h"
#include "model_registry.h"
#include "model_provider.h"
#include "model_registry.h"
#include "db1.h"
#include "token_audit.h"
#include "dashboard.h"
#include "log.h"
#include "hud.h"
#include "platform_event.h"
#include "platform_ipc.h"
#include "platform_path.h"
#include "platform_process.h"
#include "util.h"
#include "workspace.h"
#include "worktree_gc.h"
#include "git_verify.h"
#include "toolset.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#include "agent_eval.h"

static const char *server_eval_json_str(cJSON *obj, const char *key)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsString(v) ? v->valuestring : "";
}

static int server_eval_json_int(cJSON *obj, const char *key, int def)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsNumber(v) ? v->valueint : def;
}

static void server_eval_resolve_suite_path(const char *cwd, const char *suite, char *out,
                                           size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!suite || !suite[0])
      return;
   if (suite[0] == '/' || !cwd || !cwd[0])
      snprintf(out, out_len, "%s", suite);
   else
      snprintf(out, out_len, "%s/%s", cwd, suite);
}

static cJSON *server_eval_result_json(const eval_result_t *r)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddStringToObject(obj, "task_name", r->task_name);
   cJSON_AddStringToObject(obj, "agent_name", r->agent_name);
   cJSON_AddStringToObject(obj, "ablation", r->ablation);
   cJSON_AddBoolToObject(obj, "success", r->success);
   cJSON_AddNumberToObject(obj, "turns", r->turns);
   cJSON_AddNumberToObject(obj, "tool_calls", r->tool_calls);
   cJSON_AddNumberToObject(obj, "tool_call_failures", r->tool_call_failures);
   cJSON_AddNumberToObject(obj, "rescue_recoveries", r->rescue_recoveries);
   cJSON_AddNumberToObject(obj, "tool_call_success_rate", r->tool_call_success_rate);
   cJSON_AddNumberToObject(obj, "prompt_tokens", r->prompt_tokens);
   cJSON_AddNumberToObject(obj, "completion_tokens", r->completion_tokens);
   cJSON_AddNumberToObject(obj, "latency_ms", r->latency_ms);
   if (r->error[0])
      cJSON_AddStringToObject(obj, "error", r->error);
   return obj;
}

static cJSON *server_eval_display_row_json(const db1_eval_display_row_t *r)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddStringToObject(obj, "suite", r->suite);
   cJSON_AddStringToObject(obj, "task_name", r->task_name);
   cJSON_AddStringToObject(obj, "agent_name", r->agent_name);
   cJSON_AddStringToObject(obj, "ablation", r->ablation);
   cJSON_AddBoolToObject(obj, "success", r->success);
   cJSON_AddNumberToObject(obj, "turns", r->turns);
   cJSON_AddNumberToObject(obj, "tool_calls", r->tool_calls);
   cJSON_AddNumberToObject(obj, "tool_call_failures", r->tool_call_failures);
   cJSON_AddNumberToObject(obj, "rescue_recoveries", r->rescue_recoveries);
   cJSON_AddNumberToObject(obj, "latency_ms", r->latency_ms);
   cJSON_AddStringToObject(obj, "created_at", r->created_at);
   return obj;
}

int handle_eval_run(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *request_id = server_eval_json_str(req, "request_id");
   const char *suite = server_eval_json_str(req, "suite_dir");
   if (!suite[0])
      return server_send_error(conn, "eval.run requires suite_dir", request_id);

   agent_eval_run_options_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.ablation = server_eval_json_str(req, "ablation");
   if (!opts.ablation[0])
      opts.ablation = "full";
   opts.runs = server_eval_json_int(req, "runs", 1);
   if (opts.runs <= 0)
      opts.runs = 1;
   opts.seed = (unsigned int)server_eval_json_int(req, "seed", 42);
   if (strcmp(opts.ablation, "all") != 0)
   {
      agent_ablation_flags_t check_flags;
      if (agent_eval_ablation_preset(opts.ablation, &check_flags) != 0)
         return server_send_error(conn, "eval.run unknown ablation preset", request_id);
   }

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0 || acfg.agent_count == 0)
      return server_send_error(conn, "eval.run: no agents configured", request_id);

   char suite_path[MAX_PATH_LEN];
   server_eval_resolve_suite_path(server_eval_json_str(req, "cwd"), suite, suite_path,
                                  sizeof(suite_path));

   eval_result_t results[AGENT_MAX_EVAL_TASKS];
   int passes =
       agent_eval_run_with_options(&acfg, suite_path, &opts, results, AGENT_MAX_EVAL_TASKS);
   eval_task_t tasks[AGENT_MAX_EVAL_TASKS];
   int task_count = agent_eval_load_tasks(suite_path, tasks, AGENT_MAX_EVAL_TASKS);
   int preset_count = strcmp(opts.ablation, "all") == 0 ? 7 : 1;
   int total = task_count * preset_count * opts.runs;
   int rows = total > AGENT_MAX_EVAL_TASKS ? AGENT_MAX_EVAL_TASKS : total;

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "passes", passes);
   cJSON_AddNumberToObject(resp, "total", total);
   cJSON_AddStringToObject(resp, "suite_dir", suite_path);
   cJSON *arr = cJSON_AddArrayToObject(resp, "results");
   for (int i = 0; i < rows; i++)
      cJSON_AddItemToArray(arr, server_eval_result_json(&results[i]));
   if (request_id[0])
      cJSON_AddStringToObject(resp, "request_id", request_id);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_eval_results(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *request_id = server_eval_json_str(req, "request_id");
   const char *suite = server_eval_json_str(req, "suite");
   db1_eval_display_row_t rows[50];
   int n = db1_eval_results_list(suite[0] ? suite : NULL, rows, 50);
   if (n < 0)
      return server_send_error(conn, "eval.results: could not read eval results", request_id);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *arr = cJSON_AddArrayToObject(resp, "results");
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, server_eval_display_row_json(&rows[i]));
   if (request_id[0])
      cJSON_AddStringToObject(resp, "request_id", request_id);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}
