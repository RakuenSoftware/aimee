/* shadow_mirror.c -- see shadow_mirror.h. */
#include "aimee.h"

#include "shadow_mirror.h"

#include "agent_exec.h" /* agent_http_post */
#include "log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Bound concurrent delivery threads: each blocks for a subscriber's whole turn, so
 * without a cap a burst across many subscribers would spawn an unbounded pile. At
 * the cap we DROP (and count) -- shadow is best-effort, dropping is always safe. */
#define SHADOW_MIRROR_MAX_INFLIGHT 32
/* Only bounds a hung subscriber; the reply is discarded, so this is not a deadline. */
#define SHADOW_MIRROR_TIMEOUT_MS 300000
/* Cap the subscriber table (test VMs, not a crowd). */
#define SHADOW_MIRROR_MAX_SUBS 16
/* Prune a subscriber after this many consecutive failed deliveries: it went away.
 * A periodic re-subscribe (idempotent) rescues one pruned after a transient blip. */
#define SHADOW_MIRROR_FAIL_LIMIT 10

typedef struct
{
   char base_url[512];
   char bearer[256];
   int fails; /* consecutive delivery failures */
   int used;
} shadow_sub_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static shadow_sub_t g_subs[SHADOW_MIRROR_MAX_SUBS];

/* The ONE thing that arms shadow publishing. In-memory only, initialised to 0, so
 * the process ALWAYS boots disarmed: enabling it via POST /v1/shadow/enable can
 * never survive a restart. There is deliberately no config/env knob that could
 * re-arm it at boot -- that is the whole safety property (an operator can't enable
 * tapping of live prompt/response content, reboot, and have it silently persist). */
static volatile int g_armed;

static volatile long g_inflight;
static volatile long g_sent;
static volatile long g_dropped;
static volatile long g_pruned;

int shadow_mirror_publish_enabled(void)
{
   return __atomic_load_n(&g_armed, __ATOMIC_RELAXED) ? 1 : 0;
}

void shadow_mirror_set_armed(int on)
{
   __atomic_store_n(&g_armed, on ? 1 : 0, __ATOMIC_RELAXED);
   if (!on)
   {
      /* Disarming is a hard stop: forget every subscriber so a later re-arm starts
       * clean and no tap targets linger while disarmed. */
      pthread_mutex_lock(&g_lock);
      memset(g_subs, 0, sizeof(g_subs));
      pthread_mutex_unlock(&g_lock);
   }
   LOG_INFO("shadow.mirror", "shadow publishing %s", on ? "ARMED (runtime)" : "disarmed");
}

int shadow_mirror_is_mirrorable_path(const char *path)
{
   if (!path)
      return 0;
   return strcmp(path, "/v1/messages") == 0 || strcmp(path, "/v1/chat/completions") == 0 ||
          strcmp(path, "/v1/completions") == 0 || strcmp(path, "/v1/responses") == 0;
}

/* caller holds g_lock */
static shadow_sub_t *find_sub_locked(const char *base_url)
{
   for (int i = 0; i < SHADOW_MIRROR_MAX_SUBS; i++)
      if (g_subs[i].used && strcmp(g_subs[i].base_url, base_url) == 0)
         return &g_subs[i];
   return NULL;
}

int shadow_mirror_subscribe(const char *base_url, const char *bearer)
{
   if (!base_url || !base_url[0] || strlen(base_url) >= sizeof(g_subs[0].base_url))
      return -1;
   if (bearer && strlen(bearer) >= sizeof(g_subs[0].bearer))
      return -1;

   pthread_mutex_lock(&g_lock);
   shadow_sub_t *s = find_sub_locked(base_url);
   if (!s)
   {
      for (int i = 0; i < SHADOW_MIRROR_MAX_SUBS; i++)
         if (!g_subs[i].used)
         {
            s = &g_subs[i];
            break;
         }
   }
   if (!s)
   {
      pthread_mutex_unlock(&g_lock);
      return -1; /* table full */
   }
   s->used = 1;
   snprintf(s->base_url, sizeof(s->base_url), "%s", base_url);
   snprintf(s->bearer, sizeof(s->bearer), "%s", bearer ? bearer : "");
   s->fails = 0; /* refresh: a re-subscribe clears prior failures */
   pthread_mutex_unlock(&g_lock);
   LOG_INFO("shadow.mirror", "subscriber registered: %s", base_url);
   return 0;
}

int shadow_mirror_unsubscribe(const char *base_url)
{
   if (!base_url)
      return -1;
   pthread_mutex_lock(&g_lock);
   shadow_sub_t *s = find_sub_locked(base_url);
   if (s)
      memset(s, 0, sizeof(*s));
   pthread_mutex_unlock(&g_lock);
   return s ? 0 : -1;
}

int shadow_mirror_subscriber_count(void)
{
   int n = 0;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < SHADOW_MIRROR_MAX_SUBS; i++)
      if (g_subs[i].used)
         n++;
   pthread_mutex_unlock(&g_lock);
   return n;
}

/* Record the outcome of a delivery: reset failures on success; on failure count up
 * and prune the subscriber once it has clearly gone away. */
static void record_delivery(const char *base_url, int ok)
{
   pthread_mutex_lock(&g_lock);
   shadow_sub_t *s = find_sub_locked(base_url);
   if (s)
   {
      if (ok)
         s->fails = 0;
      else if (++s->fails >= SHADOW_MIRROR_FAIL_LIMIT)
      {
         LOG_INFO("shadow.mirror", "pruning unreachable subscriber: %s", base_url);
         memset(s, 0, sizeof(*s));
         __atomic_fetch_add(&g_pruned, 1, __ATOMIC_RELAXED);
      }
   }
   pthread_mutex_unlock(&g_lock);
}

