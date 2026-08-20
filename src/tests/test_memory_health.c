#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "db.h"
#include "db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"
#include "../modules/db2/c/db2_internal.h"
#include "../modules/db2/c/db_postgres.h"

/* Sum of a relation's edge weights — a cheap stand-in for "did anything change".
 * Weight is the only column normalize touches. */
static long long edge_weight_checksum(const char *relation)
{
   char qerr[128] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT COALESCE(SUM(weight), 0) FROM entity_edges WHERE relation = ?1", qerr,
       sizeof(qerr));
   assert(st);
   aimee_pg_bind_text(st, "?1", relation);
   assert(aimee_pg_step(st, qerr, sizeof(qerr)) == AIMEE_PG_ROW);
   long long v = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return v;
}

static int edge_weight_max(const char *relation)
{
   char qerr[128] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT COALESCE(MAX(weight), 0) FROM entity_edges WHERE relation = ?1", qerr,
       sizeof(qerr));
   assert(st);
   aimee_pg_bind_text(st, "?1", relation);
   assert(aimee_pg_step(st, qerr, sizeof(qerr)) == AIMEE_PG_ROW);
   int v = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return v;
}

/* Insert a semantic edge with an explicit §5 lifecycle state. Written straight to
 * SQL rather than through db2_fact_commit so the test can pin confidence_class,
 * weight and asserted_at exactly, without standing up the fact-gate provider. */
static void insert_semantic_edge(const char *source, const char *relation, const char *target,
                                 const char *cls, double confidence, int weight,
                                 const char *asserted_at)
{
   char qerr[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(),
                        "INSERT INTO entity_edges (source, relation, target, weight, edge_class,"
                        " confidence_class, confidence, asserted_at, superseded_at, suppressed)"
                        " VALUES (?1, ?2, ?3, ?4, 'semantic', ?5, ?6, ?7, '', 0)",
                        qerr, sizeof(qerr));
   assert(st);
   aimee_pg_bind_text(st, "?1", source);
   aimee_pg_bind_text(st, "?2", relation);
   aimee_pg_bind_text(st, "?3", target);
   aimee_pg_bind_int(st, "?4", weight);
   aimee_pg_bind_text(st, "?5", cls);
   aimee_pg_bind_double(st, "?6", confidence);
   aimee_pg_bind_text(st, "?7", asserted_at);
   assert(aimee_pg_step(st, qerr, sizeof(qerr)) == AIMEE_PG_DONE);
   aimee_pg_finalize(st);
}

/* (superseded_at, confidence) for a semantic edge, so the test can tell an
 * expired row from a live one and a durable row from an unconfirmed one. */
static void semantic_edge_state(const char *source, const char *relation, char *out_superseded,
                                size_t cap, double *out_confidence)
{
   char qerr[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(),
                                          "SELECT superseded_at, confidence FROM entity_edges"
                                          " WHERE source = ?1 AND relation = ?2"
                                          "   AND edge_class = 'semantic' LIMIT 1",
                                          qerr, sizeof(qerr));
   assert(st);
   aimee_pg_bind_text(st, "?1", source);
   aimee_pg_bind_text(st, "?2", relation);
   assert(aimee_pg_step(st, qerr, sizeof(qerr)) == AIMEE_PG_ROW);
   snprintf(out_superseded, cap, "%s", aimee_pg_column_text(st, 0));
   *out_confidence = aimee_pg_column_double(st, 1);
   aimee_pg_finalize(st);
}

