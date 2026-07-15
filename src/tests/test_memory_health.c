#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "db.h"
#include "db1.h"
#include "db2.h"
#include "db2_test_shim.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"

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

   db1_shutdown();
   db2_test_shim_close();

   printf("all tests passed\n");
   return 0;
}
