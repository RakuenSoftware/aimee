/* Durable session capture owned by the server compute task. */
#include "server_compute_impl.h"
#include "server_learning_outcome.h"
#include "agent_config.h"
#include "presence.h"
#include "db1_client/server_sessions.h"
#include "db1_client/primary_sessions.h"
#include "cJSON.h"
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int chat_ir_write_envelope(compute_ctx_t *c, const char *json)
{
   if (!c->ir_delivery)
      return 0;
   pthread_mutex_lock(c->write_mutex);
   if (c->conn_alive &&
       (write_all(c->conn_fd, json, strlen(json)) != 0 || write_all(c->conn_fd, "\n", 1) != 0))
      c->conn_alive = 0;
   pthread_mutex_unlock(c->write_mutex);
   return 0;
}

/* Other server owners can commit checkpoints between adapter emissions. Fill
 * that gap from the same durable log so a live subscriber sees exactly the
 * sequence it will see on reload. A disconnected subscriber needs no delivery. */
static int chat_ir_deliver(compute_ctx_t *c, const char *json, const char *sequence)
{
   unsigned long long target = strtoull(sequence, NULL, 10);
   if (c->ir_delivery && c->conn_alive && c->ir_sequence[0] &&
       target > strtoull(c->ir_sequence, NULL, 10) + 1)
   {
      while (strtoull(c->ir_sequence, NULL, 10) < target)
      {
         char *page = db1_session_ir_page(c->ir_session, c->ir_sequence, 128, c->vault_principal);
         cJSON *events = page ? cJSON_Parse(page) : NULL;
         free(page);
         if (!cJSON_IsArray(events) || !cJSON_GetArraySize(events))
         {
            cJSON_Delete(events);
            return -1;
         }
         const cJSON *event;
         cJSON_ArrayForEach(event, events)
         {
            const char *next =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "sequence"));
            if (!next || strtoull(next, NULL, 10) != strtoull(c->ir_sequence, NULL, 10) + 1 ||
                strlen(next) >= sizeof c->ir_sequence)
            {
               cJSON_Delete(events);
               return -1;
            }
            char *wire = cJSON_PrintUnformatted(event);
            if (!wire)
            {
               cJSON_Delete(events);
               return -1;
            }
            chat_ir_write_envelope(c, wire);
            free(wire);
            snprintf(c->ir_sequence, sizeof c->ir_sequence, "%s", next);
         }
         cJSON_Delete(events);
      }
      return 0;
   }
   chat_ir_write_envelope(c, json);
   snprintf(c->ir_sequence, sizeof c->ir_sequence, "%s", sequence);
   return 0;
}

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
   if (event->kind == AIMEE_SESSION_CONTROL_REQUEST &&
       server_control_register(c->ir_controls, event->request_id, event->control_kind,
                               event->control) != 0)
   {
      c->ir_failed = 1;
      return -1;
   }
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
   /* Tool identities are model content. The projection keys calls by host
    * message/block identity; rewriting tool IDs here corrupts native replay. */
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
   if (event->kind == AIMEE_SESSION_EXECUTION && strcmp(event->execution_status, "running"))
      server_learning_observe_terminal(c->ir_session, c->ir_turn, sequence, event_id,
                                       c->vault_principal, payload);
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
   int rc = chat_ir_deliver(c, json, sequence);
   free(json);
   if (rc != 0)
      c->ir_failed = 1;
   return rc;
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
   c->ir_controls = server_control_scope_open(c->ir_session, c->ir_turn, c->vault_principal);
   if (!c->ir_controls)
      return -1;
   c->ir_previous_controls = server_control_scope_bind(c->ir_controls);
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
