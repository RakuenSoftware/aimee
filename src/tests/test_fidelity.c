/* test_fidelity.c: auditable-correctness P3 fidelity storage substrate over the
 * sqlite shim — answer-level reports + per-chunk attributions, and the structural
 * guarantee that neither is a scored (demotion) artifact. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "../db2/fidelity.h"

static int64_t count_kind(void *conn, const char *kind)
{
   char e[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT COUNT(*) FROM artifacts WHERE kind = ?1", e, sizeof e);
   assert(st);
   aimee_pg_bind_text(st, "?1", kind);
   int64_t n = 0;
   if (aimee_pg_step(st, e, sizeof e) == AIMEE_PG_ROW)
      n = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int main(void)
{
   db2_test_shim_open();
   void *conn = db2_conn();

   /* write + read an answer-level report */
   char status[32] = "";
   int sup = -1, uns = -1, abst = -1;
   assert(db2_fidelity_report_write("t1", "ok", 3, 1, 2) == 0);
   assert(db2_fidelity_report_by_turn("t1", status, sizeof status, &sup, &uns, &abst) == 1);
   assert(strcmp(status, "ok") == 0 && sup == 3 && uns == 1 && abst == 2);
   printf("  report write/read OK (status=%s sup=%d uns=%d abst=%d)\n", status, sup, uns, abst);

   /* re-judge: a second report for the same turn REPLACES the prior one (upsert) */
   assert(db2_fidelity_report_write("t1", "ok", 5, 0, 0) == 0);
   assert(db2_fidelity_report_by_turn("t1", status, sizeof status, &sup, &uns, &abst) == 1);
   assert(sup == 5 && uns == 0 && abst == 0);
   printf("  re-judge replaces prior report OK (sup=%d)\n", sup);

   /* not_evaluated bucket (honest denominator on a tool-loop turn) */
   assert(db2_fidelity_report_write("t2", "not_evaluated", 0, 0, 0) == 0);
   status[0] = '\0';
   assert(db2_fidelity_report_by_turn("t2", status, sizeof status, NULL, NULL, NULL) == 1);
   assert(strcmp(status, "not_evaluated") == 0);

   /* missing turn → 0 (not an error) */
   assert(db2_fidelity_report_by_turn("nope", NULL, 0, NULL, NULL, NULL) == 0);

   /* invalid status / verdict / empty turn_id are all rejected (-1), write nothing */
   assert(db2_fidelity_report_write("tx", "bogus", 0, 0, 0) == -1);
   assert(db2_fidelity_report_by_turn("tx", NULL, 0, NULL, NULL, NULL) == 0);
   assert(db2_fidelity_report_write("", "ok", 0, 0, 0) == -1);
   assert(db2_fidelity_attribution_write("", 1, "accepted") == -1);
   assert(db2_fidelity_attribution_write("t1", 1, "maybe") == -1);
   printf("  bad status/verdict/empty-turn rejected OK\n");

   /* per-chunk attribution: stored as a non-scored kind with the judge operator,
    * and readable by turn via the count reader */
   assert(db2_fidelity_attribution_write("t1", 42, "accepted") == 0);
   assert(db2_fidelity_attribution_write("t1", 99, "irrelevant") == 0);
   assert(db2_fidelity_attribution_count_by_turn("t1") == 2);
   assert(db2_fidelity_attribution_count_by_turn("nope") == 0);
   {
      char e[256] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                             "SELECT COUNT(*) FROM artifacts WHERE"
                                             " kind = 'fidelity_attribution'"
                                             " AND operator_id = 'fidelity-judge'",
                                             e, sizeof e);
      assert(st);
      assert(aimee_pg_step(st, e, sizeof e) == AIMEE_PG_ROW);
      assert(aimee_pg_column_int64(st, 0) == 2);
      aimee_pg_finalize(st);
   }

   /* DEMOTION-INERTNESS: fidelity rows exist, but NONE is a 'retrieval_attribution'
    * (the only kind db2_demotion_score reads), so fidelity can never shift a
    * demotion percentile. */
   assert(count_kind(conn, "fidelity_report") == 2); /* t1 (upserted to 1) + t2; t3 not yet */
   assert(count_kind(conn, "fidelity_attribution") == 2);
   assert(count_kind(conn, "retrieval_attribution") == 0);
   printf("  attributions stored non-scored (fidelity-judge), demotion-inert OK\n");

   /* malformed payload on a present row → -1 (distinct from a real all-zeros report) */
   {
      char e[256] = "";
      assert(aimee_pg_exec(conn,
                           "INSERT INTO artifacts (id, kind, turn_id, payload)"
                           " VALUES ('bad1','fidelity_report','tbad','{not json')",
                           e, sizeof e) == 0);
      assert(db2_fidelity_report_by_turn("tbad", NULL, 0, NULL, NULL, NULL) == -1);
      printf("  malformed report payload returns -1 OK\n");
   }

   /* COEXISTENCE: a fidelity_report shares turn_id 't3' with a retrieval_event
    * without a unique-index collision (the partial index covers retrieval_event
    * only). */
   {
      char e[256] = "";
      assert(aimee_pg_exec(conn,
                           "INSERT INTO artifacts (id, kind, turn_id, payload)"
                           " VALUES ('re3','retrieval_event','t3','{}')",
                           e, sizeof e) == 0);
      assert(db2_fidelity_report_write("t3", "ok", 1, 0, 0) == 0);
      assert(db2_fidelity_report_by_turn("t3", NULL, 0, &sup, NULL, NULL) == 1 && sup == 1);
      printf("  fidelity_report coexists with retrieval_event on same turn_id OK\n");
   }

   /* (RE)JUDGE BOUNDARY: writing a report for a turn clears that turn's prior
    * attributions, so a re-judge never leaves stale per-chunk verdicts behind. */
   assert(db2_fidelity_report_write("t5", "ok", 1, 0, 0) == 0);
   assert(db2_fidelity_attribution_write("t5", 7, "accepted") == 0);
   assert(db2_fidelity_attribution_count_by_turn("t5") == 1);
   assert(db2_fidelity_report_write("t5", "ok", 2, 0, 0) == 0); /* re-judge */
   assert(db2_fidelity_attribution_count_by_turn("t5") == 0);   /* prior attribution cleared */
   printf("  re-judge clears prior per-chunk attributions OK\n");

   db2_test_shim_close();
   printf("fidelity: all tests passed\n");
   return 0;
}
