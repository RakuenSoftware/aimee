/* provider_catalog.c: live per-provider health and locality catalog.
 *
 * Maintains a module-global table of per-agent health state derived
 * from observed delegate outcomes.  The locality of each agent is
 * classified once at init time from the endpoint URL; health degrades
 * on consecutive failures and recovers on success.
 *
 * Thread-safety: a single mutex guards all catalog mutations.
 * Readers (health / locality query) hold the lock for the duration
 * of the lookup.
 */
#include "provider_catalog.h"
#include "log.h"
#include "cli_client.h"
#include "cJSON.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ---- Module-private state ---- */

typedef struct
{
   provider_catalog_entry_t entries[CATALOG_MAX_ENTRIES];
   int count;
   pthread_mutex_t lock;
   int initialized;
} catalog_state_t;

static catalog_state_t g_cat;
static pthread_once_t g_cat_once = PTHREAD_ONCE_INIT;

static void catalog_once_init(void)
{
   memset(&g_cat, 0, sizeof(g_cat));
   pthread_mutex_init(&g_cat.lock, NULL);
   g_cat.initialized = 1;
}

/* ---- Endpoint locality classification ---- */

provider_locality_t provider_catalog_classify_endpoint(const char *endpoint)
{
   if (!endpoint || !endpoint[0])
      return PROVIDER_LOCALITY_UNKNOWN;

   /* Unix socket / empty host */
   if (strncmp(endpoint, "unix://", 7) == 0 || endpoint[0] == '/')
      return PROVIDER_LOCALITY_LOCAL;

   /* Loopback patterns */
   if (strstr(endpoint, "localhost") || strstr(endpoint, "127.0.0.1") || strstr(endpoint, "::1") ||
       strstr(endpoint, "0.0.0.0"))
      return PROVIDER_LOCALITY_LOCAL;

   /* RFC-1918 / link-local patterns */
   if (strstr(endpoint, "192.168.") || strstr(endpoint, "169.254."))
      return PROVIDER_LOCALITY_LAN;

   /* 10.x.x.x */
   const char *p = strstr(endpoint, "://");
   const char *host = p ? p + 3 : endpoint;
   if (strncmp(host, "10.", 3) == 0)
      return PROVIDER_LOCALITY_LAN;

   /* 172.16.x.x – 172.31.x.x */
   if (strncmp(host, "172.", 4) == 0)
   {
      int second = atoi(host + 4);
      if (second >= 16 && second <= 31)
         return PROVIDER_LOCALITY_LAN;
   }

   return PROVIDER_LOCALITY_REMOTE;
}

const char *provider_locality_label(provider_locality_t loc)
{
   switch (loc)
   {
   case PROVIDER_LOCALITY_LOCAL:
      return "local";
   case PROVIDER_LOCALITY_LAN:
      return "lan";
   case PROVIDER_LOCALITY_REMOTE:
      return "remote";
   default:
      return "unknown";
   }
}

const char *catalog_health_label(catalog_health_t h)
{
   switch (h)
   {
   case CATALOG_HEALTH_HEALTHY:
      return "healthy";
   case CATALOG_HEALTH_DEGRADED:
      return "degraded";
   case CATALOG_HEALTH_STALE:
      return "stale";
   case CATALOG_HEALTH_DOWN:
      return "down";
   default:
      return "unknown";
   }
}

/* ---- Catalog maintenance ---- */

void provider_catalog_init(const agent_t *agents, int count)
{
   pthread_once(&g_cat_once, catalog_once_init);

   pthread_mutex_lock(&g_cat.lock);

   memset(g_cat.entries, 0, sizeof(g_cat.entries));
   g_cat.count = 0;

   int n = count < CATALOG_MAX_ENTRIES ? count : CATALOG_MAX_ENTRIES;
   for (int i = 0; i < n; i++)
   {
      provider_catalog_entry_t *e = &g_cat.entries[g_cat.count++];
      snprintf(e->agent_name, sizeof(e->agent_name), "%s", agents[i].name);
      snprintf(e->endpoint, sizeof(e->endpoint), "%s", agents[i].endpoint);
      snprintf(e->provider, sizeof(e->provider), "%s", agents[i].provider);
      e->locality = provider_catalog_classify_endpoint(agents[i].endpoint);
      e->health = CATALOG_HEALTH_HEALTHY;
      e->last_success = 0;
      e->last_failure = 0;
      e->failure_streak = 0;
   }

   pthread_mutex_unlock(&g_cat.lock);
}

static provider_catalog_entry_t *find_entry_locked(const char *agent_name)
{
   for (int i = 0; i < g_cat.count; i++)
   {
      if (strcmp(g_cat.entries[i].agent_name, agent_name) == 0)
         return &g_cat.entries[i];
   }
   return NULL;
}

