/* kb_reqctx.c: per-request thread-local actor principal. See kb_reqctx.h. */

#include "kb_reqctx.h"

#include <pthread.h>
#include <stdio.h> /* snprintf */
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

static pthread_key_t g_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

typedef struct
{
   char kind[32];
   char id[128];
} kb_reqctx_scope_t;

static pthread_key_t g_scope_key;
static pthread_once_t g_scope_once = PTHREAD_ONCE_INIT;
static pthread_key_t g_resolved_key;
static pthread_once_t g_resolved_once = PTHREAD_ONCE_INIT;

static void free_actor(void *p)
{
   free(p);
}
static void key_init(void)
{
   pthread_key_create(&g_key, free_actor);
}

static void scope_key_init(void)
{
   pthread_key_create(&g_scope_key, free);
}

static void resolved_key_init(void)
{
   pthread_key_create(&g_resolved_key, free);
}

void kb_reqctx_set_actor(const kb_principal_t *actor)
{
   pthread_once(&g_once, key_init);
   kb_principal_t *slot = (kb_principal_t *)pthread_getspecific(g_key);
   if (!slot)
   {
      slot = (kb_principal_t *)malloc(sizeof(*slot));
      if (!slot)
         return;
      pthread_setspecific(g_key, slot);
   }
   if (actor && actor->authenticated)
      *slot = *actor;
   else
      memset(slot, 0, sizeof(*slot));
}

void kb_reqctx_clear(void)
{
   pthread_once(&g_once, key_init);
   kb_principal_t *slot = (kb_principal_t *)pthread_getspecific(g_key);
   if (slot)
      memset(slot, 0, sizeof(*slot));
   pthread_once(&g_scope_once, scope_key_init);
   kb_reqctx_scope_t *scope = (kb_reqctx_scope_t *)pthread_getspecific(g_scope_key);
   if (scope)
      memset(scope, 0, sizeof(*scope));
   pthread_once(&g_resolved_once, resolved_key_init);
   kb_request_context_t *resolved =
       (kb_request_context_t *)pthread_getspecific(g_resolved_key);
   if (resolved)
      memset(resolved, 0, sizeof(*resolved));
}

const kb_principal_t *kb_reqctx_actor(void)
{
   pthread_once(&g_once, key_init);
   kb_principal_t *slot = (kb_principal_t *)pthread_getspecific(g_key);
   if (slot && slot->authenticated)
      return slot;
   return NULL;
}

void kb_reqctx_set_resolved(const kb_request_context_t *resolved)
{
   pthread_once(&g_resolved_once, resolved_key_init);
   kb_request_context_t *slot =
       (kb_request_context_t *)pthread_getspecific(g_resolved_key);
   if (!slot)
   {
      slot = (kb_request_context_t *)calloc(1, sizeof(*slot));
      if (!slot)
         return;
      pthread_setspecific(g_resolved_key, slot);
   }
   if (resolved)
      *slot = *resolved;
   else
      memset(slot, 0, sizeof(*slot));
}

const kb_request_context_t *kb_reqctx_resolved(void)
{
   pthread_once(&g_resolved_once, resolved_key_init);
   const kb_request_context_t *slot =
       (const kb_request_context_t *)pthread_getspecific(g_resolved_key);
   return slot && (slot->has_transport || slot->has_actor) ? slot : NULL;
}

void kb_reqctx_set_verified_scope(const char *kind, const char *id)
{
   pthread_once(&g_scope_once, scope_key_init);
   kb_reqctx_scope_t *scope = (kb_reqctx_scope_t *)pthread_getspecific(g_scope_key);
   if (!scope)
   {
      scope = (kb_reqctx_scope_t *)calloc(1, sizeof(*scope));
      if (!scope)
         return;
      pthread_setspecific(g_scope_key, scope);
   }
   snprintf(scope->kind, sizeof(scope->kind), "%s", kind ? kind : "");
   snprintf(scope->id, sizeof(scope->id), "%s", id ? id : "");
}

int kb_reqctx_verified_scope(const char **kind, const char **id)
{
   pthread_once(&g_scope_once, scope_key_init);
   const kb_reqctx_scope_t *scope = (const kb_reqctx_scope_t *)pthread_getspecific(g_scope_key);
   if (!scope || !scope->kind[0] || !scope->id[0])
      return 0;
   if (kind)
      *kind = scope->kind;
   if (id)
      *id = scope->id;
   return 1;
}

/* Its own thread-local, not a field on the actor slot: the content type is known
 * before authentication and is read by the PRE-AUTH bootstrap routes, so tying
 * its lifetime to the actor (which kb_reqctx_clear wipes mid-request) would make
 * it empty exactly where it is needed. */
#define KB_REQCTX_CT_MAX 128
static pthread_key_t g_ct_key;
static pthread_once_t g_ct_once = PTHREAD_ONCE_INIT;

static void ct_key_init(void)
{
   pthread_key_create(&g_ct_key, free);
}

void kb_reqctx_set_content_type(const char *value)
{
   pthread_once(&g_ct_once, ct_key_init);
   char *slot = (char *)pthread_getspecific(g_ct_key);
   if (!slot)
   {
      slot = (char *)malloc(KB_REQCTX_CT_MAX);
      if (!slot)
         return;
      pthread_setspecific(g_ct_key, slot);
   }
   snprintf(slot, KB_REQCTX_CT_MAX, "%s", value ? value : "");
}

void kb_reqctx_clear_content_type(void)
{
   pthread_once(&g_ct_once, ct_key_init);
   char *slot = (char *)pthread_getspecific(g_ct_key);
   if (slot)
      slot[0] = '\0';
}

const char *kb_reqctx_content_type(void)
{
   pthread_once(&g_ct_once, ct_key_init);
   const char *slot = (const char *)pthread_getspecific(g_ct_key);
   return slot ? slot : "";
}

int kb_reqctx_content_type_is_json(void)
{
   const char *ct = kb_reqctx_content_type();
   while (*ct == ' ' || *ct == '\t')
      ct++;
   if (strncasecmp(ct, "application/json", 16) != 0)
      return 0;
   /* Only a parameter may follow the media type: "application/jsonx" is a
    * different type and must not pass as JSON. */
   const char *rest = ct + 16;
   while (*rest == ' ' || *rest == '\t')
      rest++;
   return *rest == '\0' || *rest == ';';
}
