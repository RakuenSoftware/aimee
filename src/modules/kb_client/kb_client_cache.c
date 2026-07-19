/* kb_client_cache.c: short-TTL LRU for aimee-kb read results. See header. */
#include "kb_client_cache.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KB_CACHE_MAX     256
#define KB_CACHE_KEY_MAX 512

typedef struct
{
   char key[KB_CACHE_KEY_MAX];
   char *value;   /* malloc'd JSON */
   time_t stored; /* insertion time for TTL */
   unsigned seq;  /* recency (higher = newer) for LRU eviction */
} kb_cache_entry_t;

static kb_cache_entry_t g_entries[KB_CACHE_MAX];
static int g_count = 0;
static unsigned g_seq = 0;
static int g_ttl_s = 0;
static long g_hits = 0, g_misses = 0, g_invalidations = 0;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

void kb_cache_configure(int ttl_s)
{
   if (ttl_s < 0)
   {
      const char *env = getenv("AIMEE_KB_CACHE_TTL_S");
      ttl_s = (env && env[0]) ? atoi(env) : 0;
   }
   pthread_mutex_lock(&g_mtx);
   g_ttl_s = ttl_s > 0 ? ttl_s : 0;
   pthread_mutex_unlock(&g_mtx);
   if (g_ttl_s > 0)
      aimee_log(LOG_INFO, "kbcache", "result cache enabled (ttl=%ds, cap=%d)", g_ttl_s,
                KB_CACHE_MAX);
}

int kb_cache_enabled(void)
{
   return g_ttl_s > 0;
}

static int find_locked(const char *key)
{
   for (int i = 0; i < g_count; i++)
      if (strcmp(g_entries[i].key, key) == 0)
         return i;
   return -1;
}

char *kb_cache_get(const char *key)
{
   if (g_ttl_s <= 0 || !key || !key[0] || strlen(key) >= KB_CACHE_KEY_MAX)
      return NULL;
   char *out = NULL;
   pthread_mutex_lock(&g_mtx);
   int i = find_locked(key);
   if (i >= 0)
   {
      if (time(NULL) - g_entries[i].stored <= g_ttl_s)
      {
         out = g_entries[i].value ? strdup(g_entries[i].value) : NULL;
         g_entries[i].seq = ++g_seq; /* touch: most-recently-used */
      }
      else
      {
         /* expired: drop it */
         free(g_entries[i].value);
         g_entries[i] = g_entries[--g_count];
      }
   }
   if (out)
      g_hits++;
   else
      g_misses++;
   pthread_mutex_unlock(&g_mtx);
   return out;
}

void kb_cache_put(const char *key, const char *value)
{
   if (g_ttl_s <= 0 || !key || !key[0] || !value || strlen(key) >= KB_CACHE_KEY_MAX)
      return;
   char *copy = strdup(value);
   if (!copy)
      return;
   pthread_mutex_lock(&g_mtx);
   int i = find_locked(key);
   if (i < 0)
   {
      if (g_count >= KB_CACHE_MAX)
      {
         /* evict least-recently-used */
         int lru = 0;
         for (int j = 1; j < g_count; j++)
            if (g_entries[j].seq < g_entries[lru].seq)
               lru = j;
         free(g_entries[lru].value);
         g_entries[lru] = g_entries[--g_count];
      }
      i = g_count++;
      snprintf(g_entries[i].key, sizeof(g_entries[i].key), "%s", key);
      g_entries[i].value = NULL;
   }
   free(g_entries[i].value);
   g_entries[i].value = copy;
   g_entries[i].stored = time(NULL);
   g_entries[i].seq = ++g_seq;
   pthread_mutex_unlock(&g_mtx);
}

void kb_cache_invalidate_all(void)
{
   pthread_mutex_lock(&g_mtx);
   int dropped = g_count;
   for (int i = 0; i < g_count; i++)
   {
      free(g_entries[i].value);
      g_entries[i].value = NULL;
   }
   g_count = 0;
   if (dropped > 0)
      g_invalidations++;
   pthread_mutex_unlock(&g_mtx);
   if (dropped > 0)
      aimee_log(LOG_INFO, "kbcache", "invalidated (%d entries dropped)", dropped);
}

void kb_cache_stats(long *hits, long *misses, long *invalidations)
{
   pthread_mutex_lock(&g_mtx);
   if (hits)
      *hits = g_hits;
   if (misses)
      *misses = g_misses;
   if (invalidations)
      *invalidations = g_invalidations;
   pthread_mutex_unlock(&g_mtx);
}
