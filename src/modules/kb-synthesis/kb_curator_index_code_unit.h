/* kb_curator_index_code_unit.h: deep-curator code_unit vector indexer.
 *
 * Embeds proposed `code_unit` artifacts into curator_code_unit_vectors with
 * three named vectors (intent, signature, body), then commits them. No DB1
 * access. */
#ifndef KB_CURATOR_INDEX_CODE_UNIT_H
#define KB_CURATOR_INDEX_CODE_UNIT_H

#include "kb_curator_extract.h" /* kb_curator_extract_opts_t */

/* Index one proposed `code_unit` artifact into curator_code_unit_vectors and
 * commit it. Returns 1 if one was processed, 0 if none were pending (or no DB). */
int kb_curator_index_code_unit_one(const kb_curator_extract_opts_t *opts);

#endif /* KB_CURATOR_INDEX_CODE_UNIT_H */
