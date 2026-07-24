/* test_web_search_breaker.c -- per-engine circuit breaker.
 *
 * The behaviours that matter: an empty result counts as failure, the breaker
 * trips only on CONSECUTIVE failures, exactly one probe is handed out per
 * cooldown, and a failed probe re-benches rather than leaving the engine
 * half-open forever. */
#include <assert.h>
#include <stdio.h>

#include "web_search_breaker.h"

/* Controllable clock: the cooldown is 60s and the suite is not going to wait. */
static long s_fake_now = 1000;
static long fake_clock(void)
{
   return s_fake_now;
}

static void test_starts_closed(void)
{
   web_search_breaker_reset_all();
   assert(web_search_breaker_allow("duckduckgo") == 1);
   assert(web_search_breaker_is_open("duckduckgo") == 0);
   /* An engine never seen before is allowed, not denied. */
   assert(web_search_breaker_allow("never-seen") == 1);
   printf("  PASS: unknown and healthy engines are allowed\n");
}

static void test_trips_only_on_consecutive_failures(void)
{
   web_search_breaker_reset_all();
   for (int i = 0; i < WEB_BREAKER_THRESHOLD - 1; i++)
      web_search_breaker_report("ddg", 0);
   assert(web_search_breaker_is_open("ddg") == 0); /* not yet */

   /* One success must clear the run, or an engine that fails intermittently
    * would eventually trip on failures that were never consecutive. */
   web_search_breaker_report("ddg", 1);
   for (int i = 0; i < WEB_BREAKER_THRESHOLD - 1; i++)
      web_search_breaker_report("ddg", 0);
   assert(web_search_breaker_is_open("ddg") == 0);

   web_search_breaker_report("ddg", 0); /* now consecutive threshold reached */
   assert(web_search_breaker_is_open("ddg") == 1);
   assert(web_search_breaker_allow("ddg") == 0);
   printf("  PASS: trips on consecutive failures only; success clears the run\n");
}

static void test_cooldown_then_single_probe(void)
{
   web_search_breaker_reset_all();
   web_search_breaker_set_clock(fake_clock);
   s_fake_now = 1000;

   for (int i = 0; i < WEB_BREAKER_THRESHOLD; i++)
      web_search_breaker_report("ddg", 0);
   assert(web_search_breaker_allow("ddg") == 0);

   /* Still benched one second before the cooldown expires. */
   s_fake_now = 1000 + WEB_BREAKER_COOLDOWN_SECONDS - 1;
   assert(web_search_breaker_allow("ddg") == 0);

   s_fake_now = 1000 + WEB_BREAKER_COOLDOWN_SECONDS;
   /* Exactly ONE caller gets the probe; a second concurrent caller must not
    * also be sent at a known-dead engine. */
   assert(web_search_breaker_allow("ddg") == 1);
   assert(web_search_breaker_allow("ddg") == 0);
   printf("  PASS: cooldown benches the engine, then hands out one probe\n");
}

static void test_probe_success_closes(void)
{
   web_search_breaker_reset_all();
   web_search_breaker_set_clock(fake_clock);
   s_fake_now = 2000;
   for (int i = 0; i < WEB_BREAKER_THRESHOLD; i++)
      web_search_breaker_report("ddg", 0);
   s_fake_now = 2000 + WEB_BREAKER_COOLDOWN_SECONDS;
   assert(web_search_breaker_allow("ddg") == 1);

   web_search_breaker_report("ddg", 1);
   assert(web_search_breaker_is_open("ddg") == 0);
   assert(web_search_breaker_allow("ddg") == 1); /* fully closed, no probe limit */
   assert(web_search_breaker_allow("ddg") == 1);
   printf("  PASS: a successful probe closes the breaker\n");
}

static void test_probe_failure_rebenches(void)
{
   web_search_breaker_reset_all();
   web_search_breaker_set_clock(fake_clock);
   s_fake_now = 3000;
   for (int i = 0; i < WEB_BREAKER_THRESHOLD; i++)
      web_search_breaker_report("ddg", 0);
   s_fake_now = 3000 + WEB_BREAKER_COOLDOWN_SECONDS;
   assert(web_search_breaker_allow("ddg") == 1); /* the probe */

   web_search_breaker_report("ddg", 0); /* probe failed */
   /* Must be benched again for a FRESH cooldown, not left half-open where every
    * caller would be handed a probe at a dead engine. */
   assert(web_search_breaker_is_open("ddg") == 1);
   assert(web_search_breaker_allow("ddg") == 0);

   s_fake_now = 3000 + 2 * WEB_BREAKER_COOLDOWN_SECONDS;
   assert(web_search_breaker_allow("ddg") == 1);
   printf("  PASS: a failed probe re-benches for a fresh cooldown\n");
}

/* The reason this module exists rather than reusing HTTP status. */
static void test_empty_counts_as_failure(void)
{
   web_search_breaker_reset_all();
   web_search_breaker_set_clock(fake_clock);
   s_fake_now = 4000;
   /* Caller reports ok=0 for a 200-with-no-results, so an engine that answers
    * politely but never returns anything still trips. */
   for (int i = 0; i < WEB_BREAKER_THRESHOLD; i++)
      web_search_breaker_report("bot-walled", 0);
   assert(web_search_breaker_is_open("bot-walled") == 1);
   printf("  PASS: empty results trip the breaker\n");
}

static void test_engines_are_independent(void)
{
   web_search_breaker_reset_all();
   web_search_breaker_set_clock(fake_clock);
   s_fake_now = 5000;
   for (int i = 0; i < WEB_BREAKER_THRESHOLD; i++)
      web_search_breaker_report("dead", 0);
   assert(web_search_breaker_is_open("dead") == 1);
   /* A dead engine must not bench a healthy one -- the entire point of fanout. */
   assert(web_search_breaker_is_open("alive") == 0);
   assert(web_search_breaker_allow("alive") == 1);
   printf("  PASS: one dead engine does not bench another\n");
}

int main(void)
{
   test_starts_closed();
   test_trips_only_on_consecutive_failures();
   test_cooldown_then_single_probe();
   test_probe_success_closes();
   test_probe_failure_rebenches();
   test_empty_counts_as_failure();
   test_engines_are_independent();
   web_search_breaker_set_clock(NULL);
   printf("web_search_breaker: all tests passed\n");
   return 0;
}
