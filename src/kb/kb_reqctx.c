/* kb_reqctx.c: per-request thread-local actor principal. See kb_reqctx.h. */

#include "kb_reqctx.h"

#include <pthread.h>
#include <stdio.h> /* snprintf */
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

static pthread_key_t g_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void free_actor(void *p)
{
   free(p);
}
static void key_init(void)
{
   pthread_key_create(&g_key, free_actor);
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
}

const kb_principal_t *kb_reqctx_actor(void)
{
   pthread_once(&g_once, key_init);
   kb_principal_t *slot = (kb_principal_t *)pthread_getspecific(g_key);
   if (slot && slot->authenticated)
      return slot;
   return NULL;
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
