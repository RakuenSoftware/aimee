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
   /* Gap replay can already have delivered a later callback's event. Never
    * duplicate it or rewind the live cursor when its producer catches up. */
   if (c->ir_sequence[0] && target <= strtoull(c->ir_sequence, NULL, 10))
      return 0;
   if (c->ir_delivery && c->conn_alive && c->ir_sequence[0] &&
       (!json || target > strtoull(c->ir_sequence, NULL, 10) + 1))
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

/* Called on the producer thread. Retained callback writers never borrow the
 * browser connection; delivery reads their committed tail from the same log. */
int server_chat_ir_progress(compute_ctx_t *c)
{
   if (!c || !c->ir_enabled || !c->ir_delivery || !c->conn_alive)
      return 0;
   char sequence[32];
   if (!c->ir_sequence[0] || server_session_capture_cursor(c->ir_capture, sequence) ||
       chat_ir_deliver(c, NULL, sequence))
   {
      c->ir_failed = 1;
      server_session_capture_fail(c->ir_capture);
      return -1;
   }
   return 0;
}

/* The session log is committed before any delivery. A dead browser socket is
 * only a lost subscriber; storage failure stops capture and the host turn. */
static int chat_ir_sink(const aimee_session_event_t *event, void *context)
{
   compute_ctx_t *c = context;
   if (c->ir_failed || server_control_scope_failed(c->ir_controls) ||
       server_session_capture_failed(c->ir_capture))
   {
      c->ir_failed = 1;
      return -1;
   }
   char *json = NULL, sequence[32];
   if (server_session_capture_append(c->ir_capture, event, &json, sequence))
   {
      c->ir_failed = 1;
      return -1;
   }
   int rc = chat_ir_deliver(c, json, sequence);
   free(json);
   if (rc != 0)
   {
      c->ir_failed = 1;
      server_session_capture_fail(c->ir_capture);
   }
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
   c->ir_capture =
       server_session_capture_open(c->ir_session, c->ir_turn, c->vault_principal, c->ir_session);
   if (!c->ir_capture)
   {
      server_control_scope_bind(c->ir_previous_controls);
      server_control_scope_close(c->ir_controls);
      c->ir_controls = NULL;
      return -1;
   }
   c->ir_previous_capture = server_session_capture_bind(c->ir_capture);
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
   server_session_capture_counts_t counts = {0};
   if (server_session_capture_counts(c->ir_capture, &counts))
      error = "canonical capture owner unavailable";
   if (!error && !agent_request_cancelled() && !counts.model_events)
      error = "Selected harness produced no canonical model output";
   aimee_session_event_t event = {.kind = AIMEE_SESSION_EXECUTION,
                                  .execution_status = agent_request_cancelled()
                                                          ? "cancelled"
                                                          : (error ? "failed" : "completed"),
                                  .execution_error = error};
   return aimee_ir_session_emit(&event);
}

void server_chat_ir_close(compute_ctx_t *c)
{
   if (!c->ir_enabled)
      return;
   if (!c->ir_completed)
      server_chat_ir_finish(c, "execution ended without completion");
   aimee_ir_session_set_sink(c->ir_previous_sink, c->ir_previous_context);
   server_session_capture_bind(c->ir_previous_capture);
   server_session_capture_close(c->ir_capture);
   server_control_scope_bind(c->ir_previous_controls);
   server_control_scope_close(c->ir_controls);
   c->ir_capture = NULL;
   c->ir_controls = NULL;
   c->ir_enabled = 0;
}
