/* Unit tests for the deepening-sweep scope/caps logic (Part B PR-B2). Pure. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sweep.h"

static void test_allowlist(void)
{
   const char *globs[] = {"src/**", "tests/**", "Makefile"};
   int n = 3;
   assert(sweep_path_allowed("src/foo.c", globs, n) == 1);
   assert(sweep_path_allowed("src/server/bar.c", globs, n) == 1);
   assert(sweep_path_allowed("tests/t.c", globs, n) == 1);
   assert(sweep_path_allowed("Makefile", globs, n) == 1);  /* exact */
   assert(sweep_path_allowed("docs/x.md", globs, n) == 0); /* outside */
   assert(sweep_path_allowed("srcfoo.c", globs, n) == 0);  /* prefix must hit a dir boundary */
   assert(sweep_path_allowed("", globs, n) == 0);
   assert(sweep_path_allowed("src/foo.c", NULL, 0) == 0);

   const char *pre[] = {"src/db*"};
   assert(sweep_path_allowed("src/db2", pre, 1) == 1); /* trailing * prefix */
   assert(sweep_path_allowed("src/net", pre, 1) == 0);

   /* trailing-* has NO dir boundary (unlike slash-star-star): "src*" matches "srcX/.." */
   const char *nb[] = {"src*"};
   assert(sweep_path_allowed("srcX/foo.c", nb, 1) == 1);
   /* overlapping globs: either may match */
   const char *ov[] = {"src/**", "src/db*"};
   assert(sweep_path_allowed("src/db2/x.c", ov, 2) == 1);
}

static void test_partition(void)
{
   /* sorted by dir: two dirs, second exceeds the per-area cap of 2 -> chunked */
   const char *paths[] = {
       "src/a/1.c", "src/a/2.c",             /* dir src/a (2 files, cap 2) -> area 0 */
       "src/b/1.c", "src/b/2.c", "src/b/3.c" /* dir src/b (3 files, cap 2) -> areas 1,2 */
   };
   int n = 5;
   int area[5];
   int ac = sweep_partition(paths, n, 2, area);
   assert(ac == 3);
   assert(area[0] == 0 && area[1] == 0); /* src/a together */
   assert(area[2] == 1 && area[3] == 1); /* src/b first chunk */
   assert(area[4] == 2);                 /* src/b overflow chunk */

   /* a top-level file (no dir) is its own group */
   const char *p2[] = {"README", "src/x.c"};
   int a2[2];
   assert(sweep_partition(p2, 2, 50, a2) == 2);
   assert(a2[0] == 0 && a2[1] == 1);

   /* max_files_per_area == 1: every file is its own area within a dir */
   const char *three[] = {"src/a/1.c", "src/a/2.c", "src/a/3.c"};
   int a3[3];
   assert(sweep_partition(three, 3, 1, a3) == 3);
   assert(a3[0] == 0 && a3[1] == 1 && a3[2] == 2);

   /* identical leaf dir name under different roots -> distinct areas (full dir path) */
   const char *roots[] = {"a/b/c.c", "x/b/y.c"};
   int ar[2];
   assert(sweep_partition(roots, 2, 50, ar) == 2);
   assert(ar[0] == 0 && ar[1] == 1);

   /* empty + bad args */
   assert(sweep_partition(paths, 0, 50, area) == 0);
   assert(sweep_partition(NULL, 5, 50, area) == -1);
   assert(sweep_partition(paths, 5, 0, area) == -1);
}

static void test_caps_defaults(void)
{
   sweep_caps_t c;
   sweep_caps_defaults(&c);
   assert(c.max_areas == 40 && c.max_files_per_area == 50 && c.max_calls_per_area == 2);
   assert(c.max_items_per_area == 10 && c.wall_area_s == 60 && c.wall_sweep_s == 1800);
}

int main(void)
{
   test_allowlist();
   test_partition();
   test_caps_defaults();
   printf("sweep_scope: all tests passed\n");
   return 0;
}
