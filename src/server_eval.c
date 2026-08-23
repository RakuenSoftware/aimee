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
#include "config.h"      /* legacy_config_record / legacy_config_read for api.status, api.enable */
#include <aimee/delegates/delegate_backend_docker.h>
#include "server_delegate_monitor.h"
#include "server_coord_dispatcher.h"
#include "server_skill.h"
#include "server_compute_impl.h"
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
#include <aimee/delegates/delegate_credentials.h>
#include "model_registry.h"
#include "model_provider.h"
#include "model_registry.h"
#include "db1.h"
#include "eval_synthesis.h" /* synthesised regression candidates (S1) */
#include "approach_store.h"
#include <aimee/learning/attribution.h>
#include <aimee/learning/policy_arms.h>
#include "curiosity_resolve.h"
#include "token_audit.h"
#include "dashboard.h"
#include "log.h"
#include "hud.h"
#include "platform_event.h"
#include "platform_ipc.h"
#include "platform_path.h"
#include "platform_process.h"
#include "util.h"
#include <aimee/workspace/workspace.h>
#include "worktree_gc.h"
#include "modules/git/git_verify.h"
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

/* --- Synthesised regression candidates (recursive self-improvement S1) --- */

static cJSON *server_eval_candidate_json(const db1_eval_candidate_t *c)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "id", (double)c->id);
   cJSON_AddStringToObject(obj, "signature", c->signature);
   cJSON_AddStringToObject(obj, "state", c->state);
   cJSON_AddStringToObject(obj, "suite", c->suite);
   cJSON_AddStringToObject(obj, "task_name", c->task_name);
   cJSON_AddStringToObject(obj, "origin", c->origin);
   cJSON_AddStringToObject(obj, "origin_ref", c->origin_ref);
   cJSON_AddNumberToObject(obj, "occurrences", c->occurrences);
   cJSON_AddNumberToObject(obj, "distinct_sessions", c->distinct_sessions);
   cJSON_AddStringToObject(obj, "admitted_by", c->admitted_by);
   cJSON_AddStringToObject(obj, "admitted_path", c->admitted_path);
   cJSON_AddStringToObject(obj, "reject_reason", c->reject_reason);
   cJSON_AddNumberToObject(obj, "passing_windows", c->passing_windows);
   cJSON_AddStringToObject(obj, "created_at", c->created_at);
   cJSON_AddStringToObject(obj, "updated_at", c->updated_at);
   return obj;
}

/* --- Approach-level negative knowledge (recursive self-improvement S3) --- */

