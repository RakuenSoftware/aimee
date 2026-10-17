#include "server_session_log.h"
#include "server_session_control.h"
#include "agent_config.h"
#include "agent_tasks.h"
#include "aimee_sha256.h"
#include "db1_client/primary_sessions.h"
#include "db1_client/server_sessions.h"
#include <aimee/ir/session_event.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct server_session_log
{
   char session[128], turn[33], principal[128];
   unsigned long long counter;
   cJSON *tool_ids;
   aimee_ir_session_sink_fn previous_sink;
   void *previous_context;
   server_control_scope_t *previous_controls;
   struct server_session_log *previous;
};
static _Thread_local server_session_log_t *current;
static int random_id(char out[33])
{
   unsigned char bytes[16];
   if (RAND_bytes(bytes, sizeof bytes) != 1)
      return -1;
   for (int i = 0; i < 16; i++)
      snprintf(out + 2 * i, 3, "%02x", bytes[i]);
   return 0;
}
static int log_event(const aimee_session_event_t *event, void *context)
{
   server_session_log_t *scope = context;
   cJSON *payload = aimee_ir_session_event_json(event);
   if (!payload)
      return -1;
   /* Native IDs can recur after a resumed harness starts another model turn. */
   if (event->message_id)
   {
      size_t n = strlen(scope->turn) + strlen(event->message_id) + 2;
      char *id = malloc(n);
      if (!id)
      {
         cJSON_Delete(payload);
         return -1;
      }
      snprintf(id, n, "%s:%s", scope->turn, event->message_id);
      int ok =
          cJSON_ReplaceItemInObjectCaseSensitive(payload, "message_id", cJSON_CreateString(id));
      free(id);
      if (!ok)
      {
         cJSON_Delete(payload);
         return -1;
      }
   }
   cJSON *body = cJSON_GetObjectItemCaseSensitive(payload, "payload");
   cJSON *blocks = event->kind == AIMEE_SESSION_MESSAGE
                       ? cJSON_GetObjectItemCaseSensitive(body, "blocks")
                       : NULL;
   int count = blocks ? cJSON_GetArraySize(blocks) : event->kind == AIMEE_SESSION_DELTA ? 1 : 0;
   for (int i = 0; i < count; i++)
   {
      cJSON *block = blocks ? cJSON_GetArrayItem(blocks, i) : body;
      const char *native = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "tool_id"));
      if (!native || !native[0])
         continue;
      const char *kind =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, blocks ? "type" : "kind"));
      if (!scope->tool_ids)
         scope->tool_ids = cJSON_CreateObject();
      if (!scope->tool_ids)
         goto qualification_failed;
      cJSON *mapped = cJSON_GetObjectItemCaseSensitive(scope->tool_ids, native);
      if (kind && !strcmp(kind, "tool_use") &&
          (blocks || (event->delta && event->delta->type == AIMEE_DELTA_BLOCK_START)))
      {
         if (!event->message_id)
            goto qualification_failed;
         size_t n = strlen(scope->turn) + strlen(event->message_id) + strlen(native) + 3;
         char *qualified = malloc(n);
         if (!qualified)
            goto qualification_failed;
         snprintf(qualified, n, "%s:%s:%s", scope->turn, event->message_id, native);
         cJSON_DeleteItemFromObjectCaseSensitive(scope->tool_ids, native);
         mapped = cJSON_AddStringToObject(scope->tool_ids, native, qualified);
         free(qualified);
         if (!mapped)
            goto qualification_failed;
      }
      if (mapped && !cJSON_ReplaceItemInObjectCaseSensitive(
                        block, "tool_id", cJSON_CreateString(mapped->valuestring)))
         goto qualification_failed;
   }
   char id[96], sequence[32];
   snprintf(id, sizeof id, "%s:%llu", scope->turn, ++scope->counter);
   char *wire = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   int rc = wire ? db1_session_ir_append(scope->session, id, scope->turn, scope->principal, wire,
                                         sequence)
                 : -1;
   free(wire);
   return rc;
