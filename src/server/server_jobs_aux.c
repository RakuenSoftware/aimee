/* server/server_jobs_aux.c: RPC handlers for durable delegate jobs and aux routing. */
#include "aimee.h"
#include "aux_router.h"
#include "db1.h"
#include "delegate_role.h"
#include "server.h"
#include "json_fluent.h" /* jo_ok */
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static cJSON *agent_job_to_json(const db1_agent_job_t *job, int include_prompt, int include_result)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj || !job)
      return obj;

   cJSON_AddNumberToObject(obj, "id", job->id);
   cJSON_AddNumberToObject(obj, "job_id", job->id);
   cJSON_AddStringToObject(obj, "role", job->role);
   cJSON_AddStringToObject(obj, "status", job->status);
   cJSON_AddStringToObject(obj, "agent_name", job->agent_name);
   cJSON_AddNumberToObject(obj, "cursor_turn", job->cursor_turn);
   if (include_prompt && job->prompt[0])
      cJSON_AddStringToObject(obj, "prompt", job->prompt);
   if (include_result && job->result[0])
      cJSON_AddStringToObject(obj, "result", job->result);
   if (job->lease_owner[0])
      cJSON_AddStringToObject(obj, "lease_owner", job->lease_owner);
   if (job->heartbeat_at[0])
      cJSON_AddStringToObject(obj, "heartbeat_at", job->heartbeat_at);
   if (job->current_tool[0])
      cJSON_AddStringToObject(obj, "current_tool", job->current_tool);
   cJSON_AddNumberToObject(obj, "api_call_count", job->api_call_count);
   int default_max_turns = delegate_default_max_turns_for_role(job->role);
   if (default_max_turns > 0)
      cJSON_AddNumberToObject(obj, "default_max_turns", default_max_turns);
   int final_after_turns = delegate_final_after_turns_for_role(job->role);
   if (final_after_turns > 0)
      cJSON_AddNumberToObject(obj, "final_after_turns", final_after_turns);
   if (job->created_at[0])
      cJSON_AddStringToObject(obj, "created_at", job->created_at);
   if (job->updated_at[0])
      cJSON_AddStringToObject(obj, "updated_at", job->updated_at);
   return obj;
}

static cJSON *coord_job_to_json(const db1_coord_job_t *job)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj || !job)
      return obj;

   cJSON_AddNumberToObject(obj, "id", job->id);
   cJSON_AddNumberToObject(obj, "job_id", job->id);
   cJSON_AddNumberToObject(obj, "plan_id", job->plan_id);
   cJSON_AddStringToObject(obj, "status", job->status);
   cJSON_AddNumberToObject(obj, "max_concurrent", job->max_concurrent);
   cJSON_AddNumberToObject(obj, "total", job->total_tasks);
   cJSON_AddNumberToObject(obj, "done", job->done_tasks);
   cJSON_AddNumberToObject(obj, "failed", job->failed_tasks);
   cJSON_AddNumberToObject(obj, "running", job->running_tasks);
   if (job->created_at[0])
      cJSON_AddStringToObject(obj, "created_at", job->created_at);
   if (job->updated_at[0])
      cJSON_AddStringToObject(obj, "updated_at", job->updated_at);
   return obj;
}

static cJSON *coord_task_to_json(const db1_coord_task_t *task)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj || !task)
      return obj;

   cJSON_AddNumberToObject(obj, "id", task->id);
   cJSON_AddNumberToObject(obj, "job_id", task->job_id);
   cJSON_AddNumberToObject(obj, "step_id", task->step_id);
   cJSON_AddStringToObject(obj, "status", task->status);
   if (task->claimed_by[0])
      cJSON_AddStringToObject(obj, "claimed_by", task->claimed_by);
   if (task->claimed_at[0])
      cJSON_AddStringToObject(obj, "claimed_at", task->claimed_at);
   if (task->files[0])
      cJSON_AddStringToObject(obj, "files", task->files);
   if (task->result[0])
      cJSON_AddStringToObject(obj, "result", task->result);
   if (task->error[0])
      cJSON_AddStringToObject(obj, "error", task->error);
   if (task->created_at[0])
      cJSON_AddStringToObject(obj, "created_at", task->created_at);
   return obj;
}

int handle_jobs_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int limit = 20;
   cJSON *jlimit = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (cJSON_IsNumber(jlimit))
      limit = jlimit->valueint;
   if (limit <= 0)
      limit = 20;
   if (limit > 100)
      limit = 100;

   db1_agent_job_t jobs[100];
   /* include_heavy=0: the list serializes with agent_job_to_json(...,0,0) and
    * never emits prompt/result, so do not load (and then free) up to 100 ×
    * the result ceiling of bodies. */
   int n = db1_agent_job_list_recent(jobs, limit, 0);
   if (n < 0)
      return server_send_error(conn, "jobs list: could not read agent jobs", NULL);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "job_count", n);
   cJSON *arr = cJSON_AddArrayToObject(resp, "jobs");
   for (int i = 0; i < n; i++)
   {
      if (arr)
         cJSON_AddItemToArray(arr, agent_job_to_json(&jobs[i], 0, 0));
      db1_agent_job_free(&jobs[i]);
   }

   return server_send_ok(conn, resp);
}

