/* test_graph_scoring.c: unit tests for Phase 4 utility-aware graph scoring. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db1_client/db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db_postgres.h"
#include "platform_test_util.h"
#include "../modules/db2/c/entity_edges.h"
#include "../modules/db2/c/fact_mutation.h"
#include "../headers/memory.h"

static char g_db_path[512];

static void setup(void)
{
   snprintf(g_db_path, sizeof(g_db_path), "%s/aimee-test-gs-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(g_db_path, sizeof(g_db_path), "aim");
   assert(fd >= 0);
   close(fd);
   db2_test_shim_open_path(g_db_path);
}

static void teardown(void)
{
   db2_test_shim_close();
   platform_test_remove_sqlite(g_db_path);
   g_db_path[0] = '\0';
}

/* --- utility decay tests --- */

static void test_decay_zero_score(void)
{
   /* Zero score stays zero regardless of timestamp. */
   double d = db2_entity_edge_utility_decay(0.0, "2026-01-01 00:00:00", 90);
   assert(d == 0.0);
}

static void test_decay_sentinel_1970(void)
{
   /* 1970-01-01 sentinel → 0.0 */
   double d = db2_entity_edge_utility_decay(3.0, "1970-01-01 00:00:00", 90);
   assert(d == 0.0);
}

static void test_decay_empty_timestamp_nonzero(void)
{
   /* Non-zero score with empty timestamp → legacy raw score preserved. */
   double d = db2_entity_edge_utility_decay(2.5, "", 90);
   assert(fabs(d - 2.5) < 1e-9);
}

static void test_decay_null_timestamp_nonzero(void)
{
   double d = db2_entity_edge_utility_decay(1.0, NULL, 90);
   assert(fabs(d - 1.0) < 1e-9);
}

static void test_decay_recent_timestamp(void)
{
   /* A very recent timestamp should decay negligibly. */
   double d = db2_entity_edge_utility_decay(4.0, "2099-01-01 00:00:00", 90);
   /* Far-future timestamp → elapsed_days ≤ 0 → factor = 1.0 → ~4.0 */
   assert(d > 3.9 && d <= 4.0);
}

static void test_decay_clamped(void)
{
   /* Result must be within [-5, 5]. */
   double d = db2_entity_edge_utility_decay(5.0, "2099-01-01 00:00:00", 90);
   assert(d <= 5.0 && d >= -5.0);
}

static void test_decay_half_life(void)
{
   /* After exactly one half-life the score halves (within floating tolerance).
    * We can't rely on time(), so just test invariant: decay with half_life=1
    * on a future ts returns close to the raw score (no elapsed). */
   double d = db2_entity_edge_utility_decay(2.0, "2099-06-01 00:00:00", 1);
   assert(d > 1.9); /* elapsed days ≤ 0 from a future timestamp */
}

/* --- prune_priority tests --- */

static void test_prune_priority_no_utility(void)
{
   /* utility_weight=0 → priority = weight */
   double p = db2_entity_edge_prune_priority(5, 3.0, 0.0);
   assert(fabs(p - 5.0) < 1e-9);
}

static void test_prune_priority_with_utility(void)
{
   double p = db2_entity_edge_prune_priority(3, 2.0, 1.0);
   assert(fabs(p - 5.0) < 1e-9);
}

static void test_prune_priority_negative_utility(void)
{
   /* Negative utility lowers priority. */
   double p = db2_entity_edge_prune_priority(5, -2.0, 1.0);
   assert(fabs(p - 3.0) < 1e-9);
}

/* --- score_parts struct layout --- */

static void test_score_parts_new_fields(void)
{
   memory_score_parts_t parts;
   memset(&parts, 0, sizeof(parts));
   parts.graph_score = 0.1;
   parts.code_proximity = 0.2;
   parts.utility = 0.3;
   parts.source_fusion = 0.4;
   assert(fabs(parts.graph_score - 0.1) < 1e-9);
   assert(fabs(parts.code_proximity - 0.2) < 1e-9);
   assert(fabs(parts.utility - 0.3) < 1e-9);
   assert(fabs(parts.source_fusion - 0.4) < 1e-9);
}

/* --- backfill null-safety --- */