int handle_learning_approaches(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *request_id = server_eval_json_str(req, "request_id");
   const char *goal = server_eval_json_str(req, "goal");
   if (!goal[0])
      return server_send_error(conn, "learning.approaches requires goal", request_id);

   learning_approach_hit_t hits[APPROACH_MEM_MAX_RECALL];
   int n = approach_store_recall(goal, hits, APPROACH_MEM_MAX_RECALL);
   if (n < 0)
      return server_send_error(conn, "learning.approaches: could not recall", request_id);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "goal", goal);
   cJSON *arr = cJSON_AddArrayToObject(resp, "approaches");
   for (int i = 0; i < n; i++)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "approach", hits[i].approach_text);
      cJSON_AddStringToObject(o, "failure_mode", hits[i].failure_mode);
      cJSON_AddStringToObject(o, "goal", hits[i].goal_text);
      cJSON_AddStringToObject(o, "source_ref", hits[i].source_ref);
      cJSON_AddNumberToObject(o, "occurrences", (double)hits[i].occurrences);
      cJSON_AddNumberToObject(o, "similarity", hits[i].similarity);
      cJSON_AddItemToArray(arr, o);
   }
   /* The rendered advisory block, so a caller assembling a plan-time prompt
    * does not have to re-derive the wording (and cannot turn it into an
    * instruction by accident). */
   char rendered[2048];
   char arm[LEARNING_POLICY_ARM_LEN] = "";
   (void)approach_store_render(goal, rendered, sizeof(rendered), arm, sizeof(arm));
   cJSON_AddStringToObject(resp, "advisory", rendered);
   /* Which arm produced it: a caller measuring whether the block earns its
    * tokens needs to know which variant it is measuring. */
   cJSON_AddStringToObject(resp, "advisory_arm", arm);
   if (request_id[0])
      cJSON_AddStringToObject(resp, "request_id", request_id);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_learning_attribution(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *request_id = server_eval_json_str(req, "request_id");
   const char *suite = server_eval_json_str(req, "suite");

   learning_attribution_t arms[LEARNING_ATTRIBUTION_MAX_ARMS];
   int n = eval_attribution_for_suite(suite[0] ? suite : NULL, arms, LEARNING_ATTRIBUTION_MAX_ARMS);
   if (n < 0)
      return server_send_error(conn, "learning.attribution: could not read the grid", request_id);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "baseline", LEARNING_ATTRIBUTION_BASELINE);
   cJSON_AddNumberToObject(resp, "min_tasks", LEARNING_ATTRIBUTION_MIN_TASKS);
   cJSON *arr = cJSON_AddArrayToObject(resp, "arms");
   for (int i = 0; i < n; i++)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "ablation", arms[i].ablation);
      cJSON_AddNumberToObject(o, "tasks_compared", arms[i].tasks_compared);
      cJSON_AddNumberToObject(o, "baseline_passed", arms[i].baseline_passed);
      cJSON_AddNumberToObject(o, "arm_passed", arms[i].arm_passed);
      cJSON_AddNumberToObject(o, "delta", arms[i].delta);
      cJSON_AddBoolToObject(o, "attributable", arms[i].attributable);
      cJSON_AddItemToArray(arr, o);
   }
   if (request_id[0])
      cJSON_AddStringToObject(resp, "request_id", request_id);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_learning_resolve(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *request_id = server_eval_json_str(req, "request_id");
   curiosity_resolve_stats_t stats;
   int resolved = curiosity_resolve_pass(server_eval_json_int(req, "budget", 0), &stats);
   if (resolved < 0)
      return server_send_error(conn, "learning.resolve: could not read the backlog", request_id);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "resolved", stats.resolved);
   cJSON_AddNumberToObject(resp, "considered", stats.considered);
   cJSON_AddNumberToObject(resp, "still_open", stats.still_open);
   cJSON_AddNumberToObject(resp, "unknown", stats.unknown);
   cJSON_AddNumberToObject(resp, "skipped", stats.skipped);
   cJSON_AddNumberToObject(resp, "budget", stats.budget);
   /* Say so loudly rather than reporting a successful pass that closed
    * nothing because it had no way to decide. */
   cJSON_AddBoolToObject(resp, "no_probe", stats.no_probe);
   if (request_id[0])
      cJSON_AddStringToObject(resp, "request_id", request_id);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_eval_candidates(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *request_id = server_eval_json_str(req, "request_id");
   const char *state = server_eval_json_str(req, "state");
   int limit = server_eval_json_int(req, "limit", 50);
   if (limit <= 0 || limit > EVAL_CANDIDATES_MAX_ROWS)
      limit = EVAL_CANDIDATES_MAX_ROWS;

   db1_eval_candidate_t rows[EVAL_CANDIDATES_MAX_ROWS];
   int n = db1_eval_candidate_list(state, rows, limit);
   if (n < 0)
      return server_send_error(conn, "eval.candidates: could not read candidates", request_id);

   /* The endogeneity gate decides whether admission is even possible, so it
    * belongs in the same view as the backlog it governs. */
   learning_endogeneity_t endo;
   learning_gate_state_t gate = learning_gate_check(&endo);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *arr = cJSON_AddArrayToObject(resp, "candidates");
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, server_eval_candidate_json(&rows[i]));
   cJSON *g = cJSON_AddObjectToObject(resp, "gate");
   if (g)
   {
      cJSON_AddStringToObject(g, "state",
                              gate == LEARNING_GATE_OPEN                ? "open"
                              : gate == LEARNING_GATE_CLOSED_ENDOGENOUS ? "closed"
                                                                        : "unavailable");
      cJSON_AddNumberToObject(g, "exogenous_ratio", endo.exogenous_ratio);
      cJSON_AddNumberToObject(g, "committed_total", (double)endo.committed_total);
      cJSON_AddNumberToObject(g, "window_days", endo.window_days);
   }
   if (request_id[0])
      cJSON_AddStringToObject(resp, "request_id", request_id);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_eval_candidates_update(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *request_id = server_eval_json_str(req, "request_id");
   const char *op = server_eval_json_str(req, "op");
   if (!op[0])
      return server_send_error(
          conn, "eval.candidates-update requires op (scan|admit|reject|retire)", request_id);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "op", op);

   if (strcmp(op, "scan") == 0)
   {
      eval_synthesis_scan_stats_t stats;
      int observed = eval_synthesis_scan_failures(server_eval_json_int(req, "window_days", 0),
                                                  server_eval_json_str(req, "suite"), &stats);
      if (observed < 0)
      {
         cJSON_Delete(resp);
         return server_send_error(conn, "eval.candidates-update scan failed", request_id);
      }
      cJSON_AddNumberToObject(resp, "observed", observed);
      cJSON_AddNumberToObject(resp, "jobs_seen", stats.jobs_seen);
      cJSON_AddNumberToObject(resp, "signals_seen", stats.signals_seen);
      cJSON_AddNumberToObject(resp, "rejected_text", stats.rejected_text);
      cJSON_AddNumberToObject(resp, "skipped", stats.skipped);
   }
   else if (strcmp(op, "admit") == 0 || strcmp(op, "retire") == 0)
   {
      const char *dir = server_eval_json_str(req, "suite_dir");
      if (!dir[0])
      {
         cJSON_Delete(resp);
         return server_send_error(conn, "eval.candidates-update admit/retire requires suite_dir",
                                  request_id);
      }
      char suite_path[MAX_PATH_LEN];
      server_eval_resolve_suite_path(server_eval_json_str(req, "cwd"), dir, suite_path,
                                     sizeof(suite_path));
      cJSON_AddStringToObject(resp, "suite_dir", suite_path);

      int n =
          strcmp(op, "admit") == 0
              ? eval_synthesis_admit_pending(suite_path, server_eval_json_str(req, "by"),
                                             server_eval_json_int(req, "min_occurrences", 0))
              : eval_synthesis_retire(suite_path, server_eval_json_int(req, "retire_windows", 0));
      if (n < 0)
      {
         cJSON_Delete(resp);
         return server_send_error(conn, "eval.candidates-update failed", request_id);
      }
      cJSON_AddNumberToObject(resp, strcmp(op, "admit") == 0 ? "admitted" : "retired", n);
   }
   else if (strcmp(op, "reject") == 0)
   {
      int id = server_eval_json_int(req, "id", 0);
      if (id <= 0)
      {
         cJSON_Delete(resp);
         return server_send_error(conn, "eval.candidates-update reject requires id", request_id);
      }
      const char *reason = server_eval_json_str(req, "reason");
      if (db1_eval_candidate_mark_rejected((int64_t)id, reason) != 0)
      {
         cJSON_Delete(resp);
         return server_send_error(conn, "eval.candidates-update: no such candidate", request_id);
      }
      cJSON_AddNumberToObject(resp, "rejected", id);
   }
   else
   {
      cJSON_Delete(resp);
      return server_send_error(conn, "eval.candidates-update unknown op", request_id);
   }

   cJSON_AddStringToObject(resp, "status", "ok");
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
