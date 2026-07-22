/* test_cochange.c: pure co-change pairing policy (cochange.c). No DB, no git. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "index.h"

/* Load a NULL-terminated list of names into a fixed [][128] buffer. */
static int load(char buf[][128], const char *const *src)
{
   int n = 0;
   for (; src[n]; n++)
      snprintf(buf[n], 128, "%s", src[n]);
   return n;
}

static int find_pair(const cochange_pair_t *out, int n, const char *a, const char *b)
{
   for (int i = 0; i < n; i++)
      if (strcmp(out[i].a, a) == 0 && strcmp(out[i].b, b) == 0)
         return 1;
   return 0;
}

int main(void)
{
   char names[64][128];
   cochange_pair_t out[64];

   /* 1. Three distinct files -> C(3,2)=3 canonical pairs (a<b, sorted). */
   {
      const char *const src[] = {"c.c", "a.c", "b.c", NULL};
      int n = load(names, src);
      int p = cochange_pairs_for_commit(names, n, 25, out, 64);
      assert(p == 3);
      assert(find_pair(out, p, "a.c", "b.c"));
      assert(find_pair(out, p, "a.c", "c.c"));
      assert(find_pair(out, p, "b.c", "c.c"));
      /* canonical ordering: no pair has a > b */
      for (int i = 0; i < p; i++)
         assert(strcmp(out[i].a, out[i].b) < 0);
   }

   /* 2. Duplicates collapse before pairing: {a,b,a} -> one pair. */
   {
      const char *const src[] = {"a.c", "b.c", "a.c", NULL};
      int n = load(names, src);
      int p = cochange_pairs_for_commit(names, n, 25, out, 64);
      assert(p == 1);
      assert(find_pair(out, p, "a.c", "b.c"));
   }

   /* 3. Lone file has no pair. */
   {
      const char *const src[] = {"solo.c", NULL};
      int n = load(names, src);
      assert(cochange_pairs_for_commit(names, n, 25, out, 64) == 0);
   }

   /* 4. Bulk-commit gate: distinct count above max_files yields nothing. */
   {
      const char *const src[] = {"a.c", "b.c", "c.c", NULL};
      int n = load(names, src);
      assert(cochange_pairs_for_commit(names, n, 2, out, 64) == 0); /* 3 > max_files 2 */
   }

   /* 5. Duplicates do not count toward the gate: {a,a,b} passes max_files=2. */
   {
      const char *const src[] = {"a.c", "a.c", "b.c", NULL};
      int n = load(names, src);
      assert(cochange_pairs_for_commit(names, n, 2, out, 64) == 1);
   }

   /* 6. out_cap truncates without overflow. */
   {
      const char *const src[] = {"a.c", "b.c", "c.c", NULL};
      int n = load(names, src);
      assert(cochange_pairs_for_commit(names, n, 25, out, 2) == 2);
   }

   /* 7. Object-id validation: only lowercase hex 4..64 is a safe marker; anything
    * that could carry shell metacharacters (or wrong case/length) is rejected so
    * it never reaches the git command in index_backfill_cochange. */
   assert(cochange_is_hex_sha("a1b2c3d4"));
   assert(cochange_is_hex_sha("0123456789abcdef0123456789abcdef01234567")); /* 40-char */
   assert(!cochange_is_hex_sha(""));
   assert(!cochange_is_hex_sha(NULL));
   assert(!cochange_is_hex_sha("abc"));              /* too short (<4) */
   assert(!cochange_is_hex_sha("DEADBEEF"));         /* uppercase not a git id */
   assert(!cochange_is_hex_sha("abc123; rm -rf /")); /* command injection */
   assert(!cochange_is_hex_sha("$(touch pwned)"));   /* command substitution */
   assert(!cochange_is_hex_sha("HEAD"));             /* refname, not an object id */
   {
      char toolong[80];
      memset(toolong, 'a', sizeof(toolong));
      toolong[65] = '\0'; /* 65 hex chars > 64 */
      assert(!cochange_is_hex_sha(toolong));
   }

   printf("test_cochange: OK\n");
   return 0;
}