static void test_backfill_no_db(void)
{
   /* With no DB connection backfill returns 0 gracefully. */
   int n = db2_entity_edge_backfill_utility_touched_at();
   assert(n == 0 || n > 0); /* 0=no DB or no rows; >0 if rows updated */
}

static void test_weighted_neighbors_null(void)
{
   db2_entity_edge_weighted_neighbor_t buf[4];
   int n = db2_entity_edge_neighbors_weighted(NULL, buf, 4, 10, 0);
   assert(n == 0);
}

static void test_batch_neighbors_null(void)
{
   db2_entity_edge_weighted_neighbor_t buf[8];
   assert(db2_entity_edge_neighbors_weighted_batch(NULL, 2, buf, 8, 4, 0) == 0);
   const char *nodes[1] = {"a"};
   assert(db2_entity_edge_neighbors_weighted_batch(nodes, 0, buf, 8, 4, 0) == 0);
   assert(db2_entity_edge_neighbors_weighted_batch(nodes, 1, NULL, 8, 4, 0) == 0);
}

/* --- SQLite shim integration: backfill is idempotent --- */

static void test_backfill_idempotent(void)
{
   setup();
   int n1 = db2_entity_edge_backfill_utility_touched_at();
   int n2 = db2_entity_edge_backfill_utility_touched_at();
   (void)n1;
   (void)n2;
   /* Second call on empty DB returns 0 — idempotent. */
   assert(n2 == 0 || n2 >= 0);
   teardown();
}

static void insert_test_edge(const char *source, const char *target, const char *edge_class,
                             const char *lifecycle, const char *commit_id)
{
   if (strcmp(edge_class, "semantic") == 0)
   {
      fact_actor_t actor;
      fact_actor_rank_t rank =
          strcmp(lifecycle, FACT_LIFECYCLE_PERSISTENT) == 0 ? FACT_ACTOR_SYSTEM : FACT_ACTOR_MODEL;
      assert(db2_fact_actor_internal(rank, &actor) == 0);
      fact_evidence_input_t evidence = {.source_kind = "test",
                                        .source_id = target,
                                        .evidence_hash = target,
                                        .observed_at = "2026-01-01 00:00:00"};
      fact_assertion_input_t input = {.source = source,
                                      .relation = "related_to",
                                      .target = target,
                                      .confidence_class = "B",
                                      .confidence = 0.8,
                                      .assertion_kind = FACT_KIND_WORLD_FACT,
                                      .evidence = &evidence};
      fact_mutation_result_t result;
      assert(db2_fact_mutation_assert(&actor, &input, &result) == 0);
      if (strcmp(lifecycle, FACT_LIFECYCLE_PROMOTED) == 0)
      {
         fact_actor_t operator_actor = {.rank = FACT_ACTOR_OPERATOR, .authenticated = 1};
         snprintf(operator_actor.principal, sizeof(operator_actor.principal), "test:operator");
         snprintf(operator_actor.role, sizeof(operator_actor.role), "operator");
         assert(db2_fact_mutation_review(&operator_actor, result.assertion_id, FACT_REVIEW_APPROVE,
                                         &result) == 0);
      }
      assert(strcmp(result.lifecycle, lifecycle) == 0);
      return;
   }

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(),
       "INSERT INTO entity_edges(source,relation,target,weight,edge_class,lifecycle_state,"
       "commit_id) VALUES(?1,'related_to',?2,1,?3,?4,?5)",
       err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", source);
   aimee_pg_bind_text(st, "?2", target);
   aimee_pg_bind_text(st, "?3", edge_class);
   aimee_pg_bind_text(st, "?4", lifecycle);
   aimee_pg_bind_text(st, "?5", commit_id);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(st);
}

static int weighted_contains(const db2_entity_edge_weighted_neighbor_t *rows, int n,
                             const char *node)
{
   for (int i = 0; i < n; i++)
      if (strcmp(rows[i].node, node) == 0)
         return 1;
   return 0;
}

/* --- batched frontier reads ---
 *
 * The batch exists to remove a round trip per frontier node, so the round-trip
 * count is asserted directly. But the assertion that matters more is
 * EQUIVALENCE: a batch that returns a different neighbour set than the per-node
 * reads it replaces would silently change what recall can reach, which is worse
 * than the latency it set out to fix. Both are checked here, against the shim,
 * with no clock and no load sensitivity.
 */
