/* kb_curator_link_artifacts.h: deep-curator link_artifacts (doc<->code bridge).
 *
 * Materializes `mentions` artifact_links from a code_unit to the canonical
 * entities named in its domain_concepts. Combined with the entity->document
 * citations written at extraction, this bridges documents and code through the
 * entity graph. No DB1 access. */
#ifndef KB_CURATOR_LINK_ARTIFACTS_H
#define KB_CURATOR_LINK_ARTIFACTS_H

#include "kb_curator_extract.h" /* kb_curator_extract_opts_t */

/* Link one not-yet-processed committed code_unit to the entities its
 * domain_concepts name, then stamp it processed. Returns 1 if one code_unit was
 * processed, 0 if none were pending (or no DB). */
int kb_curator_link_artifacts_one(const kb_curator_extract_opts_t *opts);

#endif /* KB_CURATOR_LINK_ARTIFACTS_H */
