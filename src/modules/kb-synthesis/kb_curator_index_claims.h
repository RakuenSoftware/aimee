/* kb_curator_index_claims.h: deep-curator claim vector indexer.
 *
 * Embeds proposed `claim` artifacts into curator_claim_vectors with two named
 * vectors (subject+attribute, value) for contradiction mining, then commits
 * them. No DB1 access. */
#ifndef KB_CURATOR_INDEX_CLAIMS_H
#define KB_CURATOR_INDEX_CLAIMS_H

#include "kb_curator_extract.h" /* kb_curator_extract_opts_t */

/* Index one proposed `claim` artifact into curator_claim_vectors and commit it.
 * Returns 1 if one was processed, 0 if none were pending (or no DB). */
int kb_curator_index_claims_one(const kb_curator_extract_opts_t *opts);

#endif /* KB_CURATOR_INDEX_CLAIMS_H */
