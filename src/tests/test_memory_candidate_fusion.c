/* test_memory_candidate_fusion.c — unit tests for candidate-list fusion.
 *
 * Links the real memory_candidates_merge_interleaved rather than a local
 * mirror, which is the whole reason the policy lives in its own translation
 * unit. What is tested:
 *
 *   1. Rank fairness: every list contributes its rank-0 item before any list
 *      contributes its rank-1 item.
 *   2. No starvation under a tight cap: with a cap smaller than one list, a
 *      long first list cannot consume the whole budget. This is the regression
 *      the change exists for — appending whole lists in turn let sub-query 1
 *      spend the pool and leave sub-query 2 and the later retrieval legs with
 *      nothing.
 *   3. Headroom is preserved for the legs that run after the merge: the merge
 *      never writes past `cap`, so a caller that reserves capacity keeps it.
 *   4. Dedupe by id, first occurrence wins, matching memory_append_unique.
 *   5. Ragged lists: a list that runs out is skipped, and the rest keep going.
 *   6. Degenerate inputs are no-ops rather than crashes or truncation.
 */

#include "modules/memory/memory_candidate_fusion.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define POOL 64

static void fill(memory_t *list, const int64_t *ids, int n)
{
   memset(list, 0, sizeof(*list) * (size_t)n);
   for (int i = 0; i < n; i++)
      list[i].id = ids[i];
}

static int index_of(const memory_t *pool, int count, int64_t id)
{
   for (int i = 0; i < count; i++)
      if (pool[i].id == id)
         return i;
   return -1;
}

/* 1. Rank fairness. */
static void test_interleaves_by_rank(void)
{
   memory_t a[3], b[3], primary[POOL];
   const int64_t ids_a[] = {10, 11, 12};
   const int64_t ids_b[] = {20, 21, 22};
   fill(a, ids_a, 3);
   fill(b, ids_b, 3);
   memset(primary, 0, sizeof(primary));

   memory_t *lists[] = {a, b};
   int counts[] = {3, 3};
   int n = memory_candidates_merge_interleaved(primary, 0, lists, counts, 2, POOL);

   assert(n == 6);
   const int64_t expect[] = {10, 20, 11, 21, 12, 22};
   for (int i = 0; i < 6; i++)
      assert(primary[i].id == expect[i]);

   /* Both rank-0 items precede either rank-1 item. */
   assert(index_of(primary, n, 10) < index_of(primary, n, 11));
   assert(index_of(primary, n, 20) < index_of(primary, n, 11));
   printf("  interleaves by rank: ok\n");
}

/* 2. A long first list cannot starve a later one. */
static void test_long_list_does_not_starve(void)
{
   memory_t a[32], b[4], primary[POOL];
   int64_t ids_a[32];
   for (int i = 0; i < 32; i++)
      ids_a[i] = 100 + i;
   const int64_t ids_b[] = {200, 201, 202, 203};
   fill(a, ids_a, 32);
   fill(b, ids_b, 4);
   memset(primary, 0, sizeof(primary));

   memory_t *lists[] = {a, b};
   int counts[] = {32, 4};
   /* Cap of 8 is smaller than list a alone: the pre-change merge appended all
    * of a first and list b landed nothing. */
   int n = memory_candidates_merge_interleaved(primary, 0, lists, counts, 2, 8);

   assert(n == 8);
   int from_b = 0;
   for (int i = 0; i < n; i++)
      if (primary[i].id >= 200)
         from_b++;
   assert(from_b == 4);
   printf("  long list does not starve a later one: ok\n");
}

