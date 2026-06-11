/* response_dedup.c: bounded, TTL'd short-window response cache (§4).
 * See response_dedup.h. Intentionally small and conservative. */
#include "response_dedup.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEDUP_KEY_MAX 192
#define DEDUP_SLOTS   64

typedef struct
{
   char key[DEDUP_KEY_MAX];
   char *response; /* malloc'd; NULL = empty slot */
   double cost;
   long expires_at;
} dedup_slot_t;

static dedup_slot_t g_slots[DEDUP_SLOTS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* FNV-1a 64-bit over a NUL-terminated string. */
static uint64_t fnv1a(const char *s)
{
   uint64_t h = 1469598103934665603ULL;
   for (; s && *s; s++)
   {
      h ^= (unsigned char)*s;
      h *= 1099511628211ULL;
   }
   return h;
}

void response_dedup_key(const response_dedup_key_inputs_t *in, char *out, size_t out_cap)
{
   if (!out || out_cap == 0)
      return;
   if (!in)
   {
      out[0] = '\0';
      return;
   }
   uint64_t body_hash = fnv1a(in->body ? in->body : "");
   /* The pre-injected context (memory/<aimee-context> envelope) changes the model
    * input even when the request body is identical, so it MUST be in the key — or
    * a repeat body could replay a stale answer after the injected context moved. */
   uint64_t ctx_hash = fnv1a(in->context ? in->context : "");
   uint64_t flags_hash = fnv1a(in->behavior_flags ? in->behavior_flags : "");
   /* Every behaviour-affecting input the handler can see: the auth/source boundary
    * (principal|source), the RESOLVED backend identity (provider|model|endpoint)
    * and stream mode — built after agent/config resolution so two requests that
    * resolve to a different backend never collide — the explicit idempotency key,
    * and hashes of the body, the pre-injected context, and the behaviour-config
    * flags. */
   snprintf(out, out_cap, "%s|%s|%s|%s|%s|s%d|%s|%016llx|%016llx|%016llx",
            in->principal && in->principal[0] ? in->principal : "anon",
            in->source ? in->source : "", in->provider ? in->provider : "",
            in->model ? in->model : "", in->endpoint ? in->endpoint : "", in->stream ? 1 : 0,
            in->idempotency_key ? in->idempotency_key : "", (unsigned long long)body_hash,
            (unsigned long long)ctx_hash, (unsigned long long)flags_hash);
}

int response_dedup_get(const char *key, long now, char **resp_out, double *cost_out)
{
   if (!key || !key[0] || !resp_out)
      return 0;
   int hit = 0;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < DEDUP_SLOTS; i++)
   {
      if (g_slots[i].response && strcmp(g_slots[i].key, key) == 0)
      {
         if (g_slots[i].expires_at > now)
         {
            *resp_out = strdup(g_slots[i].response);
            if (*resp_out)
            {
               if (cost_out)
                  *cost_out = g_slots[i].cost;
               hit = 1;
            }
         }
         else
         {
            /* Expired: reclaim eagerly. */
            free(g_slots[i].response);
            g_slots[i].response = NULL;
            g_slots[i].key[0] = '\0';
         }
         break;
      }
   }
   pthread_mutex_unlock(&g_lock);
   return hit;
}

void response_dedup_put(const char *key, const char *resp, double cost, long now, int ttl_seconds)
{
   if (!key || !key[0] || !resp || !resp[0])
      return;
   if (ttl_seconds <= 0)
      ttl_seconds = RESPONSE_DEDUP_TTL_SECONDS;

   char *copy = strdup(resp);
   if (!copy)
      return;

   pthread_mutex_lock(&g_lock);

   /* Slot selection priority: (1) a slot already holding this key, (2) a free or
    * already-expired slot, (3) the soonest-to-expire live slot as the eviction
    * victim. Computed in one pass. */
   int same = -1, free_or_expired = -1, soonest_live = 0;
   for (int i = 0; i < DEDUP_SLOTS; i++)
   {
      if (g_slots[i].response && strcmp(g_slots[i].key, key) == 0)
      {
         same = i;
         break;
      }
      if (!g_slots[i].response || g_slots[i].expires_at <= now)
      {
         if (free_or_expired < 0)
            free_or_expired = i;
      }
      else if (g_slots[i].expires_at < g_slots[soonest_live].expires_at)
      {
         soonest_live = i;
      }
   }
   int target = same >= 0 ? same : (free_or_expired >= 0 ? free_or_expired : soonest_live);

   free(g_slots[target].response);
   g_slots[target].response = copy;
   g_slots[target].cost = cost;
   g_slots[target].expires_at = now + ttl_seconds;
   snprintf(g_slots[target].key, sizeof(g_slots[target].key), "%s", key);

   pthread_mutex_unlock(&g_lock);
}

void response_dedup_clear(void)
{
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < DEDUP_SLOTS; i++)
   {
      free(g_slots[i].response);
      g_slots[i].response = NULL;
      g_slots[i].key[0] = '\0';
      g_slots[i].cost = 0.0;
      g_slots[i].expires_at = 0;
   }
   pthread_mutex_unlock(&g_lock);
}
