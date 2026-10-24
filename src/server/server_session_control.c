#include "server_session_control.h"
#include "vault_principal.h"
#include <aimee/ir/session_event.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#define CONTROL_SCOPES_MAX  128
#define CONTROL_PENDING_MAX 128
struct pending_control
{
   char *id, *kind;
   cJSON *request, *reply;
   int taken, reserved, host_consumed;
   server_control_record_fn record;
   struct pending_control *next;
};
struct server_control_scope
{
   char session[128], execution_session[128], turn[128], principal[VAULT_PRINCIPAL_MAX];
   unsigned count, references;
   atomic_int closed, failed, unknown_effect_denied, replies_disabled;
   pthread_mutex_t mutex;
   struct pending_control *pending;
   struct server_control_scope *next;
};
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static server_control_scope_t *scopes;
static unsigned scope_count;
static _Thread_local server_control_scope_t *current;
server_control_scope_t *server_control_scope_current(void)
{
   return current;
}
server_control_scope_t *server_control_scope_bind(server_control_scope_t *scope)
{
   server_control_scope_t *old = current;
   current = scope;
   return old;
}
server_control_scope_t *server_control_scope_open(const char *session, const char *turn,
                                                  const char *principal)
{
   return server_control_scope_open_execution(session, turn, principal, session);
}
server_control_scope_t *server_control_scope_open_execution(const char *session, const char *turn,
                                                            const char *principal,
                                                            const char *execution_session)
{
   if (!session || !turn || !principal || !session[0] || !turn[0] || !principal[0] ||
       strlen(session) >= 128 || strlen(turn) >= 128 || strlen(principal) >= VAULT_PRINCIPAL_MAX ||
       !execution_session || !execution_session[0] || strlen(execution_session) >= 128)
      return NULL;
   server_control_scope_t *scope = calloc(1, sizeof *scope);
   if (!scope)
      return NULL;
   if (pthread_mutex_init(&scope->mutex, NULL))
   {
      free(scope);
      return NULL;
   }
   atomic_init(&scope->closed, 0);
   atomic_init(&scope->failed, 0);
   atomic_init(&scope->unknown_effect_denied, 0);
   atomic_init(&scope->replies_disabled, 0);
   snprintf(scope->session, sizeof scope->session, "%s", session);
   snprintf(scope->execution_session, sizeof scope->execution_session, "%s", execution_session);
   snprintf(scope->turn, sizeof scope->turn, "%s", turn);
   snprintf(scope->principal, sizeof scope->principal, "%s", principal);
   pthread_mutex_lock(&registry_lock);
   if (scope_count >= CONTROL_SCOPES_MAX)
   {
      pthread_mutex_unlock(&registry_lock);
      pthread_mutex_destroy(&scope->mutex);
      free(scope);
      return NULL;
   }
   for (server_control_scope_t *s = scopes; s; s = s->next)
      if (!strcmp(s->session, session) && !strcmp(s->turn, turn))
      {
         pthread_mutex_unlock(&registry_lock);
         pthread_mutex_destroy(&scope->mutex);
         free(scope);
         return NULL;
      }
   scope->references = 1;
   scope->next = scopes;
   scopes = scope;
   scope_count++;
   pthread_mutex_unlock(&registry_lock);
   return scope;
}
/* The registry protects membership and leases. Pending controls and durable
 * callbacks use only the owning scope lock, so storage cannot stall other turns. */
