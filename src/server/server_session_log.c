#include "server_session_log.h"
#include "server_learning_outcome.h"
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
   char parent[128], role[128];
   int announced;
   unsigned long long counter;
   unsigned long long model_completions;
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
   /* Model tool identities are canonical content. Qualify only transport
    * message/event identities; replay must retain the actual native tool IDs. */
   char id[96], sequence[32];
   snprintf(id, sizeof id, "%s:%llu", scope->turn, ++scope->counter);
   char *wire = cJSON_PrintUnformatted(payload);
   int rc = wire ? db1_session_ir_append(scope->session, id, scope->turn, scope->principal, wire,
                                         sequence)
                 : -1;
   free(wire);
   if (!rc && event->kind == AIMEE_SESSION_EXECUTION && strcmp(event->execution_status, "running"))
      server_learning_observe_terminal(scope->session, scope->turn, sequence, id, scope->principal,
                                       payload);
   cJSON_Delete(payload);
   const char *role =
       event->kind == AIMEE_SESSION_MESSAGE && event->message ? event->message->role : event->role;
   if (!rc && role && !strcmp(role, "assistant") &&
       (event->kind == AIMEE_SESSION_MESSAGE ||
        (event->kind == AIMEE_SESSION_DELTA && event->delta &&
         event->delta->type == AIMEE_DELTA_TURN_STOP)))
      scope->model_completions++;
   return rc;
}
/* Worker attestation identifies the parent even when no HTTP sink crosses the
 * thread boundary. Only a reference is appended there; all content stays private. */
static int publish_child(server_session_log_t *scope, const char *status)
{
   if (!scope->parent[0])
      return 0;
   aimee_session_event_t event = {.kind = AIMEE_SESSION_CHILD_EXECUTION,
                                  .child_session = scope->session,
                                  .child_turn = scope->turn,
                                  .child_role = scope->role,
                                  .execution_status = status};
   cJSON *payload = aimee_ir_session_event_json(&event);
   char *wire = payload ? cJSON_PrintUnformatted(payload) : NULL;
   cJSON_Delete(payload);
   char id[96], sequence[32];
   snprintf(id, sizeof id, "%s:child:%s", scope->turn, status);
   int rc = wire ? db1_session_ir_append(scope->parent, id, scope->turn, scope->principal, wire,
                                         sequence)
                 : -1;
   free(wire);
   if (rc)
      aimee_ir_session_fail_capture();
   return rc;
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
      if (!role[0] || strlen(role) >= sizeof scope->role)
         goto fail;
      snprintf(scope->role, sizeof scope->role, "%s", role);
      if (current && strcmp(current->principal, scope->principal))
         goto fail;
      snprintf(scope->parent, sizeof scope->parent, "%s",
               current ? current->session : attestation.session_id);
      int job = agent_get_durable_job_id();
      char key[192], digest[65];
      snprintf(key, sizeof key, "%s:%d:%s", scope->parent, job, job > 0 ? "durable" : scope->turn);
      if (aimee_sha256_hex(key, strlen(key), digest))
         goto fail;
      snprintf(scope->session, sizeof scope->session, "delegate-%s", digest);
   }
   else
      snprintf(scope->session, sizeof scope->session, "%s", attestation.session_id);
   if (ensure_owner(attestation.session_id, "native", scope->principal) ||
       (role && (ensure_owner(scope->parent, "native", scope->principal) ||
                 ensure_owner(scope->session, "delegate", scope->principal))))
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
   if (aimee_ir_session_emit(&started) || publish_child(scope, "running"))
      return -1;
   scope->announced = scope->parent[0] != 0;
   if (aimee_ir_session_emit(&user))
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
   int missing = success && !scope->model_completions;
   if (missing)
   {
      success = 0;
      error = "execution completed without canonical model output";
   }
   if (aimee_ir_session_capture_failed())
      success = 0;
   aimee_session_event_t finished = {.kind = AIMEE_SESSION_EXECUTION,
                                     .execution_status = agent_request_cancelled() ? "cancelled"
                                                         : success                 ? "completed"
                                                                                   : "failed",
                                     .execution_error = error && error[0] ? error : NULL};
   int failed = aimee_ir_session_emit(&finished) != 0 || aimee_ir_session_capture_failed();
   failed = failed || missing;
   if (scope->announced && publish_child(scope, failed ? "failed" : finished.execution_status))
      failed = 1;
   current = scope->previous;
   aimee_ir_session_restore_sink(scope->previous_sink, scope->previous_context, failed);
   server_control_scope_bind(scope->previous_controls);
   free(scope);
   return failed ? -1 : 0;
}

int server_session_log_identity(char *session, size_t capacity, char *principal,
                                size_t principal_capacity)
{
   if (!current)
      return 1;
   if (!session || !principal || strlen(current->session) >= capacity ||
       strlen(current->principal) >= principal_capacity)
      return -1;
   snprintf(session, capacity, "%s", current->session);
   snprintf(principal, principal_capacity, "%s", current->principal);
   return 0;
}
