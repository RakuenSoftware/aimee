/* test_wfe_scheduler.c -- the autonomy scheduler drives ACTIVE AUTONOMOUS work
 * items forward and leaves interactive ones for the human (Phase B). Uses the
 * synchronous wfe_scheduler_run_once + stub executors for determinism. */
#include "wfe_test_home.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "db1.h"
#include "db1_internal.h" /* db1_conn — stamp updated_at directly for the LRU test */
#include "sqlite3.h"
#include "wfe_engine.h"
#include "wfe_scheduler.h"
#include "wfe_store.h"

/* author.proposal with no `next` -> terminal after one advance. */
static const char *WF = "name: sc\nstart: au\nnodes:\n"
                        "  - id: au\n    block: author.proposal\n";

static int create(const char *suffix, const char *mode, char id[80])
{
   char err[256] = "";
   return wfe_work_item_create("sc", "r", suffix, mode, id, err, sizeof err);
}

int main(void)
{
   printf("wfe-scheduler: ");
   char home[] = "/tmp/wfe_sc_XXXXXX";
   assert(wfe_test_mkdtemp(home));
   char wf[160];
   snprintf(wf, sizeof wf, "%s/workflows", home);
   mkdir(wf, 0755);
   char p[256];
   snprintf(p, sizeof p, "%s/workflows/sc.yaml", home);
   FILE *f = fopen(p, "wb");
   assert(f);
   fputs(WF, f);
   fclose(f);
   setenv("AIMEE_HOME", home, 1);
   assert(db1_init(":memory:") == 0);

   wfe_reset_block_executors();
   wfe_register_stub_executors(); /* every block -> ADVANCED, deterministic */

   char a[80] = "", b[80] = "";
   assert(create("a", "autonomous", a) == 0 && a[0]);
   assert(create("b", "interactive", b) == 0 && b[0]);

   /* One synchronous sweep: the autonomous item runs to terminal; the
    * interactive item is left untouched for the human to drive. */
   wfe_scheduler_run_once();

   db1_work_item_t wa, wb;
   assert(db1_work_item_get(a, &wa) == 1);
   assert(db1_work_item_get(b, &wb) == 1);

   assert(strcmp(wa.state, "active") != 0); /* autonomous item advanced past active */
   assert(strcmp(wb.state, "active") == 0); /* interactive item NOT driven by the scheduler */

   /* LRU listing: the scheduler's sweep order is least-recently-updated FIRST,
    * so a busy (recently updated) item can never permanently starve a stale
    * sibling. Stamp distinct updated_at values and assert the order. */
   char c[80] = "", d[80] = "";
   assert(create("c", "autonomous", c) == 0 && c[0]);
   assert(create("d", "autonomous", d) == 0 && d[0]);
   {
      char sql[256];
      snprintf(sql, sizeof sql,
               "UPDATE lifecycle_work_item SET updated_at='2000-01-01 00:00:01' WHERE "
               "work_item_id='%s'",
               d);
      assert(sqlite3_exec(db1_conn(), sql, NULL, NULL, NULL) == SQLITE_OK);
      snprintf(sql, sizeof sql,
               "UPDATE lifecycle_work_item SET updated_at='2000-01-01 00:00:02' WHERE "
               "work_item_id='%s'",
               c);
      assert(sqlite3_exec(db1_conn(), sql, NULL, NULL, NULL) == SQLITE_OK);
   }
   db1_work_item_t *lru = NULL;
   int n = db1_work_item_list_lru(&lru);
   assert(n >= 4);
   assert(strcmp(lru[0].work_item_id, d) == 0); /* stalest first */
   assert(strcmp(lru[1].work_item_id, c) == 0);
   free(lru);

   printf("ok (autonomous=%s, interactive untouched, lru order)\n", wa.state);
   return 0;
}