/* Recompute health state from streak + last_success timestamp. */
static void recompute_health(provider_catalog_entry_t *e)
{
   time_t now = time(NULL);

   if (e->failure_streak >= 3)
   {
      /* Circuit breaker open. Half-open once the cooldown since the last
       * failure has elapsed so a transiently-failing provider can be probed
       * and recover, instead of staying wedged until a server restart. The
       * streak is preserved: a probe failure (record_failure refreshes
       * last_failure) snaps it straight back to DOWN for another cooldown,
       * while a probe success clears the streak. */
      if (e->last_failure > 0 && difftime(now, e->last_failure) >= PROVIDER_DOWN_COOLDOWN_SECONDS)
      {
         e->health = CATALOG_HEALTH_DEGRADED;
         return;
      }
      e->health = CATALOG_HEALTH_DOWN;
      return;
   }

   if (e->failure_streak >= 1)
   {
      e->health = CATALOG_HEALTH_DEGRADED;
      return;
   }

   /* No active failures — check staleness */
   if (e->last_success > 0)
   {
      double age = difftime(now, e->last_success);
      if (age > PROVIDER_DOWN_SECONDS)
      {
         e->health = CATALOG_HEALTH_DOWN;
         return;
      }
      if (age > PROVIDER_STALE_SECONDS)
      {
         e->health = CATALOG_HEALTH_STALE;
         return;
      }
   }

   e->health = CATALOG_HEALTH_HEALTHY;
}

void provider_catalog_record_success(const char *agent_name)
{
   if (!agent_name || !agent_name[0])
      return;

   pthread_once(&g_cat_once, catalog_once_init);
   pthread_mutex_lock(&g_cat.lock);

   provider_catalog_entry_t *e = find_entry_locked(agent_name);
   if (e)
   {
      e->failure_streak = 0;
      e->last_success = time(NULL);
      recompute_health(e);
   }

   pthread_mutex_unlock(&g_cat.lock);
}

void provider_catalog_record_failure(const char *agent_name, const char *failure_class)
{
   if (!agent_name || !agent_name[0])
      return;

   pthread_once(&g_cat_once, catalog_once_init);
   pthread_mutex_lock(&g_cat.lock);

   provider_catalog_entry_t *e = find_entry_locked(agent_name);
   if (e)
   {
      e->failure_streak++;
      e->last_failure = time(NULL);
      if (failure_class && failure_class[0])
         snprintf(e->last_failure_class, sizeof(e->last_failure_class), "%s", failure_class);
      recompute_health(e);

      if (e->health == CATALOG_HEALTH_DEGRADED || e->health == CATALOG_HEALTH_DOWN)
         aimee_log(LOG_WARN, "provider_catalog", "agent '%s' health → %s (streak %d, class: %s)",
                   agent_name, catalog_health_label(e->health), e->failure_streak,
                   e->last_failure_class[0] ? e->last_failure_class : "unknown");
      else
         aimee_log(LOG_DEBUG, "provider_catalog", "agent '%s' failure streak %d → %s", agent_name,
                   e->failure_streak, catalog_health_label(e->health));
   }

   pthread_mutex_unlock(&g_cat.lock);
}

catalog_health_t provider_catalog_get_health(const char *agent_name)
{
   if (!agent_name || !agent_name[0])
      return CATALOG_HEALTH_HEALTHY;

   pthread_once(&g_cat_once, catalog_once_init);
   pthread_mutex_lock(&g_cat.lock);

   provider_catalog_entry_t *e = find_entry_locked(agent_name);
   /* A streak-DOWN agent is never routed to, so it receives no further
    * record_*() calls to drive recovery. Recompute on read (scoped to an
    * active failure streak, to avoid age-based staleness silently sidelining
    * an idle-but-healthy agent) so the cooldown can half-open the breaker. */
   if (e && e->failure_streak >= 3)
      recompute_health(e);
   catalog_health_t h = e ? e->health : CATALOG_HEALTH_HEALTHY;

   pthread_mutex_unlock(&g_cat.lock);
   return h;
}

provider_locality_t provider_catalog_get_locality(const char *agent_name)
{
   if (!agent_name || !agent_name[0])
      return PROVIDER_LOCALITY_UNKNOWN;

   pthread_once(&g_cat_once, catalog_once_init);
   pthread_mutex_lock(&g_cat.lock);

   provider_catalog_entry_t *e = find_entry_locked(agent_name);
   provider_locality_t loc = e ? e->locality : PROVIDER_LOCALITY_UNKNOWN;

   pthread_mutex_unlock(&g_cat.lock);
   return loc;
}

