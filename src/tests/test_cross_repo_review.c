/* test_cross_repo_review.c: S4b review queue + adjudication over the sqlite shim:
 * upsert/fingerprint-dedup, list ordering, accept/reject, overflow eviction. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "../db2/cross_repo_review.h"
#include "../db2/db2.h"
#include "../db2/db_postgres.h"

static int up(const char *sym, const char *caller, const char *definer, double score, int qmax)
{
   return db2_cross_repo_review_upsert("rsh1", sym, caller, definer, "{}", score, "ambiguous", 0,
                                       qmax);
}

static void clear_queue(void)
{
   char e[256] = "";
   aimee_pg_exec(db2_conn(), "DELETE FROM cross_repo_review_queue", e, sizeof(e));
   aimee_pg_exec(db2_conn(), "UPDATE cross_repo_meta SET review_overflow_dropped = 0 WHERE id = 1",
                 e, sizeof(e));
}

static void test_upsert_list(void)
{
   printf("test_upsert_list... ");
   assert(up("render", "appA", "libX", 3.0, 5000) == 0);
   assert(up("update", "appA", "libY", 7.0, 5000) == 0);
   /* same fingerprint -> upsert (no dup); refresh score. */
   assert(up("render", "appA", "libX", 9.0, 5000) == 0);

   xrepo_review_row_t rows[16];
   int64_t dropped = -1;
   int n = db2_cross_repo_review_list(NULL, "open", rows, 16, &dropped);
   assert(n == 2); /* render upserted, not duplicated */
   /* ordered by evidence_score DESC: render(9) before update(7). */
   assert(strcmp(rows[0].symbol, "render") == 0 && rows[0].evidence_score > 8.9);
   assert(strcmp(rows[1].symbol, "update") == 0);
   assert(dropped == 0);

   /* filter by caller. */
   assert(up("foo_sym", "appB", "libZ", 1.0, 5000) == 0);
   n = db2_cross_repo_review_list("appA", "open", rows, 16, NULL);
   assert(n == 2);
   n = db2_cross_repo_review_list("appB", "open", rows, 16, NULL);
   assert(n == 1 && strcmp(rows[0].symbol, "foo_sym") == 0);
   printf("ok\n");
}

static void test_adjudicate(void)
{
   printf("test_adjudicate... ");
   xrepo_review_row_t rows[16];
   int n = db2_cross_repo_review_list("appA", "open", rows, 16, NULL);
   assert(n >= 1);
   int64_t id = rows[0].id;
   assert(db2_cross_repo_review_set_status(id, "accepted") == 0);
   assert(db2_cross_repo_review_set_status(id, "bogus") == -1); /* invalid status */
   /* now one fewer open for appA; the accepted one shows under status=accepted. */
   int open_after = db2_cross_repo_review_list("appA", "open", rows, 16, NULL);
   assert(open_after == n - 1);
   int acc = db2_cross_repo_review_list("appA", "accepted", rows, 16, NULL);
   assert(acc == 1 && rows[0].id == id);
   printf("ok\n");
}

static void test_overflow_eviction(void)
{
   printf("test_overflow_eviction... ");
   /* The overflow cap is global, so isolate: clear the queue, then cap=2 + insert
    * 4 with increasing score -> the 2 lowest-evidence are evicted, dropped == 2. */
   clear_queue();
   for (int i = 0; i < 4; i++)
   {
      char sym[32];
      snprintf(sym, sizeof(sym), "ovf_sym_%d", i);
      assert(up(sym, "ovfCaller", "libO", (double)(i + 1), 2) == 0);
   }
   xrepo_review_row_t rows[16];
   int64_t dropped = 0;
   int n = db2_cross_repo_review_list("ovfCaller", "open", rows, 16, &dropped);
   assert(n == 2); /* capped */
   /* highest-evidence survive: scores 4 and 3. */
   assert(rows[0].evidence_score > 3.9 && rows[1].evidence_score > 2.9);
   assert(dropped >= 2); /* cumulative evictions surfaced */
   printf("ok\n");
}

int main(void)
{
   db2_test_shim_open();
   test_upsert_list();
   test_adjudicate();
   test_overflow_eviction();
   printf("cross_repo_review: all tests passed\n");
   return 0;
}
