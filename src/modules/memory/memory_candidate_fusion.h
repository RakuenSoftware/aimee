/* memory_candidate_fusion.h — how several ranked candidate lists become one.
 *
 * Split out of the retrieval TU on purpose. memory_core_search_c.c links db2,
 * libpq and the embedder client, so nothing there can be unit-tested without
 * standing up a store; this policy is pure and belongs where a test can reach
 * it. See memory_candidate_fusion.c for why the policy is what it is. */
#ifndef AIMEE_MEMORY_CANDIDATE_FUSION_H
#define AIMEE_MEMORY_CANDIDATE_FUSION_H

/* memory.h is not self-contained: it needs the fixed-width and size types plus
 * the shared constants aimee.h defines. Include its prelude here so this header
 * can be included first, which is what lets the unit test compile the fusion
 * policy on its own. */
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "aimee.h"
#include "memory.h"

/* Round-robin merge of `n_lists` ranked candidate lists into `primary`.
 *
 * Takes rank 0 from every list, then rank 1, and so on, appending each item to
 * `primary` unless an entry with the same id is already there, and stopping at
 * `cap`. Returns the new primary count.
 *
 * Each list must already be in its own descending relevance order. Scores are
 * never compared across lists: the legs that produce them (lexical, dense,
 * graph, per-sub-query) do not share a score scale, and interleaving by rank is
 * exactly the fusion that does not require them to. */
int memory_candidates_merge_interleaved(memory_t *primary, int primary_count,
                                        memory_t *const *lists, const int *list_counts, int n_lists,
                                        int cap);

#endif /* AIMEE_MEMORY_CANDIDATE_FUSION_H */
