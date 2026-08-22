/* test_memory_lanes.c — unit tests for two-lane retrieval floor logic.
 *
 * What is tested:
 *   Floor algorithm invariant: after apply_floor, at least floor_n items from
 *      the lane are present in the top-`total` window.  Verified with a local
 *      reimplementation that mirrors the production logic.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Floor algorithm invariant. */

/* Local mirror of memory_apply_lane_floor to test the contract independently. */
typedef struct
{
   int64_t id;
} test_mem_t;

static int local_apply_floor(test_mem_t *matches, int count, const int64_t *lane_ids,
                             int lane_count, int floor_n, int total)
{
   if (floor_n <= 0 || count <= 0 || lane_count <= 0 || total <= 0)
      return total;
   if (total > count)
      total = count;

   int lane_in_top = 0;
   for (int i = 0; i < total; i++)
      for (int j = 0; j < lane_count; j++)
         if (matches[i].id == lane_ids[j])
         {
            lane_in_top++;
            break;
         }
   if (lane_in_top >= floor_n)
      return total;

   int needed = floor_n - lane_in_top;
   for (int tail = total; tail < count && needed > 0; tail++)
   {
      int is_lane = 0;
      for (int j = 0; j < lane_count; j++)
         if (matches[tail].id == lane_ids[j])
         {
            is_lane = 1;
            break;
         }
      if (!is_lane)
         continue;
      int victim = total - 1;
      test_mem_t tmp = matches[victim];
      matches[victim] = matches[tail];
      matches[tail] = tmp;
      total--;
      needed--;
   }
   return total;
}

static int count_lane_in_top(const test_mem_t *matches, int total, const int64_t *lane_ids,
                             int lane_count)
{
   int n = 0;
   for (int i = 0; i < total; i++)
      for (int j = 0; j < lane_count; j++)
         if (matches[i].id == lane_ids[j])
         {
            n++;
            break;
         }
   return n;
}

static void test_floor_already_met(void)
{
   test_mem_t matches[] = {{1}, {2}, {3}, {4}, {5}};
   int64_t lane[] = {1, 2};
   (void)local_apply_floor(matches, 5, lane, 2, 2, 4);
   /* 2 lane members already in top-4; nothing should change */
   assert(count_lane_in_top(matches, 4, lane, 2) >= 2);
   printf("  floor_already_met: ok\n");
}

static void test_floor_promotes_from_tail(void)
{
   /* Lane IDs are 10, 20 — neither are in the top-3; floor=2 → both must be promoted */
   test_mem_t matches[] = {{1}, {2}, {3}, {10}, {20}};
   int64_t lane[] = {10, 20};
   (void)local_apply_floor(matches, 5, lane, 2, 2, 3);
   assert(count_lane_in_top(matches, 3, lane, 2) >= 2);
   printf("  floor_promotes_from_tail: ok\n");
}

static void test_floor_partial_promote(void)
{
   /* Lane ID 10 is already at position 2 (in top-4); need 2 total → must promote 20 */
   test_mem_t matches[] = {{1}, {2}, {10}, {3}, {20}};
   int64_t lane[] = {10, 20};
   (void)local_apply_floor(matches, 5, lane, 2, 2, 4);
   assert(count_lane_in_top(matches, 4, lane, 2) >= 2);
   printf("  floor_partial_promote: ok\n");
}

static void test_floor_not_enough_lane_members_in_tail(void)
{
   /* Lane has 1 member in tail; floor=3; can only promote 1 → should not crash */
   test_mem_t matches[] = {{1}, {2}, {3}, {10}};
   int64_t lane[] = {10};
   (void)local_apply_floor(matches, 4, lane, 1, 3, 3);
   /* At most 1 lane member can be in top-3 */
   int n = count_lane_in_top(matches, 3, lane, 1);
   assert(n >= 0 && n <= 1);
   printf("  floor_not_enough_tail: ok\n");
}

static void test_floor_total_equals_count(void)
{
   /* total == count: tail is empty, floor cannot be met even if not already */
   test_mem_t matches[] = {{1}, {2}, {3}};
   int64_t lane[] = {99};
   (void)local_apply_floor(matches, 3, lane, 1, 1, 3);
   /* No crash, no out-of-bounds */
   printf("  floor_total_equals_count: ok\n");
}

static void test_floor_zero_disables(void)
{
   /* floor_n=0 must leave the array unchanged */
   test_mem_t matches[] = {{1}, {2}, {3}, {10}};
   int64_t lane[] = {10};
   test_mem_t orig[4];
   memcpy(orig, matches, sizeof(matches));
   (void)local_apply_floor(matches, 4, lane, 1, 0, 3);
   assert(memcmp(matches, orig, sizeof(matches)) == 0);
   printf("  floor_zero_disables: ok\n");
}

static void test_floor_two_lanes_independent(void)
{
   /* Two independent lanes: summary (IDs 10, 20) and fact (IDs 30, 40).
    * Chain the calls so the second floor does not displace the first.
    * After both floor passes (using the chained effective-total), the original
    * top-5 window contains >= 2 members of each lane. */
   test_mem_t matches[] = {{1}, {2}, {3}, {4}, {5}, {10}, {20}, {30}, {40}};
   int64_t summary_lane[] = {10, 20};
   int64_t fact_lane[] = {30, 40};
   int eff = local_apply_floor(matches, 9, summary_lane, 2, 2, 5);
   (void)local_apply_floor(matches, 9, fact_lane, 2, 2, eff);
   assert(count_lane_in_top(matches, 5, summary_lane, 2) >= 2);
   assert(count_lane_in_top(matches, 5, fact_lane, 2) >= 2);
   printf("  floor_two_lanes_independent: ok\n");
}

/* ---- main ---- */

int main(void)
{
   printf("memory_lanes:\n");

   printf("  floor_invariants:\n");
   test_floor_already_met();
   test_floor_promotes_from_tail();
   test_floor_partial_promote();
   test_floor_not_enough_lane_members_in_tail();
   test_floor_total_equals_count();
   test_floor_zero_disables();
   test_floor_two_lanes_independent();

   printf("All memory_lanes tests passed.\n");

   return 0;
}
