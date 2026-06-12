/* server_delegate_status.c: delegate job status RPC */
#include <stdio.h>
#include <string.h>

#include "server.h"
#include "db1.h"
#include "delegate_role.h"
#include "liveness.h"
#include "cJSON.h"

static void delegate_status_quarantine_degenerate_done(int job_id, db1_agent_job_t *job)
{
   if (!job || strcmp(job->status, "done") != 0 || !job->result[0] ||
       !liveness_is_degenerate_response(job->result))
      return;

   const char *message = "delegate returned raw tool-call markup or another degenerate response";
   db1_agent_job_update(job_id, "failed", job->cursor_turn, message);
   snprintf(job->status, sizeof(job->status), "%s", "failed");
   /* result is now heap-owned: replace it (free old, strdup new) — NOT
    * snprintf into a char* (whose sizeof is 8). */
   char *dup = strdup(message);
   if (dup)
   {
      free(job->result);
      job->result = dup;
   }
}

static void delegate_status_populate_job(cJSON *resp, int job_id)
{
   db1_agent_job_t job;
   memset(&job, 0, sizeof(job)); /* so the single-exit free is safe on every branch */
   cJSON_AddNumberToObject(resp, "job_id", job_id);

   if (job_id <= 0)
   {
      cJSON_AddStringToObject(resp, "job_status", "invalid");
   }
   else if (db1_agent_job_get(job_id, &job) != 0)
   {
      cJSON_AddStringToObject(resp, "job_status", "not_found");
   }
   else
   {
      delegate_status_quarantine_degenerate_done(job_id, &job);
      cJSON_AddStringToObject(resp, "job_status", job.status);
      cJSON_AddStringToObject(resp, "role", job.role);
      cJSON_AddStringToObject(resp, "agent_name", job.agent_name);
      cJSON_AddNumberToObject(resp, "cursor_turn", job.cursor_turn);
      if (job.result[0])
         cJSON_AddStringToObject(resp, "result", job.result);
      if (job.heartbeat_at[0])
         cJSON_AddStringToObject(resp, "heartbeat_at", job.heartbeat_at);
      if (job.current_tool[0])
         cJSON_AddStringToObject(resp, "current_tool", job.current_tool);
      cJSON_AddNumberToObject(resp, "api_call_count", job.api_call_count);
      int default_max_turns = delegate_default_max_turns_for_role(job.role);
      if (default_max_turns > 0)
         cJSON_AddNumberToObject(resp, "default_max_turns", default_max_turns);
      int final_after_turns = delegate_final_after_turns_for_role(job.role);
      if (final_after_turns > 0)
         cJSON_AddNumberToObject(resp, "final_after_turns", final_after_turns);
      if (job.created_at[0])
         cJSON_AddStringToObject(resp, "created_at", job.created_at);
      if (job.updated_at[0])
         cJSON_AddStringToObject(resp, "updated_at", job.updated_at);
   }
   db1_agent_job_free(&job);
}

int handle_delegate_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "job_id");
   cJSON *jids = cJSON_GetObjectItemCaseSensitive(req, "job_ids");
   if ((!cJSON_IsNumber(jid) || jid->valueint <= 0) &&
       (!cJSON_IsArray(jids) || cJSON_GetArraySize(jids) <= 0))
      return server_send_error(conn, "missing or invalid job_id", NULL);

   cJSON *jfull = cJSON_GetObjectItemCaseSensitive(req, "full_result");
   cJSON *jlimit = cJSON_GetObjectItemCaseSensitive(req, "result_limit");
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "failed to allocate delegate status response", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
   if (cJSON_IsTrue(jfull))
      cJSON_AddBoolToObject(resp, "full_result", 1);
   if (cJSON_IsNumber(jlimit))
      cJSON_AddNumberToObject(resp, "result_limit", jlimit->valueint);

   if (cJSON_IsArray(jids))
   {
      cJSON *jobs = cJSON_AddArrayToObject(resp, "jobs");
      if (!jobs)
      {
         cJSON_Delete(resp);
         return server_send_error(conn, "failed to allocate delegate status response", NULL);
      }
      cJSON *item = NULL;
      cJSON_ArrayForEach(item, jids)
      {
         if (!cJSON_IsNumber(item))
            continue;
         cJSON *job_obj = cJSON_CreateObject();
         if (!job_obj)
         {
            cJSON_Delete(resp);
            return server_send_error(conn, "failed to allocate delegate status response", NULL);
         }
         delegate_status_populate_job(job_obj, item->valueint);
         cJSON_AddItemToArray(jobs, job_obj);
      }
   }
   else
   {
      delegate_status_populate_job(resp, jid->valueint);
   }

   return server_send_ok(conn, resp);
}