qualification_failed:
   cJSON_Delete(payload);
   return -1;
}
static int ensure_owner(const char *session, const char *type, const char *principal)
{
   db1_server_session_t row;
   if (db1_server_session_get(session, &row) != 0)
   {
      // Another admitted worker may create the same parent concurrently.
      (void)db1_server_session_create(session, type, principal);
      if (db1_server_session_get(session, &row) != 0)
         return -1;
   }
   return strcmp(row.principal, principal) ? -1 : 0;
}
int server_session_log_begin(const char *role, const char *prompt, server_session_log_t **out)
{
   if (!out)
      return -1;
   *out = NULL;
   agent_request_creds_t attestation;
   agent_request_creds_snapshot(&attestation);
   if (!attestation.session_id[0])
      return 1; /* A non-session internal completion has no user history. */
   if (!attestation.vault_principal[0] || aimee_ir_session_capture_failed())
      return -1;
   void *previous_context;
   aimee_ir_session_sink_fn previous_sink = aimee_ir_session_get_sink(&previous_context);
   if (!role && previous_sink)
      return 1;
   server_session_log_t *scope = calloc(1, sizeof *scope);
   if (!scope)
      return -1;
   snprintf(scope->principal, sizeof scope->principal, "%s", attestation.vault_principal);
   if (random_id(scope->turn))
      goto fail;
   if (role)
   {
      int job = agent_get_durable_job_id();
      char key[192], digest[65];
      snprintf(key, sizeof key, "%s:%d:%s", attestation.session_id, job,
               job > 0 ? "durable" : scope->turn);
      if (aimee_sha256_hex(key, strlen(key), digest))
         goto fail;
      snprintf(scope->session, sizeof scope->session, "delegate-%s", digest);
   }
   else
      snprintf(scope->session, sizeof scope->session, "%s", attestation.session_id);
   if (ensure_owner(attestation.session_id, "native", scope->principal) ||
       (role && ensure_owner(scope->session, "delegate", scope->principal)))
      goto fail;
   scope->previous_sink = previous_sink;
   scope->previous_context = previous_context;
   scope->previous = current;
   current = scope;
   /* Child model conversations never use their parent's subscriber or controls. */
   scope->previous_controls = server_control_scope_bind(NULL);
   aimee_ir_session_set_sink(log_event, scope);
   *out = scope;
   aimee_session_event_t started = {.kind = AIMEE_SESSION_EXECUTION, .execution_status = "running"};
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = (char *)(prompt ? prompt : "")};
   aimee_message_t message = {.role = "user", .blocks = &block, .n_blocks = 1};
   aimee_session_event_t user = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "user", .message = &message};
   if (aimee_ir_session_emit(&started) || aimee_ir_session_emit(&user))
      return -1; /* Caller must finish *out, including failed admission. */
   return 0;
fail:
   free(scope);
   return -1;
}
int server_session_log_active(void)
{
   return current != NULL;
}
int server_session_log_history_load(cJSON **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!current)
      return 1;
   char *json = NULL;
   int rc = db1_session_ir_history_load(current->session, "primary", current->principal,
                                        current->turn, &json);
   if (rc == 0)
   {
      *out = cJSON_Parse(json);
      if (!cJSON_IsArray(*out) || aimee_ir_history_repair(*out) < 0)
      {
         cJSON_Delete(*out);
         *out = NULL;
         rc = -1;
      }
   }
   free(json);
   return rc;
}
int server_session_log_history_save(const cJSON *messages)
{
   if (!current)
      return 0;
   char checkpoint[33];
   char *json = cJSON_IsArray(messages) ? cJSON_PrintUnformatted(messages) : NULL;
   int rc = json && !random_id(checkpoint)
                ? db1_session_ir_history_save(current->session, checkpoint, "primary",
                                              current->principal, json)
                : -1;
   free(json);
   if (rc)
      aimee_ir_session_fail_capture();
   return rc;
}
int server_session_log_finish(server_session_log_t *scope, int success, const char *error)
{
   if (!scope)
      return 0;
   aimee_session_event_t finished = {.kind = AIMEE_SESSION_EXECUTION,
                                     .execution_status = agent_request_cancelled() ? "cancelled"
                                                         : success                 ? "completed"
                                                                                   : "failed",
                                     .execution_error = error && error[0] ? error : NULL};
   int failed = aimee_ir_session_emit(&finished) != 0 || aimee_ir_session_capture_failed();
   current = scope->previous;
   aimee_ir_session_set_sink(scope->previous_sink, scope->previous_context);
   server_control_scope_bind(scope->previous_controls);
   cJSON_Delete(scope->tool_ids);
   free(scope);
   if (failed)
      aimee_ir_session_fail_capture();
   return failed ? -1 : 0;
}
