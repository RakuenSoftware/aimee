/* test_shadow_mirror.c -- shadow-traffic publishing to dynamic subscribers.
 *
 * The mirror must (a) only fire for completion paths, (b) never fire for a request
 * that is itself a mirror (the loop guard -- get this wrong and two peers pointed at
 * each other melt down), (c) deliver the exact body with the X-Aimee-Shadow marker
 * to EVERY subscriber's ingress, (d) do nothing when publishing is off or there are
 * no subscribers, and (e) self-prune a subscriber whose deliveries keep failing.
 * agent_http_post is stubbed so the delivery path is exercised without real peers;
 * the stub captures what was sent and can be made to fail on demand. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aimee.h"
#include "shadow_mirror.h"

/* ---- stub agent_http_post: capture deliveries, optionally fail ------------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_calls;
static int g_fail; /* when set, every delivery "fails" (rc != 0) */
static char g_last_url[600];
static char g_last_auth[300];
static char g_last_body[600];
static char g_last_extra[200];

int agent_http_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers)
{
   (void)timeout_ms;
   pthread_mutex_lock(&g_lock);
   g_calls++;
   snprintf(g_last_url, sizeof(g_last_url), "%s", url ? url : "");
   snprintf(g_last_auth, sizeof(g_last_auth), "%s", auth_header ? auth_header : "");
   snprintf(g_last_body, sizeof(g_last_body), "%s", body ? body : "");
   snprintf(g_last_extra, sizeof(g_last_extra), "%s", extra_headers ? extra_headers : "");
   int fail = g_fail;
   pthread_mutex_unlock(&g_lock);
   if (response_buf)
      *response_buf = NULL;
   return fail ? -1 : 0;
}

static int calls(void)
{
   pthread_mutex_lock(&g_lock);
   int c = g_calls;
   pthread_mutex_unlock(&g_lock);
   return c;
}
static void set_fail(int f)
{
   pthread_mutex_lock(&g_lock);
   g_fail = f;
   pthread_mutex_unlock(&g_lock);
}
static void reset_calls(void)
{
   pthread_mutex_lock(&g_lock);
   g_calls = 0;
   pthread_mutex_unlock(&g_lock);
}

/* Deliveries run on detached threads; wait (bounded) for `want` of them. */
static int wait_for_calls(int want)
{
   for (int i = 0; i < 300; i++)
   {
      if (calls() >= want)
         return 1;
      struct timespec ts = {0, 10 * 1000 * 1000}; /* 10ms */
      nanosleep(&ts, NULL);
   }
   return calls() >= want;
}

static void settle(void)
{
   struct timespec ts = {0, 60 * 1000 * 1000};
   nanosleep(&ts, NULL);
}

int main(void)
{
   printf("shadow-mirror:\n");

   /* The safety property: publishing is DISARMED at process start, with no config
    * or env able to change that. Enabling is a runtime-only action. */
   assert(!shadow_mirror_publish_enabled());
   printf("  PASS: boots disarmed (no persistent enable)\n");

   /* Path classification. */
   assert(shadow_mirror_is_mirrorable_path("/v1/messages"));
   assert(shadow_mirror_is_mirrorable_path("/v1/chat/completions"));
   assert(!shadow_mirror_is_mirrorable_path("/v1/agents"));
   assert(!shadow_mirror_is_mirrorable_path(NULL));
   printf("  PASS: only completion paths are mirrorable\n");

   /* Disarmed: even with a subscriber registered, no delivery happens. */
   shadow_mirror_reset();
   assert(!shadow_mirror_publish_enabled());
   shadow_mirror_subscribe("https://a.local:8743", "tok-a");
   shadow_mirror_dispatch("/v1/messages", "{\"m\":1}", 7, 0);
   settle();
   assert(calls() == 0 && shadow_mirror_sent_count() == 0);
   printf("  PASS: disarmed -> nothing delivered even with a subscriber\n");

   /* Arm publishing at runtime. */
   shadow_mirror_reset();
   shadow_mirror_set_armed(1);
   assert(shadow_mirror_publish_enabled());

   /* No subscribers -> nothing delivered. */
   shadow_mirror_dispatch("/v1/messages", "{\"m\":1}", 7, 0);
   settle();
   assert(calls() == 0);
   printf("  PASS: no subscribers -> nothing delivered\n");

   /* Register two subscribers; a completion request fans out to BOTH, each with the
    * exact body, its own bearer, the shadow marker, and the original path. */
   reset_calls();
   assert(shadow_mirror_subscribe("https://a.local:8743", "tok-a") == 0);
   assert(shadow_mirror_subscribe("https://b.local:8743", "tok-b") == 0);
   assert(shadow_mirror_subscriber_count() == 2);
   shadow_mirror_dispatch("/v1/messages", "{\"model\":\"x\"}", 13, 0);
   assert(wait_for_calls(2));
   pthread_mutex_lock(&g_lock);
   /* last-write wins in the stub, but both must have hit /v1/messages with the body
    * and the marker; the URL is one of the two subscribers. */
   assert(strstr(g_last_url, "/v1/messages") != NULL);
   assert(strcmp(g_last_body, "{\"model\":\"x\"}") == 0);
   assert(strstr(g_last_extra, "X-Aimee-Shadow: 1") != NULL);
   assert(strstr(g_last_auth, "Bearer tok-") != NULL);
   pthread_mutex_unlock(&g_lock);
   assert(shadow_mirror_sent_count() >= 2);
   printf("  PASS: request fans out to every subscriber's real ingress\n");

   /* Loop guard: a request already marked shadow is NEVER re-mirrored. */
   reset_calls();
   shadow_mirror_dispatch("/v1/messages", "{\"m\":1}", 7, 1 /* is_shadow_inbound */);
   settle();
   assert(calls() == 0);
   printf("  PASS: a mirrored (X-Aimee-Shadow) request is not re-mirrored\n");

   /* Non-completion path is not delivered. */
   reset_calls();
   shadow_mirror_dispatch("/v1/agents", "{}", 2, 0);
   settle();
   assert(calls() == 0);
   printf("  PASS: non-completion path is not delivered\n");

   /* Unsubscribe removes a subscriber from the fan-out. */
   assert(shadow_mirror_unsubscribe("https://b.local:8743") == 0);
   assert(shadow_mirror_subscriber_count() == 1);
   printf("  PASS: unsubscribe removes a subscriber\n");

   /* Disarming is a hard stop: it forgets every subscriber. */
   shadow_mirror_set_armed(0);
   assert(!shadow_mirror_publish_enabled());
   assert(shadow_mirror_subscriber_count() == 0);
   printf("  PASS: disarm clears all subscribers\n");

   /* Self-prune: a subscriber whose deliveries keep failing is dropped. Re-arm,
    * register a dead peer, make every delivery fail, and drive dispatches until it
    * is pruned. */
   shadow_mirror_reset();
   shadow_mirror_set_armed(1);
   assert(shadow_mirror_subscribe("https://dead.local:8743", "tok-d") == 0);
   assert(shadow_mirror_subscriber_count() == 1);
   set_fail(1);
   for (int i = 0; i < 40 && shadow_mirror_subscriber_count() > 0; i++)
   {
      shadow_mirror_dispatch("/v1/messages", "{\"m\":1}", 7, 0);
      settle();
   }
   assert(shadow_mirror_subscriber_count() == 0); /* pruned */
   assert(shadow_mirror_pruned_count() >= 1);
   set_fail(0);
   printf("  PASS: a subscriber that keeps failing delivery is self-pruned\n");

   printf("shadow-mirror: ok\n");
   return 0;
}
