/* server_delegate_status.c: delegate job status RPC */
#include <stdio.h>
#include <string.h>

#include "server.h"
#include "db1_client/db1.h"
#include <aimee/delegates/delegate_role.h>
#include "request_context.h"
#include <aimee/ir/session_event.h>
#include <stdlib.h>
#include "cJSON.h"
#include "server_http_internal.h" /* SHTTP_RESP_MAX */
#include "server_job_reply.h"

/* A job reply must fit the HTTP relay (SHTTP_RESP_MAX). A delegate's prompt and
 * result can each reach 1 MiB, so `aimee jobs show` on a remote CLI answered
 * "rpc response exceeds the 262144-byte relay limit" for exactly the failed jobs
 * an operator needs to read. Leave room for the envelope and JSON escaping. */
#define JOB_REPLY_BUDGET ((size_t)SHTTP_RESP_MAX - 16u * 1024u)

/* Back n up to the start of a UTF-8 sequence so a cut never splits a character. */
static size_t utf8_floor(const char *s, size_t n)
{
   while (n > 0 && ((unsigned char)s[n] & 0xC0u) == 0x80u)
      n--;
   return n;
}

/* Shorten obj[key] to about cap bytes, keeping its head and its tail (where a
 * failure reason usually is), and record the original size. */
static void job_text_bound(cJSON *obj, const char *key, size_t cap)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (!cJSON_IsString(item) || !item->valuestring)
      return;
   const char *s = item->valuestring;
   size_t len = strlen(s);
   if (len <= cap)
      return;
   size_t head = utf8_floor(s, cap / 4);
   size_t tail_start = len - (cap - cap / 4);
   while (tail_start < len && ((unsigned char)s[tail_start] & 0xC0u) == 0x80u)
      tail_start++;
   char marker[160];
   int mlen = snprintf(marker, sizeof marker,
                       "\n[... %zu bytes elided to fit the reply; %zu bytes in full ...]\n",
                       tail_start - head, len);
   size_t out_len = head + (size_t)mlen + (len - tail_start);
   char *out = malloc(out_len + 1);
   if (!out)
      return;
   memcpy(out, s, head);
   memcpy(out + head, marker, (size_t)mlen);
   memcpy(out + head + (size_t)mlen, s + tail_start, len - tail_start);
   out[out_len] = '\0';
   char flag[64];
   snprintf(flag, sizeof flag, "%s_bytes", key);
   if (!cJSON_GetObjectItemCaseSensitive(obj, flag))
      cJSON_AddNumberToObject(obj, flag, (double)len);
   cJSON_ReplaceItemInObjectCaseSensitive(obj, key, cJSON_CreateString(out));
   free(out);
}

static size_t job_reply_size(const cJSON *resp)
{
   char *wire = cJSON_PrintUnformatted(resp);
   size_t n = wire ? strlen(wire) : (size_t)-1;
   free(wire);
   return n;
}

/* Drop obj[key] (a transcript array or the last output) and record how many
 * entries it held. */
static void job_array_omit(cJSON *obj, const char *key)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (!item)
      return;
   int count = cJSON_IsArray(item) ? cJSON_GetArraySize(item) : 1;
   cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
   char flag[64];
   snprintf(flag, sizeof flag, "%s_omitted", key);
   cJSON_AddNumberToObject(obj, flag, count);
}

void server_job_reply_fit(cJSON *resp)
{
   static const char *texts[] = {"prompt", "result", "diagnostic", "legacy_result", "log"};
   /* The transcript last: events and history repeat each other, and outputs
    * still carry the answer once both are gone. */
   static const char *arrays[] = {"events", "history", "outputs", "output"};
   if (!resp || job_reply_size(resp) <= JOB_REPLY_BUDGET)
      return;
   cJSON *job = cJSON_GetObjectItemCaseSensitive(resp, "job");
   cJSON *targets[] = {resp, cJSON_IsObject(job) ? job : NULL};
   for (size_t cap = JOB_REPLY_BUDGET / 4; cap >= 1024 && job_reply_size(resp) > JOB_REPLY_BUDGET;
        cap /= 2)
      for (size_t t = 0; t < 2; t++)
         for (size_t i = 0; targets[t] && i < sizeof texts / sizeof *texts; i++)
            job_text_bound(targets[t], texts[i], cap);
   for (size_t i = 0; i < sizeof arrays / sizeof *arrays && job_reply_size(resp) > JOB_REPLY_BUDGET;
        i++)
      for (size_t t = 0; t < 2; t++)
         if (targets[t])
            job_array_omit(targets[t], arrays[i]);
}