int handle_jobs_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "job_id");
   if (!cJSON_IsNumber(jid) || jid->valueint <= 0)
      return server_send_error(conn, "missing or invalid job_id", NULL);

   int job_id = jid->valueint;
   db1_agent_job_t job;
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "job_id", job_id);

   if (db1_agent_job_get(job_id, &job) != 0)
   {
      cJSON_AddStringToObject(resp, "job_status", "not_found");
   }
   else
   {
      cJSON_AddStringToObject(resp, "job_status", job.status);
      cJSON_AddItemToObject(resp, "job", agent_job_to_json(&job, 1, 1));
      db1_agent_job_free(&job);
   }

   return server_send_ok(conn, resp);
}

int handle_jobs_logs(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "job_id");
   if (!cJSON_IsNumber(jid) || jid->valueint <= 0)
      return server_send_error(conn, "missing or invalid job_id", NULL);

   int job_id = jid->valueint;
   db1_agent_job_t job;
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "job_id", job_id);

   if (db1_agent_job_get(job_id, &job) != 0)
   {
      cJSON_AddStringToObject(resp, "job_status", "not_found");
   }
   else
   {
      cJSON_AddStringToObject(resp, "job_status", job.status);
      cJSON_AddItemToObject(resp, "job", agent_job_to_json(&job, 1, 1));
      cJSON_AddStringToObject(resp, "log", job.result);
      db1_agent_job_free(&job);
   }

   return server_send_ok(conn, resp);
}

int handle_jobs_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "job_id");
   if (!cJSON_IsNumber(jid) || jid->valueint <= 0)
      return server_send_error(conn, "missing or invalid job_id", NULL);

   cJSON *jreason = cJSON_GetObjectItemCaseSensitive(req, "reason");
   const char *reason = "operator cancel";
   if (cJSON_IsString(jreason) && jreason->valuestring[0])
      reason = jreason->valuestring;
   int job_id = jid->valueint;
   int changed = db1_agent_job_cancel_by_id(job_id, reason);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "job_id", job_id);
   cJSON_AddBoolToObject(resp, "cancelled", changed > 0);
   cJSON_AddNumberToObject(resp, "changed", changed);
   if (changed == 0)
      cJSON_AddStringToObject(resp, "message", "No pending or running job found.");

   return server_send_ok(conn, resp);
}

int handle_coord_job_start(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jplan = cJSON_GetObjectItemCaseSensitive(req, "plan_id");
   if (!cJSON_IsNumber(jplan) || jplan->valueint <= 0)
      return server_send_error(conn, "missing or invalid plan_id", NULL);

   int max_concurrent = DB1_COORD_DEFAULT_PAR;
   cJSON *jparallel = cJSON_GetObjectItemCaseSensitive(req, "parallel");
   if (cJSON_IsNumber(jparallel) && jparallel->valueint > 0)
      max_concurrent = jparallel->valueint;

   int plan_id = jplan->valueint;
   plan_t plan;
   if (db1_execution_plan_get(plan_id, &plan) != 0)
      return server_send_error(conn, "coord job start: execution plan not found", NULL);

   int job_id = db1_coord_job_create(plan_id, max_concurrent);
   if (job_id < 0)
      return server_send_error(conn, "coord job start: could not create job", NULL);

   int added = 0;
   for (int i = 0; i < plan.step_count; i++)
   {
      int tid = db1_coord_job_add_task(job_id, plan.steps[i].id, "[]", "", "", "", "engineer");
      if (tid > 0)
         added++;
   }

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "job_id", job_id);
   cJSON_AddNumberToObject(resp, "plan_id", plan_id);
   cJSON_AddNumberToObject(resp, "tasks", added);
   cJSON_AddNumberToObject(resp, "max_concurrent", max_concurrent);

   return server_send_ok(conn, resp);
}

int handle_coord_job_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int limit = 20;
   cJSON *jlimit = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (cJSON_IsNumber(jlimit))
      limit = jlimit->valueint;
   if (limit <= 0)
      limit = 20;
   if (limit > 100)
      limit = 100;

   db1_coord_job_t jobs[100];
   int n = db1_coord_job_list_recent(jobs, limit);
   if (n < 0)
      return server_send_error(conn, "coord job list: could not read jobs", NULL);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "job_count", n);
   cJSON *arr = cJSON_AddArrayToObject(resp, "jobs");
   for (int i = 0; arr && i < n; i++)
      cJSON_AddItemToArray(arr, coord_job_to_json(&jobs[i]));

   return server_send_ok(conn, resp);
}

