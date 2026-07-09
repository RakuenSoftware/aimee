/* test_wfe_foreach.c -- the foreach.workflow executor's fan-in aggregation, driven
 * through the engine with a mock child-spawn provider. Stubs the producing blocks
 * (understand/split) and overrides only foreach with the real executor, so the DB
 * parent<->child aggregation (db1_work_item_child_counts) is exercised end-to-end:
 *   - no children + spawner returns N>0 -> park (slices running)
 *   - no children + spawner returns 0    -> advance (no packets, nothing to do)
 *   - no children + no spawner           -> park (fail closed)
 *   - all children accepted              -> advance (every slice merged)
 *   - a child rejected or abandoned      -> park pending_human (a slice will not merge)
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "db1.h"
#include "wfe_store.h"
#include "wfe_engine.h"
#include "wfe_iface.h"
#include "wfe_blocks.h"

static const char *FE = "name: fe\n"
                        "start: u\n"
                        "nodes:\n"
                        "  - id: u\n"
                        "    block: understand\n"
                        "    next: sp\n"
                        "  - id: sp\n"
                        "    block: split\n"
                        "    in:\n"
                        "      intent: u.out\n"
                        "    next: fe\n"
                        "  - id: fe\n"
                        "    block: foreach.workflow\n"
                        "    in:\n"
                        "      packets: sp.out\n"
                        "    params:\n"
                        "      workflow: slice\n";

static void setup_home(void)
{
   char d[] = "/tmp/wfe_fe_XXXXXX";
   char *dir = mkdtemp(d);
   assert(dir);
   char wf[128];
   snprintf(wf, sizeof wf, "%s/workflows", dir);
   mkdir(wf, 0755);
   char p[200];
   snprintf(p, sizeof p, "%s/fe.yaml", wf);
   FILE *f = fopen(p, "wb");
   assert(f);
   fputs(FE, f);
   fclose(f);
   setenv("AIMEE_HOME", dir, 1);
}

/* ---- mock child-spawn provider: create g_spawn_n child rows under the parent. ---- */
static int g_spawn_n;   /* children to create (0 = no packets) */
static int g_spawn_err; /* 1 -> return -1 (fatal fan-out failure) */
static int g_spawned;   /* observed: was spawn actually called? */
static int mock_spawn(const char *wi, const char *child, const char *packets_path, int maxc,
                      char *err, size_t n)
{
   (void)packets_path;
   (void)maxc;
   (void)err;
   (void)n;
   g_spawned = 1;
   if (g_spawn_err)
      return -1;
   for (int i = 0; i < g_spawn_n; i++)
   {
      char id[80], path[96];
      snprintf(id, sizeof id, "%s.c%d", wi, i);
      snprintf(path, sizeof path, "cp/%s/%d", wi, i);
      assert(db1_work_item_create(id, "r/fe", path, child, "v1", "impl", "autonomous") == 0);
      assert(db1_work_item_set_parent(id, wi) == 0);
   }
   return g_spawn_n;
}
static const wfe_foreach_provider_t MOCK = {mock_spawn};

/* Create a work item on the `fe` workflow, drive it to a stop, return its final row.
 * For the spawn-driven cases only (the pre-seeded cases mint the id + seed children
 * inline before running, since the id is not known until create). */
static void run_fe(const char *repo, const char *ppath, db1_work_item_t *wi, char id_out[80])
{
   char id[80] = "", err[256] = "";
   assert(wfe_work_item_create("fe", repo, ppath, "autonomous", id, err, sizeof err) == 0);
   if (id_out)
      snprintf(id_out, 80, "%s", id);
   assert(wfe_engine_run(id, err, sizeof err) == 0);
   assert(db1_work_item_get(id, wi) == 1);
}

int main(void)
{
   printf("wfe-foreach: ");
   setup_home();
   assert(db1_init(":memory:") == 0);
   wfe_register_stub_executors();
   wfe_register_foreach_block(); /* override foreach with the REAL aggregating executor */

   db1_work_item_t wi;

   /* (1) spawner creates 2 children -> park (slices running, nothing merged yet). */
   wfe_set_foreach_provider(&MOCK);
   g_spawn_n = 2;
   g_spawn_err = 0;
   g_spawned = 0;
   run_fe("r/one", "p/one", &wi, NULL);
   assert(g_spawned == 1);
   assert(strcmp(wi.state, "active") == 0);
   assert(strcmp(wi.pause_reason, "pending_human") == 0);

   /* (2) spawner reports 0 packets -> advance straight through (nothing to slice). */
   g_spawn_n = 0;
   g_spawn_err = 0;
   run_fe("r/zero", "p/zero", &wi, NULL);
   assert(strcmp(wi.state, "accepted") == 0);

   /* (3) no spawn provider + no children -> fail closed (park). */
   wfe_set_foreach_provider(NULL);
   run_fe("r/none", "p/none", &wi, NULL);
   assert(strcmp(wi.state, "active") == 0);
   assert(strcmp(wi.pause_reason, "pending_human") == 0);

   /* (4) all children accepted BEFORE foreach runs -> advance (every slice merged).
    * Pre-seed children so the executor sees total>0 and never calls spawn. */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("fe", "r/done", "p/done", "autonomous", id, err, sizeof err) ==
             0);
      for (int i = 0; i < 3; i++)
      {
         char cid[80], cp[96];
         snprintf(cid, sizeof cid, "%s.k%d", id, i);
         snprintf(cp, sizeof cp, "kp/%s/%d", id, i);
         assert(db1_work_item_create(cid, "r/done", cp, "slice", "v1", "impl", "autonomous") == 0);
         assert(db1_work_item_set_parent(cid, id) == 0);
         assert(db1_work_item_set_terminal(cid, "accepted") == 0);
      }
      g_spawned = 0;
      wfe_set_foreach_provider(&MOCK); /* installed, but must NOT be called (children exist) */
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "accepted") == 0);
      assert(g_spawned == 0); /* aggregation used the DB, not a re-spawn */
   }

   /* (5)+(6) a child that reached a terminal state OTHER than accepted (rejected, and
    * separately abandoned) -> the slice will never merge -> park for a human. */
   {
      const char *term[] = {"rejected", "abandoned"};
      const char *repos[] = {"r/rej", "r/aband"};
      for (int c = 0; c < 2; c++)
      {
         char id[80] = "", err[256] = "";
         char pp[32];
         snprintf(pp, sizeof pp, "p/%s", repos[c]);
         assert(wfe_work_item_create("fe", repos[c], pp, "autonomous", id, err, sizeof err) == 0);
         char cid[80], cp[96];
         snprintf(cid, sizeof cid, "%s.k0", id);
         snprintf(cp, sizeof cp, "fp/%s/0", id);
         assert(db1_work_item_create(cid, repos[c], cp, "slice", "v1", "impl", "autonomous") == 0);
         assert(db1_work_item_set_parent(cid, id) == 0);
         assert(db1_work_item_set_terminal(cid, term[c]) == 0);
         assert(wfe_engine_run(id, err, sizeof err) == 0);
         assert(db1_work_item_get(id, &wi) == 1);
         assert(strcmp(wi.state, "active") == 0);
         assert(strcmp(wi.pause_reason, "pending_human") == 0);
      }
   }

   printf("ok\n");
   return 0;
}
