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
      snprintf(e->registration, sizeof(e->registration), "%s", agents[i].registration);
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
/* Cooldown for this entry: base, doubled per breaker trip beyond the first,
 * capped. See provider_catalog_cooldown_seconds() for why trips and not streak. */
static int cooldown_for_locked(const provider_catalog_entry_t *e)
{
   int cooldown = PROVIDER_DOWN_COOLDOWN_SECONDS;
   for (int i = 0; e && i < e->breaker_trips && cooldown < PROVIDER_DOWN_COOLDOWN_MAX_SECONDS; i++)
      cooldown *= 2;
   return cooldown > PROVIDER_DOWN_COOLDOWN_MAX_SECONDS ? PROVIDER_DOWN_COOLDOWN_MAX_SECONDS
                                                        : cooldown;
}

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
      if (e->last_failure > 0 && difftime(now, e->last_failure) >= cooldown_for_locked(e))
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
      e->breaker_trips = 0;
      e->last_success = time(NULL);
      recompute_health(e);
      /* A successful credential use recovers siblings blocked by that account,
       * without erasing their independent model-specific failures. */
      if (e->registration[0])
         for (int i = 0; i < g_cat.count; i++)
         {
            provider_catalog_entry_t *peer = &g_cat.entries[i];
            if (peer != e && strcmp(peer->registration, e->registration) == 0 &&
                strcmp(peer->last_failure_class, "registration_error") == 0)
            {
               peer->failure_streak = peer->breaker_trips = 0;
               peer->last_success = e->last_success;
               peer->last_failure_class[0] = 0;
               recompute_health(peer);
            }
         }
      e->last_failure_class[0] = 0;
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
      /* Count a breaker TRIP only for a genuine half-open probe re-failure - a
       * failure arriving while the breaker had already opened (streak >= 3) and
       * then HALF-OPENED to DEGRADED after its cooldown. That is real evidence
       * the provider is still broken, and it lengthens the next cooldown.
       *
       * Both clauses are needed, because DEGRADED has TWO sources in
       * recompute_health: the half-open transition from DOWN (streak >= 3), and
       * an ordinary early streak of 1-2 failures. The streak >= 3 clause excludes
       * the latter - a second failure while merely warming up must not be counted
       * as a breaker trip and start backing off before the breaker has even
       * opened. The burst of concurrent failures that first opens the breaker is
       * also excluded: those arrive while HEALTHY or already DOWN, so counting
       * them would let parallelism, not elapsed time, drive the backoff.
       *
       * Under concurrency this can under-count - two probes half-open, both fail,
       * the first flips DEGRADED->DOWN so the second sees DOWN and is not counted.
       * That is acceptable: it is one trip per half-open CYCLE, and real cycles
       * are serialised by the wall-clock cooldown, so the backoff still grows
       * monotonically across them. Over-counting, which would over-penalise, is
       * what the gates prevent. */
      if (e->health == CATALOG_HEALTH_DEGRADED && e->failure_streak >= 3)
         e->breaker_trips++;
      e->failure_streak++;
      e->last_failure = time(NULL);
      if (failure_class && failure_class[0])
         snprintf(e->last_failure_class, sizeof(e->last_failure_class), "%s", failure_class);
      recompute_health(e);
      if (e->registration[0] && failure_class && strcmp(failure_class, "registration_error") == 0)
         for (int i = 0; i < g_cat.count; i++)
         {
            provider_catalog_entry_t *peer = &g_cat.entries[i];
            if (peer == e || strcmp(peer->registration, e->registration) != 0)
               continue;
            peer->failure_streak = e->failure_streak;
            peer->breaker_trips = e->breaker_trips;
            peer->last_failure = e->last_failure;
            snprintf(peer->last_failure_class, sizeof(peer->last_failure_class),
                     "registration_error");
            recompute_health(peer);
         }

      if (e->health == CATALOG_HEALTH_DOWN)
         /* DOWN excludes the agent from routing, so this is the line an operator
          * needs to correlate a quorum/availability problem to a specific seat.
          * Carry the class, the trip-driven cooldown and when the next probe can
          * half-open it, so a persistent outage reads as a growing cooldown
          * rather than a silent 60s heartbeat. */
         aimee_log(LOG_WARN, "provider_catalog",
                   "agent '%s' health → DOWN (streak %d, trips %d, class: %s, "
                   "cooldown %ds, next probe in <= %ds)",
                   agent_name, e->failure_streak, e->breaker_trips,
                   e->last_failure_class[0] ? e->last_failure_class : "unknown",
                   cooldown_for_locked(e), cooldown_for_locked(e));
      else if (e->health == CATALOG_HEALTH_DEGRADED)
         aimee_log(LOG_WARN, "provider_catalog",
                   "agent '%s' health → DEGRADED (streak %d, class: %s)", agent_name,
                   e->failure_streak, e->last_failure_class[0] ? e->last_failure_class : "unknown");
      else
         aimee_log(LOG_DEBUG, "provider_catalog", "agent '%s' failure streak %d → %s", agent_name,
                   e->failure_streak, catalog_health_label(e->health));
   }

   pthread_mutex_unlock(&g_cat.lock);
}

int provider_catalog_cooldown_seconds(const char *agent_name)
{
   if (!agent_name || !agent_name[0])
      return PROVIDER_DOWN_COOLDOWN_SECONDS;

   pthread_once(&g_cat_once, catalog_once_init);
   pthread_mutex_lock(&g_cat.lock);
   provider_catalog_entry_t *e = find_entry_locked(agent_name);
   int cd = e ? cooldown_for_locked(e) : PROVIDER_DOWN_COOLDOWN_SECONDS;
   pthread_mutex_unlock(&g_cat.lock);
   return cd;
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
