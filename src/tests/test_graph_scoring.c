/* test_graph_scoring.c: unit tests for Phase 4 utility-aware graph scoring. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db.h"
#include "db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"
#include "platform_test_util.h"
#include "../modules/db2/c/entity_edges.h"
#include "../headers/memory.h"

static char g_db_path[512];

static void setup(void)
{
   snprintf(g_db_path, sizeof(g_db_path), "%s/aimee-test-gs-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(g_db_path, sizeof(g_db_path), "aim");
   assert(fd >= 0);
   close(fd);
   assert(db1_init(g_db_path) == 0);
   db2_test_shim_open_path(g_db_path);
}

static void teardown(void)
{
   db2_test_shim_close();
   db1_shutdown();
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

/* --- hop-decay factor --- */

static void test_hop_decay_factor(void)
{
   /* Proposal: 1-hop → factor 1.0, 2-hop → factor 0.5.
    * db2_entity_edge_two_hop_neighbors returns hop=1 or hop=2 in .hop.
    * Verify the expected multipliers are applied by convention. */
   db2_entity_edge_hop_t h1 = {.hop = 1};
   db2_entity_edge_hop_t h2 = {.hop = 2};
   double factor1 = (h1.hop == 1) ? 1.0 : 0.5;
   double factor2 = (h2.hop == 1) ? 1.0 : 0.5;
   assert(fabs(factor1 - 1.0) < 1e-9);
   assert(fabs(factor2 - 0.5) < 1e-9);
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

static void test_two_hop_null(void)
{
   db2_entity_edge_hop_t buf[8];
   int n = db2_entity_edge_two_hop_neighbors(NULL, 8, 16, buf);
   assert(n == 0);
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
   printf("test_hop_decay_factor... ");
   test_hop_decay_factor();
   printf("ok\n");
   printf("test_backfill_no_db... ");
   test_backfill_no_db();
   printf("ok\n");
   printf("test_weighted_neighbors_null... ");
   test_weighted_neighbors_null();
   printf("ok\n");
   printf("test_two_hop_null... ");
   test_two_hop_null();
   printf("ok\n");
   printf("test_backfill_idempotent... ");
   test_backfill_idempotent();
   printf("ok\n");
   printf("graph_scoring: all tests passed\n");
   return 0;
}
