#include "server_session_capture.h"
#include "server_session_control.h"
#include "server_learning_outcome.h"
#include "db1_client/primary_sessions.h"
#include "vault_principal.h"
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   unsigned long long sequence;
   int block_index;
} source_claim_t;
#define TERMINAL_OBSERVERS_MAX 8
typedef struct
{
   server_session_terminal_action_fn action;
   server_session_verification_prepare_fn prepare;
   void *resource;
} terminal_observer_t;
struct server_session_capture
{
   char session[128], turn[128], principal[VAULT_PRINCIPAL_MAX], execution_session[128];
   pthread_mutex_t lock;
   atomic_uint references;
   atomic_int closed, failed;
   server_control_scope_t *controls;
   server_session_capture_counts_t counts;
   char committed_sequence[32];
   server_session_terminal_action_fn terminal_action;
   void *terminal_resource;
   terminal_observer_t terminal_observers[TERMINAL_OBSERVERS_MAX];
   int n_terminal_observers;
   unsigned long long model_generation, model_active;
   source_claim_t *source_claims;
   int n_claims, claims_capacity;
   struct server_session_capture *next;
};
static _Thread_local server_session_capture_t *current;
static pthread_mutex_t owners_lock = PTHREAD_MUTEX_INITIALIZER;
static server_session_capture_t *owners;
server_session_capture_t *server_session_capture_current(void)
{
   return current;
}
const char *server_session_capture_session(const server_session_capture_t *owner)
{
   return owner ? owner->session : NULL;
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
   pthread_mutex_lock(&owners_lock);
   for (server_session_capture_t *existing = owners; existing; existing = existing->next)
      if (!strcmp(existing->session, owner->session) && !strcmp(existing->turn, owner->turn))
      {
         pthread_mutex_unlock(&owners_lock);
         server_session_capture_release(owner);
         return NULL;
      }
   owner->next = owners;
   owners = owner;
   pthread_mutex_unlock(&owners_lock);
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
const char *server_session_capture_turn(const server_session_capture_t *owner)
{
   return owner ? owner->turn : NULL;
}
int server_session_capture_after_terminal(server_session_capture_t *owner,
                                          server_session_terminal_action_fn action, void *resource)
{
   if (!owner || !action)
      return -1;
   pthread_mutex_lock(&owner->lock);
   int refused = !server_session_capture_accepting(owner) || owner->terminal_action != NULL;
   for (int i = 0; !refused && i < owner->n_terminal_observers; i++)
      refused = resource && resource == owner->terminal_observers[i].resource;
   if (!refused)
   {
      owner->terminal_action = action;
      owner->terminal_resource = resource;
   }
   pthread_mutex_unlock(&owner->lock);
   return refused ? -1 : 0;
}
int server_session_capture_observe_terminal(server_session_capture_t *owner,
                                            server_session_terminal_action_fn action,
                                            void *resource)
{
   return server_session_capture_observe_verification(owner, NULL, action, resource);
}
int server_session_capture_observe_verification(server_session_capture_t *owner,
                                                server_session_verification_prepare_fn prepare,
                                                server_session_terminal_action_fn action,
                                                void *resource)
{
   if (!owner || !action)
      return -1;
   pthread_mutex_lock(&owner->lock);
   int refused = !server_session_capture_accepting(owner) ||
                 owner->n_terminal_observers >= TERMINAL_OBSERVERS_MAX ||
                 (resource && resource == owner->terminal_resource);
   for (int i = 0; !refused && i < owner->n_terminal_observers; i++)
      refused = resource ? resource == owner->terminal_observers[i].resource
                         : !owner->terminal_observers[i].resource &&
                               action == owner->terminal_observers[i].action;
   if (!refused)
      owner->terminal_observers[owner->n_terminal_observers++] =
          (terminal_observer_t){.action = action, .prepare = prepare, .resource = resource};
   pthread_mutex_unlock(&owner->lock);
   return refused ? -1 : 0;
}
/* Caller holds the writer lock. Callbacks run only after that lock is released. */
static int terminal_observers_take(server_session_capture_t *owner,
                                   terminal_observer_t out[TERMINAL_OBSERVERS_MAX])
{
   int count = owner->n_terminal_observers;
   memcpy(out, owner->terminal_observers, (size_t)count * sizeof *out);
   memset(owner->terminal_observers, 0, sizeof owner->terminal_observers);
   owner->n_terminal_observers = 0;
   return count;
}
static void terminal_observers_finish(server_session_capture_t *owner,
                                      terminal_observer_t *observers, int count, int success)
{
   for (int i = 0; i < count; i++)
      observers[i].action(success && !server_session_capture_failed(owner), observers[i].resource);
}
void server_session_capture_release(server_session_capture_t *owner)
{
   if (owner && atomic_fetch_sub(&owner->references, 1) == 1)
   {
      pthread_mutex_lock(&owners_lock);
      server_session_capture_t **entry = &owners;
      while (*entry && *entry != owner)
         entry = &(*entry)->next;
      if (*entry)
         *entry = owner->next;
      pthread_mutex_unlock(&owners_lock);
      if (owner->terminal_action)
         owner->terminal_action(0, owner->terminal_resource);
      terminal_observers_finish(owner, owner->terminal_observers, owner->n_terminal_observers, 0);
      server_control_scope_release(owner->controls);
      pthread_mutex_destroy(&owner->lock);
      free(owner->source_claims);
      free(owner);
   }
}
void server_session_capture_close(server_session_capture_t *owner)
{
   if (!owner)
      return;
   pthread_mutex_lock(&owner->lock);
   atomic_store(&owner->closed, 1);
   server_session_terminal_action_fn action = owner->terminal_action;
   void *resource = owner->terminal_resource;
   owner->terminal_action = NULL;
   owner->terminal_resource = NULL;
   terminal_observer_t observers[TERMINAL_OBSERVERS_MAX];
   int count = terminal_observers_take(owner, observers);
   pthread_mutex_unlock(&owner->lock);
   if (action)
      action(0, resource);
   terminal_observers_finish(owner, observers, count, 0);
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
int server_session_capture_cursor(server_session_capture_t *owner, char sequence[32])
{
   if (!sequence)
      return -1;
   sequence[0] = 0;
   if (!owner)
      return -1;
   pthread_mutex_lock(&owner->lock);
   snprintf(sequence, 32, "%s", owner->committed_sequence[0] ? owner->committed_sequence : "0");
   pthread_mutex_unlock(&owner->lock);
   return 0;
}
int server_session_capture_model_enter(server_session_capture_t *owner, unsigned long long *lease)
{
   if (!lease)
      return -1;
   *lease = 0;
   if (!owner)
      return -1;
   pthread_mutex_lock(&owner->lock);
   int result = -1;
   if (server_session_capture_accepting(owner) && !owner->model_active &&
       owner->model_generation != ULLONG_MAX)
   {
      owner->model_active = ++owner->model_generation;
      *lease = owner->model_active;
      atomic_fetch_add(&owner->references, 1);
      result = 0;
   }
   pthread_mutex_unlock(&owner->lock);
   return result;
}
int server_session_capture_model_leave(server_session_capture_t *owner, unsigned long long lease)
{
   if (!owner || !lease)
      return -1;
   pthread_mutex_lock(&owner->lock);
   int matched = owner->model_active == lease;
   if (matched)
      owner->model_active = 0;
   pthread_mutex_unlock(&owner->lock);
   if (matched)
      server_session_capture_release(owner);
   return matched ? 0 : -1;
}
int server_session_capture_claim_call(server_session_capture_t *owner, const char *source_sequence,
                                      int block_index)
{
   if (!owner || !source_sequence || !source_sequence[0] || block_index < 0 ||
       block_index >= AIMEE_IR_MAX_BLOCKS)
      return -1;
   char *end, canonical[32];
   errno = 0;
   unsigned long long sequence = strtoull(source_sequence, &end, 10);
   int invalid = errno || *end || !sequence;
   snprintf(canonical, sizeof canonical, "%llu", sequence);
   if (invalid || strcmp(canonical, source_sequence))
      return -1;
   pthread_mutex_lock(&owner->lock);
   int result = -1;
   unsigned long long committed = strtoull(owner->committed_sequence, NULL, 10);
   if (!server_session_capture_accepting(owner) || sequence > committed)
      goto done;
   for (int i = 0; i < owner->n_claims; i++)
      if (owner->source_claims[i].sequence == sequence &&
          owner->source_claims[i].block_index == block_index)
      {
         result = 1;
         goto done;
      }
   if (owner->n_claims >= AIMEE_IR_MAX_BLOCKS)
      goto done;
   if (owner->n_claims == owner->claims_capacity)
   {
      int capacity = owner->claims_capacity ? owner->claims_capacity * 2 : 16;
      source_claim_t *claims = realloc(owner->source_claims, (size_t)capacity * sizeof *claims);
      if (!claims)
         goto done;
      owner->source_claims = claims;
      owner->claims_capacity = capacity;
   }
   owner->source_claims[owner->n_claims++] = (source_claim_t){sequence, block_index};
   result = 0;
done:
   pthread_mutex_unlock(&owner->lock);
   return result;
}
int server_session_capture_history(server_session_capture_t *owner, cJSON **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!owner)
      return -1;
   pthread_mutex_lock(&owner->lock);
   if (!server_session_capture_accepting(owner))
   {
      pthread_mutex_unlock(&owner->lock);
      return -1;
   }
   char *wire = NULL;
   int status =
       db1_session_ir_running_history_load(owner->session, owner->principal, owner->turn, &wire);
   cJSON *history = !status && wire ? aimee_ir_json_parse_frame(wire, strlen(wire)) : NULL;
   free(wire);
   int valid = cJSON_IsArray(history) && cJSON_GetArraySize(history) <= AIMEE_IR_MAX_BLOCKS;
   const cJSON *item;
   cJSON_ArrayForEach(item, history)
   {
      aimee_message_t message = {0};
      int parsed = aimee_ir_message_parse(item, &message);
      aimee_request_t admitted = {.messages = &message, .n_messages = 1};
      valid = valid && !parsed && !aimee_ir_request_admit(&admitted);
      aimee_message_free(&message);
      if (!valid)
         break;
   }
   if (valid)
      *out = history;
   else
   {
      cJSON_Delete(history);
      server_session_capture_fail(owner);
   }
   pthread_mutex_unlock(&owner->lock);
   return valid ? 0 : -1;
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
   server_session_terminal_action_fn action = NULL;
   void *resource = NULL;
   terminal_observer_t observers[TERMINAL_OBSERVERS_MAX];
   int n_observers = 0;
   int terminal_success = 0;
   pthread_mutex_lock(&owner->lock);
   int failure_terminal = event->kind == AIMEE_SESSION_EXECUTION && event->execution_status &&
                          (!strcmp(event->execution_status, "failed") ||
                           !strcmp(event->execution_status, "cancelled"));
   if (atomic_load(&owner->closed) || (server_session_capture_failed(owner) && !failure_terminal))
   {
      pthread_mutex_unlock(&owner->lock);
      return -1;
   }
   cJSON *proofs = NULL, *payload = NULL;
   int result = -1;
   /* Only the retained verification owner may supply privileged metadata.
    * Arbitrary producers cannot turn a JSON field into a grading receipt. */
   if (event->verification_proofs)
      goto finish;
   if (event->kind == AIMEE_SESSION_EXECUTION && event->execution_status &&
       !strcmp(event->execution_status, "completed") && !server_session_capture_failed(owner))
   {
      proofs = cJSON_CreateArray();
      for (int i = 0; proofs && i < owner->n_terminal_observers; ++i)
      {
         terminal_observer_t *observer = &owner->terminal_observers[i];
         const cJSON *proof = observer->prepare ? observer->prepare(observer->resource) : NULL;
         if (!proof || !aimee_ir_verification_proof_valid(proof))
            continue;
         const char *session =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(proof, "session_id"));
         const char *turn =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(proof, "turn_id"));
         const char *actor = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(proof, "actor"));
         if (strcmp(session, owner->session) || strcmp(turn, owner->turn) ||
             strcmp(actor, owner->principal))
            continue;
         cJSON *copy = cJSON_Duplicate(proof, 1);
         if (!copy || !cJSON_AddItemToArray(proofs, copy))
         {
            cJSON_Delete(copy);
            goto finish;
         }
      }
   }
   aimee_session_event_t admitted = *event;
   admitted.verification_proofs = cJSON_GetArraySize(proofs) ? proofs : NULL;
   payload = aimee_ir_session_event_json(&admitted);
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
   if (wire)
      result = admitted.verification_proofs
                   ? db1_session_ir_append_verified(owner->session, event_id, owner->turn,
                                                    owner->principal, wire, committed)
                   : db1_session_ir_append(owner->session, event_id, owner->turn, owner->principal,
                                           wire, committed);
   free(wire);
   if (result)
      goto finish;
   snprintf(owner->committed_sequence, sizeof owner->committed_sequence, "%s", committed);
   /* Seal on the durable write, even if a later subscriber-envelope allocation
    * fails. A second terminal must never follow an already committed terminal. */
   if (event->kind == AIMEE_SESSION_EXECUTION && strcmp(event->execution_status, "running"))
   {
      atomic_store(&owner->closed, 1);
      action = owner->terminal_action;
      resource = owner->terminal_resource;
      owner->terminal_action = NULL;
      owner->terminal_resource = NULL;
      n_observers = terminal_observers_take(owner, observers);
      terminal_success = !strcmp(event->execution_status, "completed");
   }
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
   cJSON_Delete(proofs);
   cJSON_Delete(payload);
   pthread_mutex_unlock(&owner->lock);
   int resource_success = !result && terminal_success && !server_session_capture_failed(owner);
   if (action)
      action(resource_success, resource);
   terminal_observers_finish(owner, observers, n_observers, resource_success);
   return result;
}
int server_session_capture_sink(const aimee_session_event_t *event, void *owner)
{
   return server_session_capture_append(owner, event, NULL, NULL);
}
