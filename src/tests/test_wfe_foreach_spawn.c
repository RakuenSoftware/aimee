/* test_wfe_foreach_spawn.c -- the live foreach.workflow spawner: a split packet-plan
 * fans out to one child "slice" work item per packet, linked to the parent, seeded
 * with its packet, idempotent, and cap-bounded. (The child-DRIVING is the autonomy
 * driver's job, integration-gated; this pins the fan-OUT creation logic.) */
#include "wfe_test_home.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "db1.h"
#include "wfe_store.h"
#include "server/wfe_live_foreach.h"

/* A minimal but valid "slice" workflow so wfe_work_item_resolve can pin its version
 * + start stage (the spawner needs those to create children). */
static const char *SLICE = "name: slice\nstart: u\nnodes:\n  - id: u\n    block: understand\n";

static void write_file(const char *path, const char *content)
{
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(content, f);
   fclose(f);
}

/* Build a packet-plan JSON with `n` packets into `path`. */
static void write_plan(const char *path, int n)
{
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs("{\"schema_version\":1,\"packets\":[", f);
   for (int i = 0; i < n; i++)
      fprintf(f,
              "%s{\"packet_id\":\"p%d\",\"summary\":\"slice %d\",\"target_blocks\":[\"implement\"],"
              "\"dependencies\":[],\"acceptance_criteria\":[\"c\"]}",
              i ? "," : "", i, i);
   fputs("]}", f);
   fclose(f);
}

int main(void)
{
   printf("wfe-foreach-spawn: ");
   char home[] = "/tmp/wfe_spawn_XXXXXX";
   assert(wfe_test_mkdtemp(home));
   char wf[160];
   snprintf(wf, sizeof wf, "%s/workflows", home);
   mkdir(wf, 0755);
   char p[256];
   snprintf(p, sizeof p, "%s/workflows/slice.yaml", home);
   write_file(p, SLICE);
   setenv("AIMEE_HOME", home, 1);
   assert(db1_init(":memory:") == 0);

   char plan[256];
   snprintf(plan, sizeof plan, "%s/plan.json", home);
   char err[256] = "";
   db1_work_item_t wi;

   /* --- 3 packets -> 3 linked child slice runs. --- */
   assert(db1_work_item_create("par", "repo1", "p/par", "build", "v", "slices", "autonomous") == 0);
   write_plan(plan, 3);
   assert(wfe_foreach_spawn("par", "slice", plan, 16, err, sizeof err) == 3);
   int total = 0, acc = 0, fail = 0;
   assert(db1_work_item_child_counts("par", &total, &acc, &fail) == 0);
   assert(total == 3 && acc == 0 && fail == 0);
   /* each child is a "slice" run linked to the parent, seeded from its packet. */
   assert(db1_work_item_get("par.s0", &wi) == 1);
   assert(strcmp(wi.parent_id, "par") == 0);
   assert(strcmp(wi.workflow_name, "slice") == 0);
   assert(strcmp(wi.state, "active") == 0);
   assert(wi.proposal_path[0]); /* seeded with a packet proposal */

   /* --- idempotent: a re-spawn creates nothing new. --- */
   assert(wfe_foreach_spawn("par", "slice", plan, 16, err, sizeof err) == 3);
   assert(db1_work_item_child_counts("par", &total, &acc, &fail) == 0 && total == 3);

   /* --- over-cap plan is refused (no silent truncation). --- */
   assert(db1_work_item_create("par2", "repo1", "p/par2", "build", "v", "slices", "autonomous") ==
          0);
   assert(wfe_foreach_spawn("par2", "slice", plan, 2, err, sizeof err) == -1);
   assert(db1_work_item_child_counts("par2", &total, NULL, NULL) == 0 && total == 0);

   /* --- absent plan -> 0 children (advance past an empty fan-out). --- */
   assert(wfe_foreach_spawn("par2", "slice", "/no/such/plan.json", 16, err, sizeof err) == 0);

   /* --- malformed plan -> fatal error. --- */
   char bad[256];
   snprintf(bad, sizeof bad, "%s/bad.json", home);
   write_file(bad, "not json {[");
   assert(wfe_foreach_spawn("par2", "slice", bad, 16, err, sizeof err) == -1);

   /* --- empty packet list -> 0 children. --- */
   char empty[256];
   snprintf(empty, sizeof empty, "%s/empty.json", home);
   write_file(empty, "{\"schema_version\":1,\"packets\":[]}");
   assert(wfe_foreach_spawn("par2", "slice", empty, 16, err, sizeof err) == 0);

   /* --- partial fan-out rolls back ALL-OR-NOTHING: a mid-loop create failure must
    *     leave the parent with NO children (else it would advance with fewer slices,
    *     and the idempotency guard would block a re-spawn). Force the 2nd child's id to
    *     collide with a pre-existing (unrelated) row so its create fails. --- */
   assert(db1_work_item_create("par3", "repo1", "p/par3", "build", "v", "slices", "autonomous") ==
          0);
   assert(db1_work_item_create("par3.s1", "repo1", "p/collide", "slice", "v", "impl",
                               "autonomous") == 0);
   write_plan(plan, 3);
   assert(wfe_foreach_spawn("par3", "slice", plan, 16, err, sizeof err) == -1);
   assert(db1_work_item_child_counts("par3", &total, NULL, NULL) == 0 && total == 0);
   assert(db1_work_item_get("par3.s0", &wi) ==
          0); /* the child created before the fault rolled back */

   printf("ok\n");
   return 0;
}
