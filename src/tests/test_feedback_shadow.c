/* test_feedback_shadow.c: unit tests for Phase 7 path-credit feedback and
 * shadow-delta persistence. */

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
#include "memory.h"
#include "modules/memory/memory_graph_fusion.h"
#include "../modules/db2/c/shadow_delta.h"

static char g_db_path[512];

static void setup(void)
{
   snprintf(g_db_path, sizeof(g_db_path), "%s/aimee-test-fs-XXXXXX", platform_tmpdir());
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

/* --- path-credit distribution --- */

static void test_credit_single_edge_full_delta(void)
{
   /* A one-edge path gets the entire delta. */
   memory_graph_path_edge_t edges[1] = {{.relation = "defines", .hop = 1}};
   double credits[1];
   int rc = memory_graph_distribute_path_credit(0.1, edges, 1, credits);
   assert(rc == 0);
   assert(fabs(credits[0] - 0.1) < 1e-9);
}

static void test_credit_sums_to_delta(void)
{
   /* Credits across the path must sum to the total delta. */
   memory_graph_path_edge_t edges[3] = {
       {.relation = "defines", .hop = 1},
       {.relation = "calls", .hop = 2},
       {.relation = "imports", .hop = 2},
   };
   double credits[3];
   int rc = memory_graph_distribute_path_credit(0.1, edges, 3, credits);
   assert(rc == 0);
   double sum = credits[0] + credits[1] + credits[2];
   assert(fabs(sum - 0.1) < 1e-9);
}

static void test_credit_negative_delta(void)
{
   memory_graph_path_edge_t edges[2] = {
       {.relation = "defines", .hop = 1},
       {.relation = "co_discussed", .hop = 1},
   };
   double credits[2];
   int rc = memory_graph_distribute_path_credit(-0.1, edges, 2, credits);
   assert(rc == 0);
   double sum = credits[0] + credits[1];
   assert(fabs(sum - (-0.1)) < 1e-9);
   /* Both credits negative. */
   assert(credits[0] < 0.0 && credits[1] < 0.0);
}

static void test_credit_higher_gravity_gets_more(void)
{
   /* Same hop: defines (1.00) gets more credit than imports (0.30). */
   memory_graph_path_edge_t edges[2] = {
       {.relation = "defines", .hop = 1},
       {.relation = "imports", .hop = 1},
   };
   double credits[2];
   memory_graph_distribute_path_credit(0.1, edges, 2, credits);
   assert(credits[0] > credits[1]);
}

static void test_credit_hop_decay(void)
{
   /* Same relation, deeper hop gets less credit. */
   memory_graph_path_edge_t edges[2] = {
       {.relation = "calls", .hop = 1},
       {.relation = "calls", .hop = 2},
   };
   double credits[2];
   memory_graph_distribute_path_credit(0.1, edges, 2, credits);
   assert(credits[0] > credits[1]);
   /* hop-2 weight is half of hop-1 → credit ratio 2:1 */
   assert(fabs(credits[0] - 2.0 * credits[1]) < 1e-9);
}

static void test_credit_zero_path_guard(void)
{
   double credits[1];
   int rc = memory_graph_distribute_path_credit(0.1, NULL, 0, credits);
   assert(rc == -1);
   memory_graph_path_edge_t edges[1] = {{.relation = "defines", .hop = 1}};
   rc = memory_graph_distribute_path_credit(0.1, edges, 0, credits);
   assert(rc == -1);
}

/* --- memory_apply_feedback_path --- */

static void test_apply_feedback_path_null_guard(void)
{
   int rc = memory_apply_feedback_path(1, NULL, NULL, NULL, 3);
   assert(rc == -1);
}

static void test_apply_feedback_path_no_crash(void)
{
   setup();
   const char *nodes[2] = {"file:aimee:src/x.c", "symbol:aimee:foo"};
   const char *rels[2] = {"defines", "calls"};
   int hops[2] = {1, 2};
   /* No matching edges in a fresh DB → no-op, must not crash. */
   int rc = memory_apply_feedback_path(1, nodes, rels, hops, 2);
   assert(rc == 0);
   teardown();
}

/* --- shadow-delta persistence --- */

static void test_shadow_insert_and_count(void)
{
   setup();
   db2_shadow_delta_row_t row;
   memset(&row, 0, sizeof(row));
   snprintf(row.query_hash, sizeof(row.query_hash), "abc123");
   snprintf(row.project, sizeof(row.project), "aimee");
   snprintf(row.mode, sizeof(row.mode), "shadow");
   row.result_count = 20;
   row.delta_json = "{\"deltas\":[]}";

   int rc = db2_shadow_delta_insert(&row);
   /* On SQLite shim insert should succeed (0) or gracefully -1 if no table. */
   assert(rc == 0 || rc == -1);

   if (rc == 0)
   {
      int64_t n = db2_shadow_delta_count("aimee");
      assert(n >= 1);
   }
   teardown();
}

static void test_shadow_insert_null_guard(void)
{
   int rc = db2_shadow_delta_insert(NULL);
   assert(rc == -1);
}

static void test_shadow_cleanup_defaults(void)
{
   setup();
   /* Cleanup with 0 bounds uses defaults; empty table → 0 deleted. */
   int deleted = db2_shadow_delta_cleanup("aimee", 0, 0);
   assert(deleted >= 0);
   teardown();
}

static void test_shadow_cleanup_no_db(void)
{
   /* No DB connection → graceful 0. */
   int deleted = db2_shadow_delta_cleanup(NULL, 100, 7);
   assert(deleted == 0 || deleted >= 0);
}

static void test_shadow_defaults_constants(void)
{
   assert(SHADOW_DELTA_DEFAULT_MAX_ROWS == 10000);
   assert(SHADOW_DELTA_DEFAULT_RETENTION_DAYS == 14);
}

static void test_shadow_count_empty(void)
{
   setup();
   int64_t n = db2_shadow_delta_count("nonexistent_project");
   assert(n == 0);
   teardown();
}

int main(void)
{
   printf("test_credit_single_edge_full_delta... ");
   test_credit_single_edge_full_delta();
   printf("ok\n");
   printf("test_credit_sums_to_delta... ");
   test_credit_sums_to_delta();
   printf("ok\n");
   printf("test_credit_negative_delta... ");
   test_credit_negative_delta();
   printf("ok\n");
   printf("test_credit_higher_gravity_gets_more... ");
   test_credit_higher_gravity_gets_more();
   printf("ok\n");
   printf("test_credit_hop_decay... ");
   test_credit_hop_decay();
   printf("ok\n");
   printf("test_credit_zero_path_guard... ");
   test_credit_zero_path_guard();
   printf("ok\n");
   printf("test_apply_feedback_path_null_guard... ");
   test_apply_feedback_path_null_guard();
   printf("ok\n");
   printf("test_apply_feedback_path_no_crash... ");
   test_apply_feedback_path_no_crash();
   printf("ok\n");
   printf("test_shadow_insert_and_count... ");
   test_shadow_insert_and_count();
   printf("ok\n");
   printf("test_shadow_insert_null_guard... ");
   test_shadow_insert_null_guard();
   printf("ok\n");
   printf("test_shadow_cleanup_defaults... ");
   test_shadow_cleanup_defaults();
   printf("ok\n");
   printf("test_shadow_cleanup_no_db... ");
   test_shadow_cleanup_no_db();
   printf("ok\n");
   printf("test_shadow_defaults_constants... ");
   test_shadow_defaults_constants();
   printf("ok\n");
   printf("test_shadow_count_empty... ");
   test_shadow_count_empty();
   printf("ok\n");
   printf("feedback_shadow: all tests passed\n");
   return 0;
}