typedef struct
{
   char *url;      /* base_url + path, owned */
   char *auth;     /* "Bearer <token>" or NULL, owned */
   char *body;     /* request body copy, owned */
   char *base_url; /* for failure accounting, owned */
} mirror_job_t;

static void mirror_job_free(mirror_job_t *j)
{
   if (!j)
      return;
   free(j->url);
   free(j->auth);
   free(j->body);
   free(j->base_url);
   free(j);
}

static void *mirror_send_thread(void *arg)
{
   mirror_job_t *j = (mirror_job_t *)arg;
   char *resp = NULL;
   /* X-Aimee-Shadow marks this as mirrored traffic: the subscriber processes it
    * normally but must NOT re-mirror (loop guard on the receiving side). */
   int rc = agent_http_post(j->url, j->auth, j->body, &resp, SHADOW_MIRROR_TIMEOUT_MS,
                            "X-Aimee-Shadow: 1\r\n");
   free(resp); /* the subscriber's reply is deliberately discarded */
   record_delivery(j->base_url, rc == 0);
   if (rc != 0)
      LOG_DEBUG("shadow.mirror", "delivery to %s failed (rc=%d)", j->base_url, rc);
   mirror_job_free(j);
   __atomic_fetch_sub(&g_inflight, 1, __ATOMIC_RELAXED);
   return NULL;
}

/* Spawn one detached delivery to `sub` for (path, body); bounded by the in-flight
 * cap. On any resource failure the slot is backed out and the mirror dropped. */
static void deliver_one(const shadow_sub_t *sub, const char *path, const char *body, int body_len)
{
   long now = __atomic_add_fetch(&g_inflight, 1, __ATOMIC_RELAXED);
   if (now > SHADOW_MIRROR_MAX_INFLIGHT)
   {
      __atomic_fetch_sub(&g_inflight, 1, __ATOMIC_RELAXED);
      __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED);
      return;
   }

   mirror_job_t *j = calloc(1, sizeof(*j));
   if (!j)
   {
      __atomic_fetch_sub(&g_inflight, 1, __ATOMIC_RELAXED);
      return;
   }

   size_t urllen = strlen(sub->base_url) + strlen(path) + 1;
   j->url = malloc(urllen);
   if (j->url)
      snprintf(j->url, urllen, "%s%s", sub->base_url, path);
   j->base_url = strdup(sub->base_url);
   if (sub->bearer[0])
   {
      size_t alen = strlen(sub->bearer) + 8;
      j->auth = malloc(alen);
      if (j->auth)
         snprintf(j->auth, alen, "Bearer %s", sub->bearer);
   }
   j->body = malloc((size_t)body_len + 1);
   if (j->body)
   {
      memcpy(j->body, body, (size_t)body_len);
      j->body[body_len] = '\0';
   }
   if (!j->url || !j->body || !j->base_url)
   {
      mirror_job_free(j);
      __atomic_fetch_sub(&g_inflight, 1, __ATOMIC_RELAXED);
      return;
   }

   pthread_t t;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   int perr = pthread_create(&t, &attr, mirror_send_thread, j);
   pthread_attr_destroy(&attr);
   if (perr != 0)
   {
      mirror_job_free(j);
      __atomic_fetch_sub(&g_inflight, 1, __ATOMIC_RELAXED);
      return;
   }
   __atomic_fetch_add(&g_sent, 1, __ATOMIC_RELAXED);
}

void shadow_mirror_dispatch(const char *path, const char *body, int body_len, int is_shadow_inbound)
{
   /* Loop guard: never mirror a request that is itself a mirror. */
   if (is_shadow_inbound)
      return;
   if (!shadow_mirror_is_mirrorable_path(path) || !body || body_len <= 0)
      return;
   if (!shadow_mirror_publish_enabled())
      return;

   /* Snapshot subscribers under the lock, then deliver outside it: a delivery
    * blocks for the whole peer turn and must not hold the registry lock. */
   shadow_sub_t snap[SHADOW_MIRROR_MAX_SUBS];
   int n = 0;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < SHADOW_MIRROR_MAX_SUBS; i++)
      if (g_subs[i].used)
         snap[n++] = g_subs[i];
   pthread_mutex_unlock(&g_lock);

   for (int i = 0; i < n; i++)
      deliver_one(&snap[i], path, body, body_len);
}

long shadow_mirror_sent_count(void)
{
   return __atomic_load_n(&g_sent, __ATOMIC_RELAXED);
}
long shadow_mirror_dropped_count(void)
{
   return __atomic_load_n(&g_dropped, __ATOMIC_RELAXED);
}
long shadow_mirror_pruned_count(void)
{
   return __atomic_load_n(&g_pruned, __ATOMIC_RELAXED);
}

void shadow_mirror_reset(void)
{
   __atomic_store_n(&g_armed, 0, __ATOMIC_RELAXED);
   pthread_mutex_lock(&g_lock);
   memset(g_subs, 0, sizeof(g_subs));
   pthread_mutex_unlock(&g_lock);
   __atomic_store_n(&g_sent, 0, __ATOMIC_RELAXED);
   __atomic_store_n(&g_dropped, 0, __ATOMIC_RELAXED);
   __atomic_store_n(&g_pruned, 0, __ATOMIC_RELAXED);
}