int main(void)
{
   printf("memory_health: ");

   assert(db1_init(":memory:") == 0);
   db2_test_shim_open();

   /* --- memory_run_maintenance populates memory_health --- */
   {
      /* Insert some memories so maintenance has something to work with */
      memory_t m;
      memory_insert(TIER_L0, KIND_FACT, "test-key-1", "value 1", 0.5, "sess-1", &m);
      memory_insert(TIER_L1, KIND_FACT, "test-key-2", "value 2", 0.9, "sess-1", &m);
      memory_insert(TIER_L2, KIND_FACT, "test-key-3", "value 3", 1.0, "sess-1", &m);

      int promoted = 0, demoted = 0, expired = 0;
      memory_run_maintenance(&promoted, &demoted, &expired);

      /* Verify memory_health table has a row */
      char qerr[128] = "";
      aimee_pg_stmt_t *stmt =
          aimee_pg_prepare(db2_conn(), "SELECT COUNT(*) FROM memory_health", qerr, sizeof(qerr));
      assert(stmt);
      assert(aimee_pg_step(stmt, qerr, sizeof(qerr)) == AIMEE_PG_ROW);
      int count = aimee_pg_column_int(stmt, 0);
      assert(count >= 1);
      aimee_pg_finalize(stmt);
   }

   /* --- memory_query_health returns aggregated stats --- */
   {
      memory_health_t health;
      int rc = memory_query_health(&health);
      assert(rc == 0);
      assert(health.cycles >= 1);
      /* total_expirations should reflect the L0 we inserted (expired by maintenance) */
      assert(health.total_expirations >= 0);
   }

   /* --- Run multiple maintenance cycles --- */
   {
      memory_t m;
      memory_insert(TIER_L1, KIND_FACT, "multi-cycle-1", "data", 0.95, "sess-2", &m);

      int p, d, e;
      memory_run_maintenance(&p, &d, &e);
      memory_run_maintenance(&p, &d, &e);

      memory_health_t health;
      memory_query_health(&health);
      assert(health.cycles >= 3);
   }

   /* --- memory_record_conflict writes to contradiction_log --- */
   {
      memory_t m1, m2;
      memory_insert(TIER_L1, KIND_FACT, "conflict-a", "always use X", 1.0, "", &m1);
      memory_insert(TIER_L1, KIND_FACT, "conflict-b", "never use X", 1.0, "", &m2);

      memory_record_conflict(m1.id, m2.id);

      /* Verify contradiction_log has a row */
      char qerr[128] = "";
      aimee_pg_stmt_t *stmt = aimee_pg_prepare(db2_conn(), "SELECT COUNT(*) FROM contradiction_log",
                                               qerr, sizeof(qerr));
      assert(stmt);
      assert(aimee_pg_step(stmt, qerr, sizeof(qerr)) == AIMEE_PG_ROW);
      int count = aimee_pg_column_int(stmt, 0);
      assert(count >= 1);
      aimee_pg_finalize(stmt);

      /* Verify the log entry has correct IDs */
      stmt = aimee_pg_prepare(db2_conn(),
                              "SELECT memory_a_id, memory_b_id, resolution"
                              " FROM contradiction_log ORDER BY id DESC LIMIT 1",
                              qerr, sizeof(qerr));
      assert(stmt);
      assert(aimee_pg_step(stmt, qerr, sizeof(qerr)) == AIMEE_PG_ROW);
      int64_t a = aimee_pg_column_int64(stmt, 0);
      int64_t b = aimee_pg_column_int64(stmt, 1);
      const char *res = aimee_pg_column_text(stmt, 2);
      assert(a == m1.id);
      assert(b == m2.id);
      assert(strcmp(res, "pending") == 0);
      aimee_pg_finalize(stmt);
   }

   /* --- memory_resolve_conflict also logs resolution --- */
   {
      conflict_t conflicts[8];
      int count = memory_list_conflicts(conflicts, 8);
      assert(count >= 1);

      memory_resolve_conflict(conflicts[0].id, "a_decayed");

      /* Verify resolution logged */
      char qerr[128] = "";
      aimee_pg_stmt_t *stmt = aimee_pg_prepare(db2_conn(),
                                               "SELECT resolution FROM contradiction_log"
                                               " ORDER BY id DESC LIMIT 1",
                                               qerr, sizeof(qerr));
      assert(stmt);
      assert(aimee_pg_step(stmt, qerr, sizeof(qerr)) == AIMEE_PG_ROW);
      const char *res = aimee_pg_column_text(stmt, 0);
      assert(strcmp(res, "a_decayed") == 0);
      aimee_pg_finalize(stmt);
   }

   /* --- staleness calculation --- */
   {
      /* The L2 memory we inserted earlier should show up in staleness if untouched */
      memory_health_t health;
      memory_query_health(&health);
      /* staleness should be between 0 and 1 */
      assert(health.staleness >= 0.0 && health.staleness <= 1.0);
   }

   /* --- effectiveness uses DB1 server_sessions outcomes without cross-DB join --- */
   {
      memory_t m;
      memory_insert(TIER_L1, KIND_FACT, "effectiveness-db1", "value", 0.8, "sess-eff", &m);

      for (int i = 0; i < EFFECTIVENESS_MIN_SAMPLES; i++)
      {
         char sid[32];
         snprintf(sid, sizeof(sid), "eff-session-%d", i);
         assert(db1_server_session_create(sid, "cli", "tester") == 0);
         assert(db1_context_snapshot_insert(sid, m.id, 1.0) == 0);
         assert(db1_server_session_set_outcome(sid, i < 7 ? "success" : "failure") == 0);
      }

      assert(memory_compute_effectiveness() >= 1);

      char qerr[128] = "";
      aimee_pg_stmt_t *stmt = aimee_pg_prepare(
          db2_conn(), "SELECT effectiveness FROM memories WHERE id = ?1", qerr, sizeof(qerr));
      assert(stmt);
      aimee_pg_bind_int64(stmt, "?1", m.id);
      assert(aimee_pg_step(stmt, qerr, sizeof(qerr)) == AIMEE_PG_ROW);
      double effectiveness = aimee_pg_column_double(stmt, 0);
      assert(fabs(effectiveness - (8.0 / 12.0)) < 0.000001);
      aimee_pg_finalize(stmt);
   }

   /* --- never_surfaced_l2 counts DB2 memories absent from DB1 context_snapshots --- */
   {
      memory_t surfaced, unsurfaced;
      effectiveness_stats_t stats;

      memory_insert(TIER_L2, KIND_FACT, "surfaced-l2", "value", 0.8, "sess-a", &surfaced);
      memory_insert(TIER_L2, KIND_FACT, "unsurfaced-l2", "value", 0.8, "sess-b", &unsurfaced);
      assert(db1_context_snapshot_insert("surfaced-session", surfaced.id, 0.9) == 0);

      assert(memory_effectiveness_stats(&stats) == 0);
      assert(stats.never_surfaced_l2 >= 1);
   }

   /* --- memory_run_maintenance normalizes entity edge weights ---
    *
    * Regression guard for a WIRING bug, not for the SQL:
    * db2_entity_edge_normalize_weights() and its memory_graph_normalize()
    * wrapper were both fully implemented, but nothing ever called them — the
    * maintenance cycle ran its sibling memory_graph_prune() and stopped there,
    * so per-relation weights were never rescaled in a running system. Assert
    * through memory_run_maintenance() rather than calling normalize directly:
    * the missing call WAS the defect, so a direct-call test would have passed
    * against the broken tree and proved nothing.
    *
    * One edge for this relation, deliberately. The pass divides by a correlated
    * (SELECT MAX(weight) ... WHERE relation = ...), and postgres (production)
    * evaluates that against the statement-start snapshot while this suite's
    * shim (db2_test_shim = sqlite) re-evaluates it per row and sees its own
    * writes. With two edges the two backends disagree — 2,4 normalizes to 50,100
    * on postgres but 50,8 under the shim, since updating the first row raises the
    * max the second row divides by. A single edge has nothing to interfere with,
    * so it pins to 100 on both and the guard tests the wiring rather than the
    * shim's UPDATE semantics. */
   {
      char qerr[128] = "";
      memory_t anchor;

      /* Anchor the edge to an L1 memory. memory_graph_prune() runs FIRST and
       * deletes any edge where neither endpoint appears in an L1/L2 memory, so
       * an unanchored fixture would be gone before normalize ever saw it. */
      memory_insert(TIER_L1, KIND_FACT, "norm-anchor", "anchor for edge weights", 0.9, "sess-n",
                    &anchor);

      aimee_pg_stmt_t *ins =
          aimee_pg_prepare(db2_conn(),
                           "INSERT INTO entity_edges (source, relation, target, weight) VALUES "
                           "('norm-anchor', 'rel-norm', 'norm-anchor', 5)",
                           qerr, sizeof(qerr));
      assert(ins);
      assert(aimee_pg_step(ins, qerr, sizeof(qerr)) == AIMEE_PG_DONE);
      aimee_pg_finalize(ins);

      int promoted = 0, demoted = 0, expired = 0;
      memory_run_maintenance(&promoted, &demoted, &expired);

      /* Sole edge for the relation, so it IS the per-relation max: 5 * 100 / 5. */
      aimee_pg_stmt_t *stmt = aimee_pg_prepare(db2_conn(),
                                               "SELECT COUNT(*), MAX(weight) FROM entity_edges "
                                               "WHERE relation = 'rel-norm'",
                                               qerr, sizeof(qerr));
      assert(stmt);
      assert(aimee_pg_step(stmt, qerr, sizeof(qerr)) == AIMEE_PG_ROW);
      int rows = aimee_pg_column_int(stmt, 0);
      int max_w = aimee_pg_column_int(stmt, 1);
      aimee_pg_finalize(stmt);
      assert(rows == 1); /* the prune must not have eaten the fixture */
      assert(max_w == 100);
   }

   /* --- normalize converges and then stops writing ---
    *
    * The property both backends must satisfy, tested without depending on either
    * one's UPDATE semantics. postgres divides by a statement-start snapshot of
    * MAX(weight); the sqlite shim re-evaluates it per row and sees its own
    * writes, so the per-run arithmetic legitimately differs (2,4 -> 50,100 on
    * postgres; 50,8 then 100,8 under the shim). What must hold everywhere is the
    * fixpoint: the per-relation maximum ends at 100, and once converged a further
    * maintenance cycle changes nothing.
    *
    * The second half is the one that matters operationally. normalize now runs on
    * EVERY maintenance cycle, so rewriting rows whose value is already correct
    * would burn postgres WAL and bump updated_at forever on an idle graph. It did
    * exactly that (measured: 2 of 2 rows on a converged graph) until the pass
    * learned to skip converged rows. Asserted on the ROWS-UPDATED count, not on a
    * checksum: rewriting a row to the value it already holds is a real write that
    * no value comparison can see. */
   {
      char qerr[128] = "";
      memory_t anchor;
      memory_insert(TIER_L1, KIND_FACT, "conv-anchor", "anchor for convergence", 0.9, "sess-c",
                    &anchor);

      aimee_pg_stmt_t *ins =
          aimee_pg_prepare(db2_conn(),
                           "INSERT INTO entity_edges (source, relation, target, weight) VALUES "
                           "('conv-anchor', 'rel-conv', 'conv-anchor', 3), "
                           "('conv-anchor', 'rel-conv', 'conv-anchor', 6), "
                           "('conv-anchor', 'rel-conv', 'conv-anchor', 9)",
                           qerr, sizeof(qerr));
      assert(ins);
      assert(aimee_pg_step(ins, qerr, sizeof(qerr)) == AIMEE_PG_DONE);
      aimee_pg_finalize(ins);

      /* Drive to the fixpoint. Three cycles is enough for either backend. */
      int p_ = 0, d_ = 0, e_ = 0;
      for (int i = 0; i < 3; i++)
         memory_run_maintenance(&p_, &d_, &e_);

      long long before = edge_weight_checksum("rel-conv");
      int max_w = edge_weight_max("rel-conv");
      assert(max_w == 100); /* converged: the per-relation max is normalized */

      /* Converged: the pass must now touch nothing at all. */
      assert(memory_graph_normalize() == 0); /* zero rows rewritten, not "same values" */

      memory_run_maintenance(&p_, &d_, &e_);
      long long after = edge_weight_checksum("rel-conv");
      assert(after == before); /* and the values stay put */
   }

   /* --- quiet-lane alarm: the rule, not the plumbing ---
    *
    * A maintenance cycle that produces nothing is indistinguishable from a
    * healthy idle one unless something asks whether work was waiting. These pin
    * both sides of that question, because an alarm that fires on a healthy idle
    * system is worse than no alarm -- operators learn to ignore it. */
   {
      /* Output, backlog or not: never an alarm. */
      assert(memory_quiet_lane_alarm(1, 0, 99) == 0);
      assert(memory_quiet_lane_alarm(5, 500, 99) == 0);

      /* Quiet with an EMPTY backlog is a healthy idle system, however long it
       * lasts. This is the case that must stay silent. */
      assert(memory_quiet_lane_alarm(0, 0, 1) == 0);
      assert(memory_quiet_lane_alarm(0, 0, 1000) == 0);

      /* Quiet WITH a backlog: not yet an alarm -- one or two idle cycles are
       * normal (rate limits, nothing eligible this pass). */
      assert(memory_quiet_lane_alarm(0, 10, 1) == 0);
      assert(memory_quiet_lane_alarm(0, 10, 2) == 0);

      /* Sustained silence while work waits is the fault. */
      assert(memory_quiet_lane_alarm(0, 10, 3) == 1);
      assert(memory_quiet_lane_alarm(0, 1, 4) == 1);

      /* A negative/absent backlog reading must not alarm: an unknown count is
       * not evidence of a wedged lane. */
      assert(memory_quiet_lane_alarm(0, -1, 99) == 0);

      /* The run counter starts clean and is readable. */
      assert(memory_quiet_cycles() >= 0);
      printf("quiet_lane_alarm OK ");
   }

   /* --- §5 fact-class lifecycle runs as part of the maintenance cycle ---
    *
    * db2_fact_expire_speculative and db2_fact_promote_durable were written and
    * unit-tested but never scheduled, so neither ever ran in production: Class C
    * speculation accumulated forever and Class B never earned durability. These
    * assertions go through memory_run_maintenance rather than calling the jobs
    * directly, because "the job works" was already true — what was missing, and
    * what this pins, is that the cycle invokes it. */
   {
      /* Unconfirmed (weight 1) Class C, asserted long before any sane TTL. */
      insert_semantic_edge("lifecycle-c", "frobnicates", "thing", "C", 0.4, 1,
                           "2000-01-01 00:00:00");
      /* Confirmed (weight 2) Class C: repeatedly observed, so it must survive. */
      insert_semantic_edge("lifecycle-c-confirmed", "wibbles", "y", "C", 0.4, 2,
                           "2000-01-01 00:00:00");
      /* Class B confirmed well past the default threshold of 3. */
      insert_semantic_edge("lifecycle-b", "works_for", "ecorp", "B", 0.6, 5, "2000-01-01 00:00:00");

      int p_ = 0, d_ = 0, e_ = 0;
      memory_run_maintenance(&p_, &d_, &e_);

      char superseded[64] = "";
      double confidence = 0.0;

      semantic_edge_state("lifecycle-c", "frobnicates", superseded, sizeof(superseded),
                          &confidence);
      assert(superseded[0] != '\0'); /* aged-out speculation was stamped */

      semantic_edge_state("lifecycle-c-confirmed", "wibbles", superseded, sizeof(superseded),
                          &confidence);
      assert(superseded[0] == '\0'); /* confirmed C is not speculation any more */

      semantic_edge_state("lifecycle-b", "works_for", superseded, sizeof(superseded), &confidence);
      assert(superseded[0] == '\0'); /* promotion must not expire anything */
      assert(confidence == 0.8);     /* B became durable */

      /* Both jobs report through the cycle's own counters, so a run that only
       * moved typed facts is not mistaken for a quiet lane. */
      assert(p_ >= 1);
      assert(e_ >= 1);
      printf("fact_lifecycle_scheduled OK ");

      /* Orphan pruning must not reach typed facts. None of the entities above
       * appears in any L1/L2 memory, so every one of these rows is an "orphan" by
       * the co-occurrence rule — and a maintenance cycle just ran. If the prune
       * treated them as co-occurrence edges they would have been DELETEd rather
       * than stamped, and the reads above would have found nothing at all. Assert
       * the rows are still present, including the one expiry superseded: a
       * superseded fact is archived, not erased. */
      char qerr[128] = "";
      aimee_pg_stmt_t *st =
          aimee_pg_prepare(db2_conn(),
                           "SELECT COUNT(*) FROM entity_edges WHERE edge_class = 'semantic'"
                           " AND source LIKE 'lifecycle-%'",
                           qerr, sizeof(qerr));
      assert(st);
      assert(aimee_pg_step(st, qerr, sizeof(qerr)) == AIMEE_PG_ROW);
      assert(aimee_pg_column_int(st, 0) == 3);
      aimee_pg_finalize(st);
      printf("semantic_survives_prune OK ");

      /* Weight normalisation must not touch typed facts either. The same cycle
       * that runs the lifecycle jobs also rescales edge weights per relation so
       * the maximum is 100 -- which is meaningful for a co-occurrence tally and
       * destructive for a semantic edge, where weight IS the confirmation count
       * the §5 thresholds read (promote_durable weight>=threshold,
       * expire_speculative weight<=1).
       *
       * Give 'works_for' a co-occurrence edge heavy enough to set the scale.
       * Before the fix, lifecycle-b (weight 5) was rescaled and the Class A row
       * went 1 -> 20, which on the next cycle is "confirmed 20 times". */
      insert_semantic_edge("norm-a", "works_for", "norm-corp", "A", 1.0, 1, "2026-01-01 00:00:00");
      {
         char qerr2[128] = "";
         aimee_pg_stmt_t *ins = aimee_pg_prepare(
             db2_conn(),
             "INSERT INTO entity_edges (source, relation, target, weight, edge_class)"
             " VALUES ('norm-cooccur', 'works_for', 'norm-other', 50, 'cooccurrence')",
             qerr2, sizeof(qerr2));
         assert(ins);
         assert(aimee_pg_step(ins, qerr2, sizeof(qerr2)) == AIMEE_PG_DONE);
         aimee_pg_finalize(ins);
      }

      int p2 = 0, d2 = 0, e2 = 0;
      memory_run_maintenance(&p2, &d2, &e2);

      /* The typed facts keep their real counts; only the co-occurrence edge is
       * rescaled. Asserting the exact values, because "unchanged" is the whole
       * point -- an off-by-scaling here silently re-dates every fact's
       * confirmation history. */
      char qerr3[128] = "";
      aimee_pg_stmt_t *w = aimee_pg_prepare(db2_conn(),
                                            "SELECT weight FROM entity_edges"
                                            " WHERE source = ?1 AND relation = 'works_for'",
                                            qerr3, sizeof(qerr3));
      assert(w);
      aimee_pg_bind_text(w, "?1", "norm-a");
      assert(aimee_pg_step(w, qerr3, sizeof(qerr3)) == AIMEE_PG_ROW);
      assert(aimee_pg_column_int(w, 0) == 1); /* Class A still asserted once */
      aimee_pg_finalize(w);

      semantic_edge_state("lifecycle-b", "works_for", superseded, sizeof(superseded), &confidence);
      w = aimee_pg_prepare(db2_conn(),
                           "SELECT weight FROM entity_edges WHERE source = 'lifecycle-b'", qerr3,
                           sizeof(qerr3));
      assert(w);
      assert(aimee_pg_step(w, qerr3, sizeof(qerr3)) == AIMEE_PG_ROW);
      assert(aimee_pg_column_int(w, 0) == 5); /* still five confirmations, not 100 */
      aimee_pg_finalize(w);
      printf("semantic_weight_not_normalised OK ");
   }

   /* --- a co-occurrence observation must not bump a typed fact's weight ---
    *
    * The unique index is on the bare (source, relation, target) triple, so a
    * semantic row and a co-occurrence row for one triple cannot coexist and the
    * upsert's ON CONFLICT lands on whichever is there. Without a guard, "these
    * two words appeared in the same session" increments the confirmation count
    * that §5 uses to grant durability. */
   {
      insert_semantic_edge("share-src", "works_for", "share-tgt", "B", 0.6, 1,
                           "2026-01-01 00:00:00");
      int added = -1;
      /* Same triple, via the co-occurrence writer. */
      assert(db2_entity_edge_upsert("share-src", "works_for", "share-tgt", 0, 0, 0, 0, &added) ==
             0);

      char qerr[128] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(),
                                             "SELECT weight, edge_class FROM entity_edges"
                                             " WHERE source = 'share-src'",
                                             qerr, sizeof(qerr));
      assert(st);
      assert(aimee_pg_step(st, qerr, sizeof(qerr)) == AIMEE_PG_ROW);
      assert(aimee_pg_column_int(st, 0) == 1); /* NOT bumped to 2 */
      assert(strcmp(aimee_pg_column_text(st, 1), "semantic") == 0);
      aimee_pg_finalize(st);
      printf("cooccurrence_does_not_bump_fact OK ");
   }

   db1_shutdown();
   db2_test_shim_close();

   printf("all tests passed\n");
   return 0;
}
