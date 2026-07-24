/* kb_curator_index_narrative.h: deep-curator narrative vector indexer.
 *
 * Embeds proposed doc_summary / synthesis / open_question artifacts and upserts
 * them into curator_narrative_vectors for filter-first doc retrieval, then
 * commits them. No DB1 access. */
#ifndef KB_CURATOR_INDEX_NARRATIVE_H
#define KB_CURATOR_INDEX_NARRATIVE_H

#include "kb_curator_extract.h" /* kb_curator_extract_opts_t */

/* Index one proposed narrative-kind artifact into curator_narrative_vectors and
 * commit it. Returns 1 if one was processed, 0 if none were pending (or no DB). */
int kb_curator_index_narrative_one(const kb_curator_extract_opts_t *opts);

#endif /* KB_CURATOR_INDEX_NARRATIVE_H */
