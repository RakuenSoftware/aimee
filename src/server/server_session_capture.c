#include "server_session_capture.h"
#include "server_session_control.h"
#include "server_learning_outcome.h"
#include "db1_client/primary_sessions.h"
#include "vault_principal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct server_session_capture
{
   char session[128], turn[128], principal[VAULT_PRINCIPAL_MAX], execution_session[128];
   pthread_mutex_t lock;
   atomic_uint references;
   atomic_int closed, failed;
   server_control_scope_t *controls;
   server_session_capture_counts_t counts;
};
static _Thread_local server_session_capture_t *current;
server_session_capture_t *server_session_capture_current(void)
{
   return current;
}
server_session_capture_t *server_session_capture_bind(server_session_capture_t *owner)
{
   server_session_capture_t *previous = current;
   current = owner;
   return previous;
}
server_session_capture_t *server_session_capture_open(const char *session, const char *turn,
                                                      const char *principal,
                                                      const char *execution_session)
{
   if (!session || !session[0] || strlen(session) >= 128 || !turn || !turn[0] ||
       strlen(turn) >= 128 || !principal || !principal[0] ||
       strlen(principal) >= VAULT_PRINCIPAL_MAX || !execution_session || !execution_session[0] ||
       strlen(execution_session) >= 128)
      return NULL;
   server_session_capture_t *owner = calloc(1, sizeof *owner);
   if (!owner)
      return NULL;
   if (pthread_mutex_init(&owner->lock, NULL))
   {
      free(owner);
      return NULL;
   }
   atomic_init(&owner->references, 1);
   atomic_init(&owner->closed, 0);
   atomic_init(&owner->failed, 0);
   snprintf(owner->session, sizeof owner->session, "%s", session);
   snprintf(owner->turn, sizeof owner->turn, "%s", turn);
   snprintf(owner->principal, sizeof owner->principal, "%s", principal);
   snprintf(owner->execution_session, sizeof owner->execution_session, "%s", execution_session);
   if (server_control_scope_capture(session, principal, &owner->controls))
   {
      server_session_capture_release(owner);
      return NULL;
   }
   return owner;
}
int server_session_capture_failed(const server_session_capture_t *owner)
{
   return owner && (atomic_load(&owner->failed) || server_control_scope_failed(owner->controls));
}
int server_session_capture_accepting(const server_session_capture_t *owner)
{
   return owner && !atomic_load(&owner->closed) && !server_session_capture_failed(owner);
}
void server_session_capture_fail(server_session_capture_t *owner)
{
   if (owner)
      atomic_store(&owner->failed, 1);
}
int server_session_capture_retain_current(const char *execution_session, const char *principal,
                                          server_session_capture_t **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!current)
      return 0;
   if (!execution_session || !principal || strcmp(execution_session, current->execution_session) ||
       strcmp(principal, current->principal) || atomic_load(&current->closed) ||
       server_session_capture_failed(current))
      return -1;
   atomic_fetch_add(&current->references, 1);
   *out = current;
   return 0;
}
void server_session_capture_release(server_session_capture_t *owner)
{
   if (owner && atomic_fetch_sub(&owner->references, 1) == 1)
   {
      server_control_scope_release(owner->controls);
      pthread_mutex_destroy(&owner->lock);
      free(owner);
   }
}
void server_session_capture_close(server_session_capture_t *owner)
{
   if (!owner)
      return;
   pthread_mutex_lock(&owner->lock);
   atomic_store(&owner->closed, 1);
   pthread_mutex_unlock(&owner->lock);
   if (current == owner)
      current = NULL;
   server_session_capture_release(owner);
}
int server_session_capture_counts(server_session_capture_t *owner,
                                  server_session_capture_counts_t *out)
{
   if (!owner || !out)
      return -1;
   pthread_mutex_lock(&owner->lock);
   *out = owner->counts;
   pthread_mutex_unlock(&owner->lock);
   return 0;
}
static int qualify(cJSON *payload, const char *field, const char *turn, const char *native)
{
   if (!native)
      return 0;
   size_t length = strlen(turn) + strlen(native) + 2;
   char *id = malloc(length);
   if (!id)
      return -1;
   snprintf(id, length, "%s:%s", turn, native);
   cJSON *value = cJSON_CreateString(id);
   free(id);
   if (!value || !cJSON_ReplaceItemInObjectCaseSensitive(payload, field, value))
   {
      cJSON_Delete(value);
      return -1;
   }
   return 0;
}
static char *make_envelope(const server_session_capture_t *owner, const char *event_id,
                           const char *sequence, cJSON **payload)
{
   cJSON *envelope = cJSON_CreateObject();
   int valid = envelope && *payload && cJSON_AddNumberToObject(envelope, "version", 1) &&
               cJSON_AddStringToObject(envelope, "session_id", owner->session) &&
               cJSON_AddStringToObject(envelope, "sequence", sequence) &&
               cJSON_AddStringToObject(envelope, "event_id", event_id) &&
               cJSON_AddStringToObject(envelope, "turn_id", owner->turn) &&
               cJSON_AddStringToObject(envelope, "actor", owner->principal);
   if (valid && cJSON_AddItemToObject(envelope, "event", *payload))
      *payload = NULL;
   else
      valid = 0;
   char *wire = valid ? cJSON_PrintUnformatted(envelope) : NULL;
   cJSON_Delete(envelope);
   return wire;
}
int server_session_capture_append(server_session_capture_t *owner,
                                  const aimee_session_event_t *event, char **envelope,
                                  char sequence[32])
{
   if (envelope)
      *envelope = NULL;
   if (sequence)
      sequence[0] = 0;
   if (!owner || !event)
      return -1;
   pthread_mutex_lock(&owner->lock);
   int failure_terminal = event->kind == AIMEE_SESSION_EXECUTION && event->execution_status &&
                          (!strcmp(event->execution_status, "failed") ||
                           !strcmp(event->execution_status, "cancelled"));
   if (atomic_load(&owner->closed) || (server_session_capture_failed(owner) && !failure_terminal))
   {
      pthread_mutex_unlock(&owner->lock);
      return -1;
   }
   cJSON *payload = aimee_ir_session_event_json(event);
   int result = -1;
   if (!payload || qualify(payload, "message_id", owner->turn, event->message_id) ||
       qualify(payload, "request_id", owner->turn, event->request_id))
      goto finish;
   if (event->kind == AIMEE_SESSION_CONTROL_REQUEST && owner->controls &&
       server_control_register(owner->controls, event->request_id, event->control_kind,
                               event->control))
      goto finish;
   char event_id[192], committed[32];
   snprintf(event_id, sizeof event_id, "%s:%llu", owner->turn, ++owner->counts.counter);
   char *wire = cJSON_PrintUnformatted(payload);
   result = wire ? db1_session_ir_append(owner->session, event_id, owner->turn, owner->principal,
                                         wire, committed)
                 : -1;
   free(wire);
   if (result)
      goto finish;
   if (event->kind == AIMEE_SESSION_EXECUTION && strcmp(event->execution_status, "running"))
      server_learning_observe_terminal(owner->session, owner->turn, committed, event_id,
                                       owner->principal, payload);
   if (envelope)
   {
      *envelope = make_envelope(owner, event_id, committed, &payload);
      if (!*envelope)
      {
         result = -1;
         goto finish;
      }
   }
   if (sequence)
      snprintf(sequence, 32, "%s", committed);
   if (event->kind == AIMEE_SESSION_EXECUTION && strcmp(event->execution_status, "running"))
      atomic_store(&owner->closed, 1);
   const char *role =
       event->kind == AIMEE_SESSION_MESSAGE && event->message ? event->message->role : event->role;
   if (role && !strcmp(role, "assistant"))
   {
      owner->counts.model_events++;
      if (event->kind == AIMEE_SESSION_MESSAGE ||
          (event->kind == AIMEE_SESSION_DELTA && event->delta &&
           event->delta->type == AIMEE_DELTA_TURN_STOP))
         owner->counts.model_completions++;
   }
finish:
   if (result)
      server_session_capture_fail(owner);
   cJSON_Delete(payload);
   pthread_mutex_unlock(&owner->lock);
   return result;
}
int server_session_capture_sink(const aimee_session_event_t *event, void *owner)
{
   return server_session_capture_append(owner, event, NULL, NULL);
}