/* Read a model observation only from the authenticated canonical log owner.
 * Polling never rewrites durable terminal state or treats a display as history. */
int server_delegate_job_observation(cJSON *resp, int job_id, int full)
{
   const request_context_t *context = request_context_get();
   char *wire = NULL;
   int rc = context && context->vault_principal[0]
                ? db1_agent_job_canonical_observation(job_id, context->vault_principal, &wire)
                : -1;
   cJSON *observation = rc == 0 && wire ? cJSON_ParseWithOpts(wire, NULL, 1) : NULL;
   free(wire);
   if (rc == -2)
   {
      cJSON_AddStringToObject(resp, "canonical_state", "forbidden");
      return -2;
   }
   if (!observation)
   {
      cJSON_AddStringToObject(resp, "canonical_state", rc == 1 ? "missing" : "unavailable");
      return rc == 1 ? 1 : -1;
   }
   const char *schema = cJSON_GetStringValue(cJSON_GetObjectItem(observation, "schema"));
   const cJSON *parent = cJSON_GetObjectItem(observation, "parent_source");
   const cJSON *child = cJSON_GetObjectItem(observation, "child");
   const cJSON *events = cJSON_GetObjectItem(observation, "events");
   const cJSON *history = cJSON_GetObjectItem(observation, "history");
   const char *owner = cJSON_GetStringValue(cJSON_GetObjectItem(parent, "actor"));
   const char *turn = cJSON_GetStringValue(cJSON_GetObjectItem(child, "turn_id"));
   const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(observation, "job_id"));
   char expected[32];
   snprintf(expected, sizeof expected, "%d", job_id);
   if (!schema || strcmp(schema, "aimee.agent-job-observation.v1") || !context || !owner ||
       strcmp(owner, context->vault_principal) || !id || strcmp(id, expected) || !turn ||
       !turn[0] || !cJSON_IsArray(events) || !cJSON_IsArray(history) ||
       !aimee_ir_json_unique(observation))
      goto unavailable;
   cJSON *source = cJSON_CreateObject(), *outputs = cJSON_CreateArray();
   if (!source || !outputs)
   {
      cJSON_Delete(source);
      cJSON_Delete(outputs);
      goto unavailable;
   }
   const char *fields[] = {"schema",        "job_id",           "parent_source",  "child",
                           "last_sequence", "execution_status", "terminal_source"};
   for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
   {
      const cJSON *value = cJSON_GetObjectItem(observation, fields[i]);
      if (!value || !cJSON_AddItemToObject(source, fields[i], cJSON_Duplicate(value, 1)))
      {
         cJSON_Delete(source);
         cJSON_Delete(outputs);
         goto unavailable;
      }
   }
   const cJSON *last = NULL, *entry;
   cJSON_ArrayForEach(entry, events)
   {
      const char *entry_turn = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "turn_id"));
      const char *entry_actor = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "actor"));
      const cJSON *payload = cJSON_GetObjectItem(entry, "payload");
      const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "kind"));
      const cJSON *message = cJSON_GetObjectItem(payload, "payload");
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(message, "role"));
      if (!entry_turn || strcmp(entry_turn, turn) || !kind || strcmp(kind, "message") || !role ||
          strcmp(role, "assistant"))
         continue;
      aimee_message_t parsed = {0};
      if (!entry_actor || strcmp(entry_actor, owner) || aimee_ir_message_parse(message, &parsed))
      {
         aimee_message_free(&parsed);
         cJSON_Delete(source);
         cJSON_Delete(outputs);
         goto unavailable;
      }
      aimee_message_free(&parsed);
      cJSON *copy = cJSON_Duplicate(message, 1);
      if (!copy || !cJSON_AddItemToArray(outputs, copy))
      {
         cJSON_Delete(copy);
         cJSON_Delete(source);
         cJSON_Delete(outputs);
         goto unavailable;
      }
      last = message;
   }
   cJSON_AddStringToObject(resp, "canonical_state", "available");
   cJSON_AddItemToObject(resp, "canonical_source", source);
   cJSON_AddItemToObject(resp, "outputs", outputs);
   if (last)
   {
      cJSON_AddItemToObject(resp, "output", cJSON_Duplicate(last, 1));
      const char *status =
          cJSON_GetStringValue(cJSON_GetObjectItem(observation, "execution_status"));
      const char *job_status = cJSON_GetStringValue(cJSON_GetObjectItem(resp, "job_status"));
      if (status && !strcmp(status, "completed") && job_status && !strcmp(job_status, "done"))
      {
         aimee_message_t parsed = {0};
         if (!aimee_ir_message_parse(last, &parsed))
         {
            aimee_response_t view = {.content = parsed.blocks, .n_content = parsed.n_blocks};
            char *display = aimee_ir_response_text_alloc(&view);
            if (display)
               cJSON_AddStringToObject(resp, "result", display);
            free(display);
         }
         aimee_message_free(&parsed);
      }
   }
   if (full)
   {
      cJSON_AddItemToObject(resp, "history", cJSON_Duplicate(history, 1));
      cJSON_AddItemToObject(resp, "events", cJSON_Duplicate(events, 1));
   }
   cJSON_Delete(observation);
   return 0;