int handle_coord_job_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "job_id");
   if (!cJSON_IsNumber(jid) || jid->valueint <= 0)
      return server_send_error(conn, "missing or invalid job_id", NULL);

   int job_id = jid->valueint;
   db1_coord_job_t job;
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "job_id", job_id);

   if (db1_coord_job_get(job_id, &job) != 0)
   {
      cJSON_AddStringToObject(resp, "job_status", "not_found");
   }
   else
   {
      db1_coord_task_t tasks[DB1_COORD_MAX_TASKS];
      int count = db1_coord_job_list_tasks(job_id, tasks, DB1_COORD_MAX_TASKS);
      cJSON_AddStringToObject(resp, "job_status", job.status);
      cJSON_AddItemToObject(resp, "job", coord_job_to_json(&job));
      cJSON *arr = cJSON_AddArrayToObject(resp, "tasks");
      for (int i = 0; arr && i < count; i++)
         cJSON_AddItemToArray(arr, coord_task_to_json(&tasks[i]));
   }

   return server_send_ok(conn, resp);
}

int handle_coord_job_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "job_id");
   if (!cJSON_IsNumber(jid) || jid->valueint <= 0)
      return server_send_error(conn, "missing or invalid job_id", NULL);

   int job_id = jid->valueint;
   int rc_cancel = db1_coord_job_cancel(job_id);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "job_id", job_id);
   cJSON_AddBoolToObject(resp, "cancelled", rc_cancel == 0);
   if (rc_cancel != 0)
      cJSON_AddStringToObject(resp, "message", "No coordinated job found.");

   return server_send_ok(conn, resp);
}

static cJSON *aux_config_to_json(const config_t *cfg)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj || !cfg)
      return obj;

   cJSON_AddBoolToObject(obj, "enabled", cfg->aux_enabled);
   cJSON_AddStringToObject(obj, "default_provider",
                           cfg->aux_default_provider[0] ? cfg->aux_default_provider : "");
   cJSON_AddStringToObject(obj, "default_model",
                           cfg->aux_default_model[0] ? cfg->aux_default_model : "");
   cJSON_AddNumberToObject(obj, "default_max_tokens", cfg->aux_default_max_tokens);

   cJSON *tasks = cJSON_AddArrayToObject(obj, "tasks");
   for (int i = 0; tasks && i < cfg->aux_task_count; i++)
   {
      cJSON *task = cJSON_CreateObject();
      if (!task)
         continue;
      cJSON_AddStringToObject(task, "task", cfg->aux_tasks[i].task);
      cJSON_AddStringToObject(task, "provider", cfg->aux_tasks[i].provider);
      cJSON_AddStringToObject(task, "model", cfg->aux_tasks[i].model);
      cJSON_AddNumberToObject(task, "max_tokens", cfg->aux_tasks[i].max_tokens);
      cJSON_AddItemToArray(tasks, task);
   }
   return obj;
}

int handle_aux_config_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   config_t cfg;
   if (config_load(&cfg) != 0)
      return server_send_error(conn, "aux config: could not load configuration", NULL);

   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "auxiliary", aux_config_to_json(&cfg));

   return server_send_ok(conn, resp);
}

int handle_aux_test(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jtask = cJSON_GetObjectItemCaseSensitive(req, "task");
   cJSON *jprompt = cJSON_GetObjectItemCaseSensitive(req, "prompt");
   if (!cJSON_IsString(jtask) || !jtask->valuestring[0] || !cJSON_IsString(jprompt) ||
       !jprompt->valuestring[0])
      return server_send_error(conn, "usage: aimee aux test <task> \"<prompt>\" [max_tokens]",
                               NULL);

   int max_tokens = 0;
   cJSON *jmax = cJSON_GetObjectItemCaseSensitive(req, "max_tokens");
   if (cJSON_IsNumber(jmax))
      max_tokens = jmax->valueint;

   config_t cfg;
   if (config_load(&cfg) != 0)
      return server_send_error(conn, "aux test: could not load configuration", NULL);
   if (!cfg.aux_enabled)
      return server_send_error(conn,
                               "aux routing is disabled (set auxiliary.enabled: true in "
                               "aimee.yaml)",
                               NULL);

   char *result = aux_call(&cfg, jtask->valuestring, jprompt->valuestring, max_tokens);
   if (!result)
      return server_send_error(conn, "aux_call failed", NULL);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "task", jtask->valuestring);
   cJSON_AddStringToObject(resp, "response", result);
   free(result);

   return server_send_ok(conn, resp);
}
