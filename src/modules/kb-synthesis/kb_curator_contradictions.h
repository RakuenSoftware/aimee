/* kb_curator_contradictions.h: deep-curator detect_contradictions pass.
 *
 * Finds committed claims that assert conflicting values for the same
 * subject+attribute and links them with `contradicts` artifact_links. No DB1
 * access. */
#ifndef KB_CURATOR_CONTRADICTIONS_H
#define KB_CURATOR_CONTRADICTIONS_H

#include "kb_curator_extract.h" /* kb_curator_extract_opts_t */

/* Write `contradicts` links for up to a bounded batch of not-yet-linked claim
 * pairs (same subject+attribute, different value). Returns 1 if it wrote at
 * least one link this call, 0 if there was nothing new (or no DB). */
int kb_curator_detect_contradictions_one(const kb_curator_extract_opts_t *opts);

#endif /* KB_CURATOR_CONTRADICTIONS_H */