static void scope_free(server_control_scope_t *scope)
{
   struct pending_control *p = scope->pending;
   while (p)
   {
      struct pending_control *next = p->next;
      free(p->id);
      free(p->kind);
      cJSON_Delete(p->request);
      cJSON_Delete(p->reply);
      free(p);
      p = next;
   }
   pthread_mutex_destroy(&scope->mutex);
   free(scope);
}
int server_control_scope_failed(const server_control_scope_t *scope)
{
   return scope && atomic_load(&scope->failed);
}
int server_control_unknown_effect_denied(const server_control_scope_t *scope)
{
   return !scope || atomic_load(&scope->unknown_effect_denied);
}
void server_control_deny_unknown_effects(server_control_scope_t *scope)
{
   if (scope)
      atomic_store(&scope->unknown_effect_denied, 1);
}
int server_control_scope_capture(const char *session, const char *principal,
                                 server_control_scope_t **out)
{
   if (!out)
      return -1;
   *out = NULL;
   pthread_mutex_lock(&registry_lock);
   int result = 0;
   if (current)
   {
      if (atomic_load(&current->closed) || atomic_load(&current->failed) || !session ||
          (strcmp(current->session, session) && strcmp(current->execution_session, session)) ||
          !principal || strcmp(current->principal, principal))
         result = -1;
      else
      {
         current->references++;
         *out = current;
      }
   }
   pthread_mutex_unlock(&registry_lock);
   return result;
}
void server_control_scope_release(server_control_scope_t *scope)
{
   if (!scope)
      return;
   pthread_mutex_lock(&registry_lock);
   int destroy = --scope->references == 0;
   pthread_mutex_unlock(&registry_lock);
   if (destroy)
      scope_free(scope);
}
void server_control_scope_close(server_control_scope_t *scope)
{
   if (!scope)
      return;
   pthread_mutex_lock(&scope->mutex);
   atomic_store(&scope->closed, 1);
   pthread_mutex_unlock(&scope->mutex);
   pthread_mutex_lock(&registry_lock);
   server_control_scope_t **slot = &scopes;
   while (*slot && *slot != scope)
      slot = &(*slot)->next;
   if (*slot)
   {
      *slot = scope->next;
      scope_count--;
   }
   if (current == scope)
      current = NULL;
   pthread_mutex_unlock(&registry_lock);
   server_control_scope_release(scope);
}
static int record_control(server_control_scope_t *scope, struct pending_control *p,
                          aimee_session_event_kind_t kind, const cJSON *payload)
{
   if (!p->record)
      return 0;
   char qualified[128 + 1024 + 2];
   snprintf(qualified, sizeof qualified, "%s:%s", scope->turn, p->id);
   aimee_session_event_t event = {
       .kind = kind, .request_id = qualified, .control_kind = p->kind, .control = payload};
   int result = p->record(scope->session, scope->turn, scope->principal, &event);
   if (result)
      atomic_store(&scope->failed, 1);
   return result;
}
void server_control_scope_disable_replies(server_control_scope_t *scope)
{
   if (!scope)
      return;
   pthread_mutex_lock(&scope->mutex);
   atomic_store(&scope->replies_disabled, 1);
   pthread_mutex_unlock(&scope->mutex);
}
int server_control_scope_can_reply(const server_control_scope_t *scope)
{
   return scope && !atomic_load(&scope->closed) && !atomic_load(&scope->failed) &&
          !atomic_load(&scope->replies_disabled);
}
int server_control_register(server_control_scope_t *scope, const char *id, const char *kind,
                            const cJSON *request)
{
   return server_control_register_recorded(scope, id, kind, request, NULL);
}
static int register_control(server_control_scope_t *scope, const char *id, const char *kind,
                            const cJSON *request, server_control_record_fn record, int reserved)
{
   if (!server_control_scope_can_reply(scope) || !id || !id[0] || strlen(id) > 1024 || !kind || !kind[0] ||
       !cJSON_IsObject(request) || !aimee_ir_json_unique(request))
      return -1;
   struct pending_control *p = calloc(1, sizeof *p);
   if (!p)
      return -1;
   p->id = strdup(id);
   p->kind = strdup(kind);
   p->request = cJSON_Duplicate(request, 1);
   p->reserved = p->host_consumed = reserved;
   if (!p->id || !p->kind || !p->request)
   {
      free(p->id);
      free(p->kind);
      cJSON_Delete(p->request);
      free(p);
      return -1;
   }
   pthread_mutex_lock(&scope->mutex);
   int valid = server_control_scope_can_reply(scope);
   for (struct pending_control *old = scope->pending; old; old = old->next)
      if (!strcmp(old->id, id))
      {
         if (valid && !reserved && !record && old->reserved && !old->taken &&
             !strcmp(old->kind, kind) && cJSON_Compare(old->request, request, 1))
         {
            old->reserved = 0;
            pthread_mutex_unlock(&scope->mutex);
            free(p->id);
            free(p->kind);
            cJSON_Delete(p->request);
            free(p);
            return 0;
         }
         valid = 0;
      }
   valid = valid && scope->count < CONTROL_PENDING_MAX;
   p->record = record;
   if (valid && record_control(scope, p, AIMEE_SESSION_CONTROL_REQUEST, p->request))
      valid = 0;
   if (valid)
   {
      p->next = scope->pending;
      scope->pending = p;
      scope->count++;
   }
   pthread_mutex_unlock(&scope->mutex);
   if (!valid)
   {
      free(p->id);
      free(p->kind);
      cJSON_Delete(p->request);
      free(p);
      return -1;
   }
   return 0;
}
int server_control_register_recorded(server_control_scope_t *scope, const char *id,
                                     const char *kind, const cJSON *request,
                                     server_control_record_fn record)
{
   return register_control(scope, id, kind, request, record, 0);
}
int server_control_reserve(server_control_scope_t *scope, const char *id, const char *kind,
                           const cJSON *request)
{
   return register_control(scope, id, kind, request, NULL, 1);
}
void server_control_scope_fail(server_control_scope_t *scope)
{
   if (scope)
      atomic_store(&scope->failed, 1);
}
static int valid_reply(const cJSON *reply)
{
   if (!cJSON_IsObject(reply) || !aimee_ir_json_unique(reply))
      return 0;
   const char *decision = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "decision"));
   if (!decision ||
       (strcmp(decision, "allow") && strcmp(decision, "deny") && strcmp(decision, "cancel")))
      return 0;
   const cJSON *field;
   cJSON_ArrayForEach(field, reply)
   {
      if (!strcmp(field->string, "decision"))
         continue;
      if (!strcmp(field->string, "content") && cJSON_IsObject(field))
         continue;
      if (strcmp(field->string, "answers") || !cJSON_IsObject(field))
         return 0;
      const cJSON *answers;
      cJSON_ArrayForEach(answers, field)
      {
         if (!answers->string || !answers->string[0] || !cJSON_IsArray(answers) ||
             cJSON_GetArraySize(answers) > 32)
            return 0;
         const cJSON *answer;
         cJSON_ArrayForEach(answer, answers) if (!cJSON_IsString(answer)) return 0;
      }
   }
   char *json = cJSON_PrintUnformatted(reply);
   int valid = json && strlen(json) <= 65536;
   free(json);
   return valid;
}
int server_control_reply(const char *session, const char *qualified, const char *principal,
                         const cJSON *reply)
{
   if (!session || !qualified || !principal || !principal[0] || !valid_reply(reply))
      return -1;
   pthread_mutex_lock(&registry_lock);
   server_control_scope_t *selected = NULL;
   size_t prefix = 0;
   int result = 1;
   for (server_control_scope_t *scope = scopes; scope; scope = scope->next)
   {
      if (strcmp(scope->session, session))
         continue;
      prefix = strlen(scope->turn);
      if (strncmp(qualified, scope->turn, prefix) || qualified[prefix] != ':')
         continue;
      if (strcmp(scope->principal, principal))
         result = -1;
      else
      {
         selected = scope;
         selected->references++;
      }
      break;
   }
   pthread_mutex_unlock(&registry_lock);
   if (!selected)
      return result;
   pthread_mutex_lock(&selected->mutex);
   if (atomic_load(&selected->failed))
      result = -1;
   else if (!atomic_load(&selected->closed))
      for (struct pending_control *p = selected->pending; p; p = p->next)
      {
         if (strcmp(p->id, qualified + prefix + 1))
            continue;
         if (p->reply || p->taken || p->reserved)
            break;
         if (!aimee_ir_control_reply_valid(p->kind, p->request, reply))
         {
            result = -1;
            break;
         }
         p->reply = cJSON_Duplicate(reply, 1);
         result = p->reply ? 0 : -1;
         break;
      }
   pthread_mutex_unlock(&selected->mutex);
   server_control_scope_release(selected);
   return result;
}
int server_control_take(server_control_scope_t *scope, const char *native_id, char **out_id,
                        char **out_kind, cJSON **request, cJSON **reply)
{
   if (!scope || !out_id || !out_kind || !request || !reply)
      return -1;
   *out_id = NULL;
   *out_kind = NULL;
   *request = NULL;
   *reply = NULL;
   pthread_mutex_lock(&scope->mutex);
   if (atomic_load(&scope->closed) || atomic_load(&scope->failed))
   {
      pthread_mutex_unlock(&scope->mutex);
      return -1;
   }
   for (struct pending_control *p = scope->pending; p; p = p->next)
   {
      if (!p->reply || p->taken || p->reserved || (!native_id && (p->record || p->host_consumed)) ||
          (native_id && strcmp(p->id, native_id)))
         continue;
      char *id = strdup(p->id), *kind = strdup(p->kind);
      cJSON *copy = cJSON_Duplicate(p->request, 1);
      if (!id || !kind || !copy)
      {
         free(id);
         free(kind);
         cJSON_Delete(copy);
         pthread_mutex_unlock(&scope->mutex);
         return -1;
      }
      if (record_control(scope, p, AIMEE_SESSION_CONTROL_RESPONSE, p->reply))
      {
         free(id);
         free(kind);
         cJSON_Delete(copy);
         pthread_mutex_unlock(&scope->mutex);
         return -1;
      }
      *out_id = id;
      *out_kind = kind;
      *request = copy;
      *reply = p->reply;
      p->reply = NULL;
      p->taken = 1;
      pthread_mutex_unlock(&scope->mutex);
      return 1;
   }
   pthread_mutex_unlock(&scope->mutex);
   return 0;
}
int server_control_abandon(server_control_scope_t *scope, const char *id)
{
   if (!scope || !id)
      return -1;
   pthread_mutex_lock(&scope->mutex);
   int result = 1;
   if (atomic_load(&scope->closed) || atomic_load(&scope->failed))
      result = -1;
   else
      for (struct pending_control *p = scope->pending; p; p = p->next)
      {
         if (strcmp(p->id, id) || p->taken)
            continue;
         cJSON *reply = cJSON_CreateObject();
         if (!reply || !cJSON_AddStringToObject(reply, "decision", "cancel"))
         {
            cJSON_Delete(reply);
            result = -1;
            atomic_store(&scope->failed, 1);
            break;
         }
         result = record_control(scope, p, AIMEE_SESSION_CONTROL_RESPONSE, reply);
         cJSON_Delete(reply);
         if (!result)
         {
            p->taken = 1;
            cJSON_Delete(p->reply);
            p->reply = NULL;
         }
         break;
      }
   pthread_mutex_unlock(&scope->mutex);
   return result;
}
