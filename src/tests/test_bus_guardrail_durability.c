/* test_bus_guardrail_durability.c: the second module on the bus — the
 * guardrail-semantic risk event — carried exactly once to its db1 sink.
 *
 * gsem_record used to call db1_guardrail_event_insert directly; it now publishes
 * over the shared event bus, and the bus consumer performs the real insert. Same
 * all-or-nothing, load-bearing property as the audit row: every guardrail event
 * the bus accepts reaches db1 exactly once, and a graceful stop drains the
 * in-flight ones. This emits N events through the real bus, stops, and requires
 * the real db1 guardrail_events table to hold exactly N — proving the second
 * kind rides the same transport with the same guarantee.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audit_bus.h" /* audit_bus_*, guardrail_event_t, db1_guardrail_event_* */
#include "db1/db1.h"

#define N 2000

int main(void)
{
   printf("test_bus_guardrail_durability:\n");

   char home[] = "/tmp/aimee-busgr-XXXXXX";
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   if (db1_init(":memory:") != 0)
   {
      fprintf(stderr, "FAIL: db1 init\n");
      return 1;
   }

   assert(audit_bus_start() == 0);
   for (int i = 0; i < N; i++)
   {
      guardrail_event_t e;
      memset(&e, 0, sizeof e);
      snprintf(e.session_id, sizeof e.session_id, "sess-g");
      snprintf(e.tool_name, sizeof e.tool_name, "Tool_%d", i % 5);
      e.overall_risk = (double)(i % 100) / 100.0;
      e.action_risk = 0.1;
      e.diff_risk = 0.2;
      e.drift_risk = 0.3;
      e.antipattern_similarity = 0.4;
      snprintf(e.recommendation, sizeof e.recommendation, "warn");
      snprintf(e.labels, sizeof e.labels, "label-%d", i % 5);
      snprintf(e.final_action, sizeof e.final_action, (i % 2) ? "block" : "allow");
      snprintf(e.explanation, sizeof e.explanation, "explanation body %d", i);
      e.dry_run = i % 2;
      audit_bus_emit_guardrail(&e);
   }
   audit_bus_stop(); /* drains every in-flight event to db1 */

   uint64_t dropped = audit_bus_dropped();
   uint64_t written = audit_bus_written();
   printf("  emitted %d, written %llu, dropped %llu\n", N, (unsigned long long)written,
          (unsigned long long)dropped);
   if (dropped != 0 || written != (uint64_t)N)
   {
      fprintf(stderr, "FAIL: written %llu dropped %llu, expected %d written / 0 dropped\n",
              (unsigned long long)written, (unsigned long long)dropped, N);
      return 1;
   }

   /* The real db1 sink must hold exactly N rows. */
   guardrail_event_row_t *rows = calloc(N, sizeof *rows);
   assert(rows);
   int count = 0;
   if (db1_guardrail_event_list(N, 0, rows, &count) != 0)
   {
      fprintf(stderr, "FAIL: db1_guardrail_event_list failed\n");
      return 1;
   }
   if (count != N)
   {
      fprintf(stderr, "FAIL: db1 holds %d guardrail events, expected %d\n", count, N);
      return 1;
   }
   /* Spot-check a row round-tripped through the bus wire form intact. */
   int ok_shape = 0;
   for (int i = 0; i < count; i++)
      if (strcmp(rows[i].session_id, "sess-g") == 0 && strncmp(rows[i].tool_name, "Tool_", 5) == 0 &&
          (strcmp(rows[i].final_action, "block") == 0 || strcmp(rows[i].final_action, "allow") == 0))
         ok_shape++;
   if (ok_shape != N)
   {
      fprintf(stderr, "FAIL: %d/%d rows had the expected round-tripped shape\n", ok_shape, N);
      return 1;
   }
   printf("  db1 guardrail_events: %d rows, each round-tripped through the bus intact\n", count);

   free(rows);
   db1_shutdown();
   printf("test_bus_guardrail_durability: OK (a second module rides the bus, exactly once)\n");
   return 0;
}