int provider_catalog_inferred_concurrency(const char *agent_name)
{
   provider_locality_t loc = provider_catalog_get_locality(agent_name);
   switch (loc)
   {
   case PROVIDER_LOCALITY_LOCAL:
      return PROVIDER_LOCAL_CONCURRENCY_DEFAULT;
   case PROVIDER_LOCALITY_LAN:
      return PROVIDER_LAN_CONCURRENCY_DEFAULT;
   default:
      return 0;
   }
}

int provider_catalog_dump_json(char *buf, size_t cap)
{
   if (!buf || cap < 3)
      return -1;

   pthread_once(&g_cat_once, catalog_once_init);
   pthread_mutex_lock(&g_cat.lock);

   int pos = 0;
   buf[pos++] = '[';

   for (int i = 0; i < g_cat.count; i++)
   {
      const provider_catalog_entry_t *e = &g_cat.entries[i];
      char entry[512];
      int n = snprintf(entry, sizeof(entry),
                       "%s{\"agent\":\"%s\",\"provider\":\"%s\","
                       "\"locality\":\"%s\",\"health\":\"%s\","
                       "\"failure_streak\":%d}",
                       i > 0 ? "," : "", e->agent_name, e->provider,
                       provider_locality_label(e->locality), catalog_health_label(e->health),
                       e->failure_streak);
      if (pos + n + 2 >= (int)cap)
      {
         pthread_mutex_unlock(&g_cat.lock);
         return -1;
      }
      memcpy(buf + pos, entry, (size_t)n);
      pos += n;
   }

   buf[pos++] = ']';
   buf[pos] = '\0';

   pthread_mutex_unlock(&g_cat.lock);
   return pos;
}

/* Try to acquire / release a slot via the server RPC.
 * Returns 1 (acquired), 0 (at limit), or -1 (server unreachable — use
 * in-process fallback). Timeout is short: we'd rather fall back than block
 * the delegate launch waiting for a slow server. */
static int server_slot_acquire(const char *agent_name, int max_parallel)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "provider.slot_acquire");
   cJSON_AddStringToObject(req, "agent_name", agent_name);
   cJSON_AddNumberToObject(req, "max_parallel", max_parallel);
   cJSON *resp = cli_v1_dispatch_local(req, 2000); /* co-located /v1 dispatch */
   cJSON_Delete(req);
   if (!resp)
      return -1;
   cJSON *acq = cJSON_GetObjectItemCaseSensitive(resp, "acquired");
   int result = cJSON_IsTrue(acq) ? 1 : 0;
   cJSON_Delete(resp);
   return result;
}

/* Release a slot via the server. Returns 0 on success, -1 if unreachable. */
static int server_slot_release_try(const char *agent_name)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "provider.slot_release");
   cJSON_AddStringToObject(req, "agent_name", agent_name);
   cJSON *resp = cli_v1_dispatch_local(req, 2000); /* co-located /v1 dispatch */
   cJSON_Delete(req);
   cJSON_Delete(resp);
   return 0;
}

int provider_catalog_concurrent_acquire(const char *agent_name, int max_parallel)
{
   if (!agent_name || !agent_name[0] || max_parallel <= 0)
      return 1; /* unlimited */

   /* Try the server first: it holds the authoritative global count and resets
    * cleanly on restart. Fall back to in-process tracking when the server is
    * not running (dev mode, standalone invocation). */
   int srv = server_slot_acquire(agent_name, max_parallel);
   if (srv >= 0)
      return srv;

   /* Fallback: in-process tracking. */
   pthread_once(&g_cat_once, catalog_once_init);
   pthread_mutex_lock(&g_cat.lock);
   provider_catalog_entry_t *e = find_entry_locked(agent_name);
   if (!e)
   {
      pthread_mutex_unlock(&g_cat.lock);
      return 1; /* unknown agent — don't block */
   }
   if (e->active_count >= max_parallel)
   {
      pthread_mutex_unlock(&g_cat.lock);
      return 0;
   }
   e->active_count++;
   pthread_mutex_unlock(&g_cat.lock);
   return 1;
}

void provider_catalog_concurrent_release(const char *agent_name)
{
   if (!agent_name || !agent_name[0])
      return;

   /* Try the server; it's a fast no-op when unreachable. If the server is up
    * it owns the slot, so we're done. If it's down the slot was tracked
    * in-process, so decrement there. */
   if (server_slot_release_try(agent_name) == 0)
      return;

   pthread_once(&g_cat_once, catalog_once_init);
   pthread_mutex_lock(&g_cat.lock);
   provider_catalog_entry_t *e = find_entry_locked(agent_name);
   if (e && e->active_count > 0)
      e->active_count--;
   pthread_mutex_unlock(&g_cat.lock);
}