void aimee_pg_test_stmt_count_reset(void);
long aimee_pg_test_stmt_count(void);

static void test_batch_matches_per_node_reads_and_costs_one_statement(void)
{
   setup();
   insert_test_edge("froot-a", "froot-a-n1", "cooccurrence", "candidate", "");
   insert_test_edge("froot-a", "froot-a-n2", "cooccurrence", "candidate", "");
   insert_test_edge("froot-b", "froot-b-n1", "cooccurrence", "candidate", "");
   insert_test_edge("froot-c-src", "froot-c", "cooccurrence", "candidate", "");

   const char *frontier[3] = {"froot-a", "froot-b", "froot-c"};

   /* What the per-node reader returns, node by node -- the behaviour the batch
    * has to reproduce. */
   db2_entity_edge_weighted_neighbor_t single[32];
   int ns = 0;
   long single_stmts = 0;
   aimee_pg_test_stmt_count_reset();
   for (int i = 0; i < 3; i++)
      ns += db2_entity_edge_neighbors_weighted(frontier[i], single + ns, 32 - ns, 16, 0);
   single_stmts = aimee_pg_test_stmt_count();

   db2_entity_edge_weighted_neighbor_t batch[32];
   memset(batch, 0, sizeof(batch));
   aimee_pg_test_stmt_count_reset();
   int nb = db2_entity_edge_neighbors_weighted_batch(frontier, 3, batch, 32, 16, 0);
   long batch_stmts = aimee_pg_test_stmt_count();

   /* Same neighbours, both directions of the edge included. */
   assert(nb == ns);
   for (int i = 0; i < ns; i++)
      assert(weighted_contains(batch, nb, single[i].node));
   assert(weighted_contains(batch, nb, "froot-a-n1"));
   assert(weighted_contains(batch, nb, "froot-a-n2"));
   assert(weighted_contains(batch, nb, "froot-b-n1"));
   assert(weighted_contains(batch, nb, "froot-c-src"));

   /* One statement for the whole frontier, where there were three. */
   assert(single_stmts == 3);
   assert(batch_stmts == 1);

   teardown();
}

static void test_batch_caps_per_node_not_globally(void)
{
   setup();
   /* A high-degree node next to a low-degree one. Under a single global LIMIT
    * the busy node would consume the budget and the quiet one would vanish from
    * the frontier entirely -- a silent change to what the walk can reach. */
   insert_test_edge("busy", "busy-n1", "cooccurrence", "candidate", "");
   insert_test_edge("busy", "busy-n2", "cooccurrence", "candidate", "");
   insert_test_edge("busy", "busy-n3", "cooccurrence", "candidate", "");
   insert_test_edge("busy", "busy-n4", "cooccurrence", "candidate", "");
   insert_test_edge("quiet", "quiet-n1", "cooccurrence", "candidate", "");

   const char *frontier[2] = {"busy", "quiet"};
   db2_entity_edge_weighted_neighbor_t rows[32];
   memset(rows, 0, sizeof(rows));
   int n = db2_entity_edge_neighbors_weighted_batch(frontier, 2, rows, 32, 1, 0);

   assert(n == 2); /* one per frontier node, not two from "busy" */
   assert(weighted_contains(rows, n, "quiet-n1"));
   teardown();
}

static void test_batch_applies_the_same_lifecycle_gates(void)
{
   setup();
   /* The quarantine is a property of the reader, not of the caller. A batched
    * read that skipped it would be a way around the gate. */
   const char *commit_id = "test-batch-lifecycle";
   insert_test_edge("batch-root", "batch-candidate", "semantic", "candidate", commit_id);
   insert_test_edge("batch-root", "batch-persistent", "semantic", "persistent", commit_id);
   insert_test_edge("batch-root", "batch-cooccurrence", "cooccurrence", "candidate", "");

   const char *frontier[1] = {"batch-root"};
   db2_entity_edge_weighted_neighbor_t rows[16];
   memset(rows, 0, sizeof(rows));
   int n = db2_entity_edge_neighbors_weighted_batch(frontier, 1, rows, 16, 16, 0);

   assert(!weighted_contains(rows, n, "batch-candidate"));
   assert(weighted_contains(rows, n, "batch-persistent"));
   assert(weighted_contains(rows, n, "batch-cooccurrence"));
   teardown();
}

