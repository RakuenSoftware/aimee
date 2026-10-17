/* Durable session capture owned by the server compute task. */
#include "server_compute_impl.h"
#include "agent_config.h"
#include "presence.h"
#include "db1_client/server_sessions.h"
#include "db1_client/primary_sessions.h"
#include "cJSON.h"
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The session log is committed before any delivery. A dead browser socket is
 * only a lost subscriber; storage failure stops capture and the host turn. */
static int chat_ir_sink(const aimee_session_event_t *event, void *context)
{
   compute_ctx_t *c = context;
   if (c->ir_failed)
      return -1;
   const char *role =
       event->kind == AIMEE_SESSION_MESSAGE && event->message ? event->message->role : event->role;
   if (role && !strcmp(role, "assistant"))
      c->ir_model_events++;
   cJSON *payload = aimee_ir_session_event_json(event);
   if (!payload)
   {
      c->ir_failed = 1;
      return -1;
   }
   if (event->message_id)
   {
      size_t n = strlen(c->ir_turn) + strlen(event->message_id) + 2;
      char *id = malloc(n);
      if (!id)
      {
         cJSON_Delete(payload);
         c->ir_failed = 1;
         return -1;
      }
      snprintf(id, n, "%s:%s", c->ir_turn, event->message_id);
      cJSON_ReplaceItemInObjectCaseSensitive(payload, "message_id", cJSON_CreateString(id));
      free(id);
   }
   cJSON *body = cJSON_GetObjectItemCaseSensitive(payload, "payload");
   cJSON *blocks = event->kind == AIMEE_SESSION_MESSAGE
                       ? cJSON_GetObjectItemCaseSensitive(body, "blocks")
                       : NULL;
   int count = blocks ? cJSON_GetArraySize(blocks) : (event->kind == AIMEE_SESSION_DELTA ? 1 : 0);
   for (int i = 0; i < count; i++)
   {
      cJSON *block = blocks ? cJSON_GetArrayItem(blocks, i) : body;
      cJSON *id = cJSON_GetObjectItemCaseSensitive(block, "tool_id");
      if (!cJSON_IsString(id) || !id->valuestring[0])
         continue;
      const cJSON *kind = cJSON_GetObjectItemCaseSensitive(block, blocks ? "type" : "kind");
      int use = cJSON_IsString(kind) && !strcmp(kind->valuestring, "tool_use");
      if (!c->ir_tool_ids)
         c->ir_tool_ids = cJSON_CreateObject();
      if (!c->ir_tool_ids)
      {
         cJSON_Delete(payload);
         c->ir_failed = 1;
         return -1;
      }
      cJSON *mapped = cJSON_GetObjectItemCaseSensitive(c->ir_tool_ids, id->valuestring);
      if (use && (blocks || (event->delta && event->delta->type == AIMEE_DELTA_BLOCK_START)))
      {
         size_t n = strlen(c->ir_turn) + strlen(event->message_id) + strlen(id->valuestring) + 3;
         char *qualified = malloc(n);
         if (!qualified)
         {
            cJSON_Delete(payload);
            c->ir_failed = 1;
            return -1;
         }
         snprintf(qualified, n, "%s:%s:%s", c->ir_turn, event->message_id, id->valuestring);
         cJSON_DeleteItemFromObjectCaseSensitive(c->ir_tool_ids, id->valuestring);
         mapped = cJSON_AddStringToObject(c->ir_tool_ids, id->valuestring, qualified);
         free(qualified);
      }
      if (mapped && cJSON_IsString(mapped))
         cJSON_ReplaceItemInObjectCaseSensitive(block, "tool_id",
                                                cJSON_CreateString(mapped->valuestring));
   }
   if (event->request_id)
   {
      size_t n = strlen(c->ir_turn) + strlen(event->request_id) + 2;
      char *id = malloc(n);
      if (!id)
      {
         cJSON_Delete(payload);
         c->ir_failed = 1;
         return -1;
      }
      snprintf(id, n, "%s:%s", c->ir_turn, event->request_id);
      cJSON_ReplaceItemInObjectCaseSensitive(payload, "request_id", cJSON_CreateString(id));
      free(id);
   }
   char event_id[192], sequence[32];
   snprintf(event_id, sizeof(event_id), "%s:%llu", c->ir_turn, ++c->ir_counter);
   char *json = cJSON_PrintUnformatted(payload);
   if (!json || db1_session_ir_append(c->ir_session, event_id, c->ir_turn, c->vault_principal, json,
                                      sequence) != 0)
   {
      free(json);
      cJSON_Delete(payload);
      c->ir_failed = 1;
      return -1;
   }
   free(json);
   cJSON *envelope = cJSON_CreateObject();
   cJSON_AddNumberToObject(envelope, "version", 1);
   cJSON_AddStringToObject(envelope, "session_id", c->ir_session);
   cJSON_AddStringToObject(envelope, "sequence", sequence);
   cJSON_AddStringToObject(envelope, "event_id", event_id);
   cJSON_AddStringToObject(envelope, "turn_id", c->ir_turn);
   cJSON_AddStringToObject(envelope, "actor", c->vault_principal);
   cJSON_AddItemToObject(envelope, "event", payload);
   json = cJSON_PrintUnformatted(envelope);
   cJSON_Delete(envelope);
   if (!json)
   {
      c->ir_failed = 1;
      return -1;
   }
   pthread_mutex_lock(c->write_mutex);
   if (c->conn_alive &&
       (write_all(c->conn_fd, json, strlen(json)) != 0 || write_all(c->conn_fd, "\n", 1) != 0))
      c->conn_alive = 0;
   pthread_mutex_unlock(c->write_mutex);
   free(json);
   return 0;
}

