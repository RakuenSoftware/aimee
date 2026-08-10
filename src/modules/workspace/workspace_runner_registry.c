/* workspace_runner_registry.c — process-global map of workspace id -> runner
 * queue. See workspace_runner_registry.h. A single mutex guards the slot table;
 * the queues themselves are independently synchronized. */
#include "workspace_runner_registry.h"
#include "cJSON.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   int in_use;
   char id[WS_RUNNER_REGISTRY_IDLEN];
   ws_runner_queue_t *queue;
} registry_slot_t;

static registry_slot_t g_slots[WS_RUNNER_REGISTRY_MAX];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* Caller holds g_mu. */
static registry_slot_t *find_slot(const char *id)
{
   for (int i = 0; i < WS_RUNNER_REGISTRY_MAX; i++)
      if (g_slots[i].in_use && strcmp(g_slots[i].id, id) == 0)
         return &g_slots[i];
   return NULL;
}

ws_runner_queue_t *ws_runner_registry_get_or_create(const char *id)
{
   if (!id || !id[0] || strlen(id) >= WS_RUNNER_REGISTRY_IDLEN)
      return NULL;

   pthread_mutex_lock(&g_mu);
   registry_slot_t *s = find_slot(id);
   if (s)
   {
      ws_runner_queue_t *q = s->queue;
      pthread_mutex_unlock(&g_mu);
      return q;
   }
   for (int i = 0; i < WS_RUNNER_REGISTRY_MAX; i++)
   {
      if (!g_slots[i].in_use)
      {
         ws_runner_queue_t *q = calloc(1, sizeof(*q));
         if (!q)
         {
            pthread_mutex_unlock(&g_mu);
            return NULL;
         }
         ws_runner_queue_init(q);
         g_slots[i].in_use = 1;
         snprintf(g_slots[i].id, sizeof(g_slots[i].id), "%s", id);
         g_slots[i].queue = q;
         pthread_mutex_unlock(&g_mu);
         return q;
      }
   }
   pthread_mutex_unlock(&g_mu);
   return NULL; /* registry full */
}

ws_runner_queue_t *ws_runner_registry_lookup(const char *id)
{
   if (!id || !id[0])
      return NULL;
   pthread_mutex_lock(&g_mu);
   registry_slot_t *s = find_slot(id);
   ws_runner_queue_t *q = s ? s->queue : NULL;
   pthread_mutex_unlock(&g_mu);
   return q;
}

void ws_runner_registry_remove(const char *id)
{
   if (!id || !id[0])
      return;
   pthread_mutex_lock(&g_mu);
   registry_slot_t *s = find_slot(id);
   ws_runner_queue_t *q = NULL;
   if (s)
   {
      q = s->queue;
      s->in_use = 0;
      s->id[0] = '\0';
      s->queue = NULL;
   }
   pthread_mutex_unlock(&g_mu);

   /* Tear the queue down outside the registry lock: close() wakes any blocked
    * transport/poll, then destroy() frees it. */
   if (q)
   {
      ws_runner_queue_close(q);
      ws_runner_queue_destroy(q);
      free(q);
   }
}

cJSON *ws_runner_registry_poll(const char *id, int timeout_ms)
{
   ws_runner_queue_t *q = ws_runner_registry_get_or_create(id);
   if (!q)
      return NULL;
   return ws_runner_queue_poll(q, timeout_ms);
}

int ws_runner_registry_respond(const char *id, cJSON *response)
{
   ws_runner_queue_t *q = ws_runner_registry_lookup(id);
   if (!q)
   {
      cJSON_Delete(response);
      return -1;
   }
   return ws_runner_queue_respond(q, response);
}