unavailable:
   cJSON_Delete(observation);
   cJSON_AddStringToObject(resp, "canonical_state", "unavailable");
   return -1;
}

static void delegate_status_populate_job(cJSON *resp, int job_id, int full)
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
      cJSON_AddStringToObject(resp, "job_status", job.status);
      int observation = server_delegate_job_observation(resp, job_id, full);
      if (observation == -2)
      {
         cJSON_ReplaceItemInObject(resp, "job_status", cJSON_CreateString("forbidden"));
         db1_agent_job_free(&job);
         return;
      }
      cJSON_AddStringToObject(resp, "role", job.role);
      cJSON_AddStringToObject(resp, "agent_name", job.agent_name);
      cJSON_AddNumberToObject(resp, "cursor_turn", job.cursor_turn);
      if (observation >= 0 && job.result[0] &&
          (!strcmp(job.status, "failed") || !strcmp(job.status, "cancelled") ||
           !strcmp(job.status, "partial")))
         cJSON_AddStringToObject(resp, "diagnostic", job.result);
      else if (observation == 1 && full && job.result[0])
         cJSON_AddStringToObject(resp, "legacy_result", job.result); /* Inert archive only. */
      if (job.heartbeat_at[0])
         cJSON_AddStringToObject(resp, "heartbeat_at", job.heartbeat_at);
      if (job.current_tool[0])
         cJSON_AddStringToObject(resp, "current_tool", job.current_tool);
      cJSON_AddNumberToObject(resp, "api_call_count", job.api_call_count);
      /* cost_known distinguishes an unmeasured job from a genuinely free one.
       * A poller that commits cost_usd as measured spend must check it. */
      cJSON_AddNumberToObject(resp, "cost_usd", job.cost_usd);
      cJSON_AddBoolToObject(resp, "cost_known", job.cost_known != 0);
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
         delegate_status_populate_job(job_obj, item->valueint, cJSON_IsTrue(jfull));
         server_job_reply_fit(job_obj);
         cJSON_AddItemToArray(jobs, job_obj);
      }
      /* Each job fits alone; several together may not. Their transcripts go
       * first, then the last outputs. */
      static const char *arrays[] = {"events", "history", "outputs", "output"};
      for (size_t i = 0;
           i < sizeof arrays / sizeof *arrays && job_reply_size(resp) > JOB_REPLY_BUDGET; i++)
         cJSON_ArrayForEach(item, jobs) job_array_omit(item, arrays[i]);
   }
   else
   {
      delegate_status_populate_job(resp, jid->valueint, cJSON_IsTrue(jfull));
      server_job_reply_fit(resp);
   }

   return server_send_ok(conn, resp);
}