int server_chat_ir_begin(compute_ctx_t *c, const char *session, const char *text)
{
   if (!c->presence_locked || !session || !session[0] || strlen(session) >= sizeof(c->ir_session) ||
       !c->vault_principal[0])
      return -1;
   if (!presence_turn_host_own(c->presence_session, c->presence_turn_id))
      return -1;
   db1_server_session_t row;
   int found = db1_server_session_get(session, &row);
   if (found == 0 && strcmp(row.principal, c->vault_principal))
      return -1;
   if (found != 0 && db1_server_session_create(session, "webchat", c->vault_principal) != 0)
      return -1;
   snprintf(c->ir_session, sizeof(c->ir_session), "%s", session);
   /* Random process-independent turn identity, also for requests without a presence attachment. */
   unsigned char random[16];
   if (RAND_bytes(random, sizeof(random)) != 1)
      return -1;
   for (int i = 0; i < 16; i++)
      snprintf(c->ir_turn + 2 * i, 3, "%02x", random[i]);
   c->ir_previous_sink = aimee_ir_session_get_sink(&c->ir_previous_context);
   c->ir_enabled = 1;
   aimee_ir_session_set_sink(chat_ir_sink, c);
   aimee_session_event_t lifecycle = {.kind = AIMEE_SESSION_EXECUTION,
                                      .execution_status = "running"};
   if (aimee_ir_session_emit(&lifecycle) != 0)
      return -1;
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = (char *)text};
   aimee_message_t message = {.role = "user", .blocks = &block, .n_blocks = 1};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "user", .message = &message};
   return aimee_ir_session_emit(&event);
}

int server_chat_ir_finish(compute_ctx_t *c, const char *error)
{
   if (!c->ir_enabled || c->ir_completed)
      return 0;
   c->ir_completed = 1;
   if (!error && !agent_request_cancelled() && !c->ir_model_events)
      error = "Selected harness produced no canonical model output";
   aimee_session_event_t event = {.kind = AIMEE_SESSION_EXECUTION,
                                  .execution_status = agent_request_cancelled()
                                                          ? "cancelled"
                                                          : (error ? "failed" : "completed"),
                                  .execution_error = error};
   return aimee_ir_session_emit(&event);
}
