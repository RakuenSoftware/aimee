/* memory_candidate_fusion.c — merging several ranked candidate lists into one.
 *
 * The candidate array handed to ranking is small (96 slots) and several
 * retrieval legs write into it in sequence. Merging one whole list at a time
 * spends the remaining capacity on whichever list runs first: a single
 * retrieval pass can fill the pool alone, so later lists — and the legs that
 * run after the merge — can be left with nothing at all. That is arrival order
 * deciding what the reader is allowed to see, not a ranking judgement, and it
 * is invisible downstream because the evicted rows never existed as candidates.
 *
 * Interleaving by rank shares a fixed budget across lists instead. Paired
 * measurements on multi-hop retrieval put naive parallel-and-pool sub-query
 * expansion *below* running no expansion at all, and interleaved fusion well
 * above both.
 *
 * Deliberately does not call memory_append_unique: that lives in the retrieval
 * TU, which links db2 and libpq. Keeping the dedupe here is what lets the
 * fusion policy be tested without a store. The contract is the same — first
 * occurrence of an id wins, later duplicates are dropped. */

#include "memory_candidate_fusion.h"

static int fusion_contains_id(const memory_t *pool, int count, int64_t id)
{
   for (int i = 0; i < count; i++)
      if (pool[i].id == id)
         return 1;
   return 0;
}

int memory_candidates_merge_interleaved(memory_t *primary, int primary_count,
                                        memory_t *const *lists, const int *list_counts, int n_lists,
                                        int cap)
{
   if (!primary || !lists || !list_counts || n_lists <= 0 || cap <= 0)
      return primary_count;
   if (primary_count < 0)
      primary_count = 0;

   int deepest = 0;
   for (int i = 0; i < n_lists; i++)
      if (list_counts[i] > deepest)
         deepest = list_counts[i];

   for (int depth = 0; depth < deepest && primary_count < cap; depth++)
      for (int i = 0; i < n_lists && primary_count < cap; i++)
      {
         if (!lists[i] || depth >= list_counts[i])
            continue;
         const memory_t *candidate = &lists[i][depth];
         if (fusion_contains_id(primary, primary_count, candidate->id))
            continue;
         primary[primary_count++] = *candidate;
      }

   return primary_count;
}