/* 3. The merge respects a cap that reserves headroom for later legs. */
static void test_respects_reserved_headroom(void)
{
   memory_t a[32], primary[POOL];
   int64_t ids_a[32];
   for (int i = 0; i < 32; i++)
      ids_a[i] = 300 + i;
   fill(a, ids_a, 32);
   memset(primary, 0, sizeof(primary));

   memory_t *lists[] = {a};
   int counts[] = {32};
   /* Pre-seeded pool of 5, cap 20: at most 15 may be added. */
   for (int i = 0; i < 5; i++)
      primary[i].id = 900 + i;
   int n = memory_candidates_merge_interleaved(primary, 5, lists, counts, 1, 20);

   assert(n == 20);
   for (int i = 0; i < 5; i++)
      assert(primary[i].id == 900 + i); /* pre-existing candidates untouched */
   /* Nothing written past the cap. */
   for (int i = 20; i < POOL; i++)
      assert(primary[i].id == 0);
   printf("  respects reserved headroom: ok\n");
}

/* 4. Dedupe by id, first occurrence wins. */
static void test_dedupes_by_id(void)
{
   memory_t a[3], b[3], primary[POOL];
   const int64_t ids_a[] = {1, 2, 3};
   const int64_t ids_b[] = {2, 3, 4}; /* overlaps a */
   fill(a, ids_a, 3);
   fill(b, ids_b, 3);
   memset(primary, 0, sizeof(primary));
   primary[0].id = 1; /* already present */

   memory_t *lists[] = {a, b};
   int counts[] = {3, 3};
   int n = memory_candidates_merge_interleaved(primary, 1, lists, counts, 2, POOL);

   assert(n == 4); /* 1 (pre-existing) + 2, 3, 4 */
   for (int64_t id = 1; id <= 4; id++)
      assert(index_of(primary, n, id) >= 0);
   for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++)
         assert(primary[i].id != primary[j].id);
   printf("  dedupes by id: ok\n");
}

/* 5. Ragged lists: an exhausted list is skipped, the rest keep contributing. */
static void test_ragged_lists(void)
{
   memory_t a[1], b[3], primary[POOL];
   const int64_t ids_a[] = {50};
   const int64_t ids_b[] = {60, 61, 62};
   fill(a, ids_a, 1);
   fill(b, ids_b, 3);
   memset(primary, 0, sizeof(primary));

   memory_t *lists[] = {a, b};
   int counts[] = {1, 3};
   int n = memory_candidates_merge_interleaved(primary, 0, lists, counts, 2, POOL);

   assert(n == 4);
   const int64_t expect[] = {50, 60, 61, 62};
   for (int i = 0; i < 4; i++)
      assert(primary[i].id == expect[i]);
   printf("  ragged lists: ok\n");
}

/* 6. Degenerate inputs return the pool unchanged. */
static void test_degenerate_inputs(void)
{
   memory_t a[2], primary[POOL];
   const int64_t ids_a[] = {70, 71};
   fill(a, ids_a, 2);
   memset(primary, 0, sizeof(primary));

   memory_t *lists[] = {a};
   int counts[] = {2};
   memory_t *empty_lists[] = {NULL};
   int empty_counts[] = {0};

   assert(memory_candidates_merge_interleaved(NULL, 0, lists, counts, 1, POOL) == 0);
   assert(memory_candidates_merge_interleaved(primary, 3, NULL, counts, 1, POOL) == 3);
   assert(memory_candidates_merge_interleaved(primary, 3, lists, NULL, 1, POOL) == 3);
   assert(memory_candidates_merge_interleaved(primary, 3, lists, counts, 0, POOL) == 3);
   assert(memory_candidates_merge_interleaved(primary, 3, lists, counts, 1, 0) == 3);
   /* A full pool takes nothing more. */
   assert(memory_candidates_merge_interleaved(primary, POOL, lists, counts, 1, POOL) == POOL);
   /* A NULL list among real ones is skipped, not dereferenced. */
   assert(memory_candidates_merge_interleaved(primary, 0, empty_lists, empty_counts, 1, POOL) == 0);
   printf("  degenerate inputs: ok\n");
}

int main(void)
{
   printf("test_memory_candidate_fusion:\n");
   test_interleaves_by_rank();
   test_long_list_does_not_starve();
   test_respects_reserved_headroom();
   test_dedupes_by_id();
   test_ragged_lists();
   test_degenerate_inputs();
   printf("test_memory_candidate_fusion: all passed\n");
   return 0;
}
