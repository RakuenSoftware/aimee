#include "server_session_control.h"
#include "vault_principal.h"
#include <aimee/ir/session_event.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define CONTROL_SCOPES_MAX  128
#define CONTROL_PENDING_MAX 128
struct pending_control
{
   char *id, *kind;
   cJSON *request, *reply;
   int taken;
   struct pending_control *next;
};
struct server_control_scope
{
   char session[128], turn[128], principal[VAULT_PRINCIPAL_MAX];
   unsigned count, references;
   int closed;
   struct pending_control *pending;
   struct server_control_scope *next;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
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
   if (!session || !turn || !principal || !session[0] || !turn[0] || !principal[0] ||
       strlen(session) >= 128 || strlen(turn) >= 128 || strlen(principal) >= VAULT_PRINCIPAL_MAX)
      return NULL;
   server_control_scope_t *scope = calloc(1, sizeof *scope);
   if (!scope)
      return NULL;
   snprintf(scope->session, sizeof scope->session, "%s", session);
   snprintf(scope->turn, sizeof scope->turn, "%s", turn);
   snprintf(scope->principal, sizeof scope->principal, "%s", principal);
   pthread_mutex_lock(&lock);
   if (scope_count >= CONTROL_SCOPES_MAX)
   {
      pthread_mutex_unlock(&lock);
      free(scope);
      return NULL;
   }
   for (server_control_scope_t *s = scopes; s; s = s->next)
      if (!strcmp(s->session, session) && !strcmp(s->turn, turn))
      {
         pthread_mutex_unlock(&lock);
         free(scope);
         return NULL;
      }
   scope->references = 1;
   scope->next = scopes;
   scopes = scope;
   scope_count++;
   pthread_mutex_unlock(&lock);
   return scope;
}
/* All scope state and lease accounting share the registry lock. No caller
 * may dereference a borrowed scope after its execution/lease has ended. */
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
   free(scope);
}
int server_control_scope_capture(const char *session, const char *principal,
                                 server_control_scope_t **out)
{
   if (!out)
      return -1;
   *out = NULL;
   pthread_mutex_lock(&lock);
   int result = 0;
   if (current)
   {
      if (current->closed || !session || strcmp(current->session, session) || !principal ||
          strcmp(current->principal, principal))
         result = -1;
      else
      {
         current->references++;
         *out = current;
      }
   }
   pthread_mutex_unlock(&lock);
   return result;
}
void server_control_scope_release(server_control_scope_t *scope)
{
   if (!scope)
      return;
   pthread_mutex_lock(&lock);
   int destroy = --scope->references == 0;
   pthread_mutex_unlock(&lock);
   if (destroy)
      scope_free(scope);
}
void server_control_scope_close(server_control_scope_t *scope)
{
   if (!scope)
      return;
   pthread_mutex_lock(&lock);
   server_control_scope_t **slot = &scopes;
   while (*slot && *slot != scope)
      slot = &(*slot)->next;
   if (*slot)
   {
      *slot = scope->next;
      scope_count--;
   }
   scope->closed = 1;
   if (current == scope)
      current = NULL;
   pthread_mutex_unlock(&lock);
   server_control_scope_release(scope);
}
int server_control_register(server_control_scope_t *scope, const char *id, const char *kind,
                            const cJSON *request)
{
   if (!scope || !id || !id[0] || strlen(id) > 1024 || !kind || !kind[0] ||
       !cJSON_IsObject(request))
      return -1;
   struct pending_control *p = calloc(1, sizeof *p);
   if (!p)
      return -1;
   p->id = strdup(id);
   p->kind = strdup(kind);
   p->request = cJSON_Duplicate(request, 1);
   if (!p->id || !p->kind || !p->request)
   {
      free(p->id);
      free(p->kind);
      cJSON_Delete(p->request);
      free(p);
      return -1;
   }
   pthread_mutex_lock(&lock);
   int valid = !scope->closed && scope->count < CONTROL_PENDING_MAX;
   for (struct pending_control *old = scope->pending; old; old = old->next)
      if (!strcmp(old->id, id))
         valid = 0;
   if (valid)
   {
      p->next = scope->pending;
      scope->pending = p;
      scope->count++;
   }
   pthread_mutex_unlock(&lock);
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
   pthread_mutex_lock(&lock);
   int result = 1;
   for (server_control_scope_t *scope = scopes; scope; scope = scope->next)
   {
      if (strcmp(scope->session, session))
         continue;
      size_t prefix = strlen(scope->turn);
      if (strncmp(qualified, scope->turn, prefix) || qualified[prefix] != ':')
         continue;
      if (strcmp(scope->principal, principal))
      {
         result = -1;
         break;
      }
      for (struct pending_control *p = scope->pending; p; p = p->next)
      {
         if (strcmp(p->id, qualified + prefix + 1))
            continue;
         if (p->reply || p->taken)
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
      break;
   }
   pthread_mutex_unlock(&lock);
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
   pthread_mutex_lock(&lock);
   if (scope->closed)
   {
      pthread_mutex_unlock(&lock);
      return -1;
   }
   for (struct pending_control *p = scope->pending; p; p = p->next)
   {
      if (!p->reply || p->taken || (native_id && strcmp(p->id, native_id)))
         continue;
      char *id = strdup(p->id), *kind = strdup(p->kind);
      cJSON *copy = cJSON_Duplicate(p->request, 1);
      if (!id || !kind || !copy)
      {
         free(id);
         free(kind);
         cJSON_Delete(copy);
         pthread_mutex_unlock(&lock);
         return -1;
      }
      *out_id = id;
      *out_kind = kind;
      *request = copy;
      *reply = p->reply;
      p->reply = NULL;
      p->taken = 1;
      pthread_mutex_unlock(&lock);
      return 1;
   }
   pthread_mutex_unlock(&lock);
   return 0;
}