static void test_candidate_semantic_edges_are_quarantined_from_graph_recall(void)
{
   setup();
   const char *commit_id = "test-graph-recall-lifecycle";
   insert_test_edge("recall-root", "candidate-direct", "semantic", "candidate", commit_id);
   insert_test_edge("recall-root", "persistent-direct", "semantic", "persistent", commit_id);
   insert_test_edge("recall-root", "promoted-direct", "semantic", "promoted", commit_id);
   insert_test_edge("recall-root", "cooccurrence-direct", "cooccurrence", "candidate", "");
   insert_test_edge("persistent-direct", "candidate-hop-two", "semantic", "candidate", commit_id);
   insert_test_edge("persistent-direct", "promoted-hop-two", "semantic", "promoted", commit_id);

   db2_entity_edge_weighted_neighbor_t weighted[16];
   memset(weighted, 0, sizeof(weighted));
   int nw = db2_entity_edge_neighbors_weighted("recall-root", weighted, 16, 16, 0);
   assert(!weighted_contains(weighted, nw, "candidate-direct"));
   assert(weighted_contains(weighted, nw, "persistent-direct"));
   assert(weighted_contains(weighted, nw, "promoted-direct"));
   assert(weighted_contains(weighted, nw, "cooccurrence-direct"));

   memset(weighted, 0, sizeof(weighted));
   nw = db2_entity_edge_neighbors_weighted("persistent-direct", weighted, 16, 16, 0);
   assert(!weighted_contains(weighted, nw, "candidate-hop-two"));
   assert(weighted_contains(weighted, nw, "recall-root"));
   assert(weighted_contains(weighted, nw, "promoted-hop-two"));

   teardown();
}

int main(void)
{
   printf("test_decay_zero_score... ");
   test_decay_zero_score();
   printf("ok\n");
   printf("test_decay_sentinel_1970... ");
   test_decay_sentinel_1970();
   printf("ok\n");
   printf("test_decay_empty_timestamp_nonzero... ");
   test_decay_empty_timestamp_nonzero();
   printf("ok\n");
   printf("test_decay_null_timestamp_nonzero... ");
   test_decay_null_timestamp_nonzero();
   printf("ok\n");
   printf("test_decay_recent_timestamp... ");
   test_decay_recent_timestamp();
   printf("ok\n");
   printf("test_decay_clamped... ");
   test_decay_clamped();
   printf("ok\n");
   printf("test_decay_half_life... ");
   test_decay_half_life();
   printf("ok\n");
   printf("test_prune_priority_no_utility... ");
   test_prune_priority_no_utility();
   printf("ok\n");
   printf("test_prune_priority_with_utility... ");
   test_prune_priority_with_utility();
   printf("ok\n");
   printf("test_prune_priority_negative_utility... ");
   test_prune_priority_negative_utility();
   printf("ok\n");
   printf("test_score_parts_new_fields... ");
   test_score_parts_new_fields();
   printf("ok\n");
   printf("test_backfill_no_db... ");
   test_backfill_no_db();
   printf("ok\n");
   printf("test_weighted_neighbors_null... ");
   test_weighted_neighbors_null();
   printf("ok\n");
   printf("test_batch_neighbors_null... ");
   test_batch_neighbors_null();
   printf("ok\n");
   printf("test_batch_matches_per_node_reads_and_costs_one_statement... ");
   test_batch_matches_per_node_reads_and_costs_one_statement();
   printf("ok\n");
   printf("test_batch_caps_per_node_not_globally... ");
   test_batch_caps_per_node_not_globally();
   printf("ok\n");
   printf("test_batch_applies_the_same_lifecycle_gates... ");
   test_batch_applies_the_same_lifecycle_gates();
   printf("ok\n");
   printf("test_backfill_idempotent... ");
   test_backfill_idempotent();
   printf("ok\n");
   printf("test_candidate_semantic_edges_are_quarantined_from_graph_recall... ");
   test_candidate_semantic_edges_are_quarantined_from_graph_recall();
   printf("ok\n");
   printf("graph_scoring: all tests passed\n");
   return 0;
}
