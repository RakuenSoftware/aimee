/* The guardrail event's durability across the bus, emitter half.
 *
 * THE PROPERTY: every guardrail event the bus accepts reaches the store exactly
 * once, and a graceful stop drains the ones in flight. Load-bearing -- it is
 * what lets an operator read the guardrail history as fact rather than as a
 * sample.
 *
 * WHY THIS FILE READS NOTHING BACK. The original did: it emitted N events and
 * then called db1_guardrail_event_list() to check them. That worked while the
 * store was an in-process SQLite database. It cannot work now, because the
 * store is reached OVER THE BUS and obs_bus_stop() is half of the property --
 * asking the store after the stop asks through a transport that was just torn
 * down, and restarting it does not bring the module back.
 *
 * So this half stops at the boundary: emit, stop, and report what the BUS
 * counted. test_bus_guardrail_durability.sh does the read-back straight out of
 * PostgreSQL, which is the only place a verification of "it reached the store"
 * can honestly live once the store is a separate process.
 *
 * Each event carries a unique identity (session_id "s<i>") and per-i field
 * values, so the SQL side can prove EXACTLY-ONCE rather than merely a matching
 * total -- a loss and a duplicate that net to N would pass a count and fail the
 * identity check -- and that every field survived the wire.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <aimee/audit/obs_bus.h>
#include "platform_test_util.h"
#include "server/obs_bus_adapter.h"
#include "support/store_module_fixture.h"

/* The default is 2000, which is what makes the exactly-once claim worth
 * something: a race that loses one event in a hundred is invisible at ten.
 * AIMEE_TEST_GUARDRAIL_N lowers it for diagnosis, where watching five events
 * cross is worth more than counting two thousand that did not. */
#define N_DEFAULT 2000

int main(void)
{
   /* LINE-BUFFERED, because this test can be killed. Piped or redirected, stdout
    * is block-buffered by default, so a run that times out loses every line it
    * printed -- which made three events look like total silence and sent the
    * diagnosis after a deadlock that was not there. A test whose output is
    * evidence has to emit that evidence as it goes. */
   setvbuf(stdout, NULL, _IOLBF, 0);

   int n = N_DEFAULT;
   {
      const char *override = getenv("AIMEE_TEST_GUARDRAIL_N");
      if (override && *override)
      {
         int parsed = atoi(override);
         if (parsed > 0)
            n = parsed;
      }
   }
   printf("test_bus_guardrail_durability: %d event(s)\n", n);

   char home[256];
   snprintf(home, sizeof home, "%s/aimee-busgr-XXXXXX", platform_tmpdir());
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);

   /* The sink goes on BEFORE the bus starts -- obs_bus refuses to be
    * reconfigured once running, and the fixture is what starts it. */
   if (server_obs_bus_configure() != 0)
   {
      fprintf(stderr, "FAIL: could not install the guardrail sink\n");
      return 1;
   }

   if (!store_module_fixture_available())
      return 0; /* skipped, with the reason printed by the fixture */
   store_module_fixture_start();

   for (int i = 0; i < n; i++)
   {
      guardrail_event_t e;
      memset(&e, 0, sizeof e);
      snprintf(e.session_id, sizeof e.session_id, "s%d", i);
      snprintf(e.tool_name, sizeof e.tool_name, "Tool_%d", i % 5);
      e.overall_risk = (double)i; /* exact for a double; verified back precisely */
      e.action_risk = 0.1;
      e.diff_risk = 0.2;
      e.drift_risk = 0.3;
      e.antipattern_similarity = 0.4;
      snprintf(e.recommendation, sizeof e.recommendation, "warn");
      snprintf(e.labels, sizeof e.labels, "lab-%d", i % 5);
      snprintf(e.final_action, sizeof e.final_action, (i % 2) ? "block" : "allow");
      snprintf(e.explanation, sizeof e.explanation, "expl %d", i);
      e.dry_run = i % 2;
      obs_bus_emit_guardrail(&e);
   }

   /* A settle window, off by default.
    *
    * With it at zero every event is still queued when the stop begins, which is
    * the harder half of the property and the one worth defaulting to. Setting
    * it lets a diagnosis separate "the sink never reaches the store" from "the
    * sink fails only during the stop" -- two failures that look identical from
    * the counters alone. */
   {
      const char *settle = getenv("AIMEE_TEST_GUARDRAIL_SETTLE_MS");
      int ms = settle && *settle ? atoi(settle) : 0;
      if (ms > 0)
      {
         struct timespec pause = {ms / 1000, (long)(ms % 1000) * 1000000L};
         nanosleep(&pause, NULL);
      }
   }

   /* The other half of the property: a graceful stop drains what is in flight.
    * Everything the bus accepted must be in the store by the time this
    * returns. */
   obs_bus_stop();

   uint64_t dropped = obs_bus_dropped();
   uint64_t written = obs_bus_written();
   printf("  emitted %d, written %llu, dropped %llu\n", n, (unsigned long long)written,
          (unsigned long long)dropped);

   /* Machine-readable, for the shell half. It compares the store's row count
    * against WRITTEN rather than against N, so a run where the bus legitimately
    * dropped something still checks the claim that matters: what the bus
    * accepted is what the store holds. */
   printf("EMITTED=%d WRITTEN=%llu DROPPED=%llu\n", n, (unsigned long long)written,
          (unsigned long long)dropped);

   if (dropped != 0 || written != (uint64_t)n)
   {
      fprintf(stderr, "FAIL: written %llu dropped %llu, expected %d written / 0 dropped\n",
              (unsigned long long)written, (unsigned long long)dropped, n);
      return 1;
   }
   printf("test_bus_guardrail_durability: emitter OK (the SQL half verifies the store)\n");
   return 0;
}
