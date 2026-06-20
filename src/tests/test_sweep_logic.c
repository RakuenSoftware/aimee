/* Unit tests for the deepening-sweep pure decision logic (Part B PR-B1):
 * exclusion identity + the mechanical deletion test. No IO, no backend. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sweep.h"

static void test_seam_key_and_exclude(void)
{
   char k[SWEEP_KEY_MAX];
   sweep_seam_key("src/foo.c", "helper_x", k, sizeof(k));
   assert(strcmp(k, "src/foo.c:helper_x") == 0);

   /* exclusion keys on the ORIGINAL seam, so a renamed proposed module does not match */
   const char *settled[] = {"src/foo.c:helper_x", "src/net.c:parse"};
   assert(sweep_excluded("src/foo.c:helper_x", settled, 2) == 1);
   assert(sweep_excluded("src/foo_extract.c:helper_x", settled, 2) ==
          0); /* rename bypass blocked */
   assert(sweep_excluded("src/other.c:thing", settled, 2) == 0);

   /* incomplete seam -> empty key (never an ambiguous ":" that could collide) */
   char empty[SWEEP_KEY_MAX];
   sweep_seam_key(NULL, NULL, empty, sizeof(empty));
   assert(empty[0] == '\0');
   sweep_seam_key("src/foo.c", "", empty, sizeof(empty));
   assert(empty[0] == '\0');
   assert(sweep_excluded("", settled, 2) == 0);
}

static sweep_edges_t mk(int callers, int files, int shared, int common)
{
   sweep_edges_t e = {callers, files, shared, common};
   return e;
}

static void test_score(void)
{
   sweep_score_cfg_t cfg;
   sweep_score_cfg_defaults(&cfg);
   assert(cfg.min_callers == 3 && cfg.min_distinct_files == 2 && cfg.shared_state_tolerance == 1);
   char r[128];

   /* clean cross-site seam -> STRONG, reason names the counts */
   sweep_edges_t strong = mk(3, 3, 0, 0);
   assert(sweep_score(&strong, &cfg, r, sizeof(r)) == SWEEP_STRONG);
   assert(strstr(r, "independent callers"));

   /* below the rule of three -> REJECT, reason names the shortfall */
   sweep_edges_t few = mk(2, 2, 0, 0);
   assert(sweep_score(&few, &cfg, r, sizeof(r)) == SWEEP_REJECT);
   assert(strstr(r, "only 2 caller"));

   /* count ok but single-file inflation -> WORTH */
   sweep_edges_t inflated = mk(30, 1, 0, 0);
   assert(sweep_score(&inflated, &cfg, r, sizeof(r)) == SWEEP_WORTH);

   /* count ok but callers not independent -> WORTH */
   sweep_edges_t funnel = mk(4, 3, 0, 1);
   assert(sweep_score(&funnel, &cfg, r, sizeof(r)) == SWEEP_WORTH);

   /* count ok but over-coupled (shared > tolerance) -> WORTH */
   sweep_edges_t coupled = mk(4, 3, 2, 0);
   assert(sweep_score(&coupled, &cfg, r, sizeof(r)) == SWEEP_WORTH);

   /* shared exactly at tolerance is fine -> STRONG */
   sweep_edges_t at_tol = mk(3, 2, 1, 0);
   assert(sweep_score(&at_tol, &cfg, r, sizeof(r)) == SWEEP_STRONG);

   /* NULL edges -> REJECT; NULL cfg -> defaults applied */
   assert(sweep_score(NULL, &cfg, r, sizeof(r)) == SWEEP_REJECT);
   assert(sweep_score(&strong, NULL, r, sizeof(r)) == SWEEP_STRONG);
}

static caller_hit_t mkc(const char *file, const char *caller, int line)
{
   caller_hit_t h;
   memset(&h, 0, sizeof(h));
   snprintf(h.file_path, sizeof(h.file_path), "%s", file);
   snprintf(h.caller, sizeof(h.caller), "%s", caller);
   h.line = line;
   return h;
}

static void test_edges_from_callers(void)
{
   /* 3 callers across 3 files, distinct callers -> clean edges */
   caller_hit_t a[3] = {mkc("x.c", "f1", 1), mkc("y.c", "f2", 2), mkc("z.c", "f3", 3)};
   sweep_edges_t e = sweep_edges_from_callers(a, 3, 0);
   assert(e.caller_count == 3 && e.distinct_files == 3 && e.common_caller == 0);

   /* all callers in one calling function -> funnel (common_caller) + blast deps */
   caller_hit_t f[3] = {mkc("x.c", "dispatch", 1), mkc("y.c", "dispatch", 2),
                        mkc("z.c", "dispatch", 3)};
   e = sweep_edges_from_callers(f, 3, 2);
   assert(e.common_caller == 1 && e.shared_state == 2);

   /* all callers in one file -> distinct_files 1 */
   caller_hit_t s[3] = {mkc("x.c", "f1", 1), mkc("x.c", "f2", 2), mkc("x.c", "f3", 3)};
   e = sweep_edges_from_callers(s, 3, 0);
   assert(e.distinct_files == 1 && e.common_caller == 0);

   /* callers with empty caller names are counted but excluded from funnel detection
    * (a single named caller among empties is not a funnel) */
   caller_hit_t mixed[3] = {mkc("x.c", "", 1), mkc("y.c", "", 2), mkc("z.c", "g", 3)};
   e = sweep_edges_from_callers(mixed, 3, 0);
   assert(e.caller_count == 3 && e.distinct_files == 3 && e.common_caller == 0);

   /* empty / negative blast clamps */
   e = sweep_edges_from_callers(NULL, 0, -5);
   assert(e.caller_count == 0 && e.shared_state == 0);
}

int main(void)
{
   test_seam_key_and_exclude();
   test_score();
   test_edges_from_callers();
   printf("sweep_logic: all tests passed\n");
   return 0;
}
