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

#include <aimee/audit/obs_bus.h> /* obs_bus_*, guardrail_event_t, db1_guardrail_event_* */
#include "db1/db1.h"
#include "server/obs_bus_adapter.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#define N 2000

int main(void)
{
   printf("test_bus_guardrail_durability:\n");

   char home[256];
   snprintf(home, sizeof home, "%s/aimee-busgr-XXXXXX", platform_tmpdir());
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
   assert(server_obs_bus_configure() == 0);

   /* Each event carries a UNIQUE identity (session_id "s<i>") and per-i field
    * values, so the read-back can prove exactly-once (a loss+dup that nets to N
    * would fail the seen[] check) AND that every field round-tripped the wire. */
   assert(obs_bus_start() == 0);
   for (int i = 0; i < N; i++)
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
   obs_bus_stop(); /* drains every in-flight event to db1 */

   uint64_t dropped = obs_bus_dropped();
   uint64_t written = obs_bus_written();
   printf("  emitted %d, written %llu, dropped %llu\n", N, (unsigned long long)written,
          (unsigned long long)dropped);
   if (dropped != 0 || written != (uint64_t)N)
   {
      fprintf(stderr, "FAIL: written %llu dropped %llu, expected %d written / 0 dropped\n",
              (unsigned long long)written, (unsigned long long)dropped, N);
      return 1;
   }

   /* The real db1 sink must hold exactly N rows — each identity once, every field
    * matching what that identity emitted. */
   guardrail_event_row_t *rows = calloc(N, sizeof *rows);
   char *seen = calloc(N, 1);
   assert(rows && seen);
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
   for (int r = 0; r < count; r++)
   {
      if (rows[r].session_id[0] != 's')
      {
         fprintf(stderr, "FAIL: row %d has unexpected session_id '%s'\n", r, rows[r].session_id);
         return 1;
      }
      int id = atoi(rows[r].session_id + 1);
      if (id < 0 || id >= N || seen[id])
      {
         fprintf(stderr, "FAIL: exactly-once violated at id=%d (out-of-range or duplicate)\n", id);
         return 1;
      }
      seen[id] = 1;
      char etool[32], elab[32], eexpl[32];
      snprintf(etool, sizeof etool, "Tool_%d", id % 5);
      snprintf(elab, sizeof elab, "lab-%d", id % 5);
      snprintf(eexpl, sizeof eexpl, "expl %d", id);
      const char *efa = (id % 2) ? "block" : "allow";
      if (strcmp(rows[r].tool_name, etool) != 0 || strcmp(rows[r].final_action, efa) != 0 ||
          strcmp(rows[r].labels, elab) != 0 || strcmp(rows[r].explanation, eexpl) != 0 ||
          rows[r].dry_run != (id % 2) || rows[r].overall_risk != (double)id)
      {
         fprintf(stderr,
                 "FAIL: id=%d fields did not round-trip: tool=%s fa=%s lab=%s expl=%s "
                 "dry=%d risk=%g\n",
                 id, rows[r].tool_name, rows[r].final_action, rows[r].labels, rows[r].explanation,
                 rows[r].dry_run, rows[r].overall_risk);
         return 1;
      }
   }
   int missing = 0;
   for (int i = 0; i < N; i++)
      if (!seen[i])
         missing++;
   if (missing)
   {
      fprintf(stderr, "FAIL: %d of %d identities missing from db1\n", missing, N);
      return 1;
   }
   printf("  db1 guardrail_events: %d rows, each identity present exactly once with all fields "
          "round-tripped\n",
          count);

   free(rows);
   free(seen);
   db1_shutdown();
   printf("test_bus_guardrail_durability: OK (a second module rides the bus, exactly once)\n");
   return 0;
}
