/* web_search_breaker.c: see web_search_breaker.h. */

#include "web_search_breaker.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Engines are named by config strings; the set is small and fixed in practice
 * (duckduckgo, searxng, tavily), so a linear table is the whole data structure. */
#define BREAKER_MAX_ENGINES 8
#define BREAKER_NAME_MAX    32

typedef struct
{
   char name[BREAKER_NAME_MAX];
   int consecutive_failures;
   long opened_at;  /* 0 when closed */
   int probe_taken; /* a half-open probe is outstanding */
} breaker_t;

static breaker_t s_engines[BREAKER_MAX_ENGINES];
static int s_count = 0;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static long (*s_clock)(void) = NULL;

static long breaker_now(void)
{
   return s_clock ? s_clock() : (long)time(NULL);
}

/* Caller holds s_lock. Returns NULL only when the table is full, which cannot
 * happen for the configured backends. */
static breaker_t *engine_slot(const char *engine)
{
   if (!engine || !engine[0])
      return NULL;
   for (int i = 0; i < s_count; i++)
      if (strcmp(s_engines[i].name, engine) == 0)
         return &s_engines[i];
   if (s_count >= BREAKER_MAX_ENGINES)
      return NULL;
   breaker_t *e = &s_engines[s_count++];
   snprintf(e->name, sizeof(e->name), "%s", engine);
   e->consecutive_failures = 0;
   e->opened_at = 0;
   e->probe_taken = 0;
   return e;
}

int web_search_breaker_allow(const char *engine)
{
   pthread_mutex_lock(&s_lock);
   breaker_t *e = engine_slot(engine);
   int allow = 1;
   if (e && e->opened_at != 0)
   {
      if (breaker_now() - e->opened_at < WEB_BREAKER_COOLDOWN_SECONDS)
         allow = 0; /* still benched */
      else if (e->probe_taken)
         allow = 0; /* someone else already holds the single probe */
      else
      {
         /* Hand out the one half-open probe. Marking it here, under the lock, is
          * what stops two concurrent callers from both probing a dead engine. */
         e->probe_taken = 1;
         allow = 1;
      }
   }
   pthread_mutex_unlock(&s_lock);
   return allow;
}

void web_search_breaker_report(const char *engine, int ok)
{
   pthread_mutex_lock(&s_lock);
   breaker_t *e = engine_slot(engine);
   if (e)
   {
      if (ok)
      {
         e->consecutive_failures = 0;
         e->opened_at = 0;
         e->probe_taken = 0;
      }
      else
      {
         if (e->consecutive_failures < WEB_BREAKER_THRESHOLD)
            e->consecutive_failures++;
         /* A failed probe re-opens the breaker for a fresh cooldown rather than
          * leaving it half-open forever. */
         if (e->consecutive_failures >= WEB_BREAKER_THRESHOLD)
            e->opened_at = breaker_now();
         e->probe_taken = 0;
      }
   }
   pthread_mutex_unlock(&s_lock);
}

int web_search_breaker_is_open(const char *engine)
{
   pthread_mutex_lock(&s_lock);
   breaker_t *e = engine_slot(engine);
   int open = 0;
   if (e && e->opened_at != 0)
      open = (breaker_now() - e->opened_at < WEB_BREAKER_COOLDOWN_SECONDS);
   pthread_mutex_unlock(&s_lock);
   return open;
}

void web_search_breaker_reset_all(void)
{
   pthread_mutex_lock(&s_lock);
   memset(s_engines, 0, sizeof(s_engines));
   s_count = 0;
   pthread_mutex_unlock(&s_lock);
}

void web_search_breaker_set_clock(long (*now)(void))
{
   pthread_mutex_lock(&s_lock);
   s_clock = now;
   pthread_mutex_unlock(&s_lock);
}
