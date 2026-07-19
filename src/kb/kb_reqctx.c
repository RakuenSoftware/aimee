/* kb_reqctx.c: per-request thread-local actor principal. See kb_reqctx.h. */

#include "kb_reqctx.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

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
