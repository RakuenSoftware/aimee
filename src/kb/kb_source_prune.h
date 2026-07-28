/* kb_source_prune.h: physical garbage collection for retired source generations. */
#ifndef AIMEE_KB_SOURCE_PRUNE_H
#define AIMEE_KB_SOURCE_PRUNE_H

/* Claim and prune at most max_generations due generations. Returns the number
 * finalized, or -1 only when the candidate store itself is unavailable. Store
 * failures release a candidate for retry and do not make it retrievable. */
int kb_source_prune_sweep(int max_generations);

#endif /* AIMEE_KB_SOURCE_PRUNE_H */
