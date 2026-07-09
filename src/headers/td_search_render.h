/* td_search_render.h: render the /v1/search {"hits":[...]} shape into the text
 * result the kb_search tool returns, and extract (doc_id, excerpt) pairs for the
 * learning-to-rank outcome capture. Pure (cJSON + dstr only) so both are unit
 * tested independently of the agent tool loop. */
#ifndef DEC_TD_SEARCH_RENDER_H
#define DEC_TD_SEARCH_RENDER_H 1

#include "cJSON.h"
#include <stdint.h>

/* Render a /v1/search hits array into a concise text block for the tool result.
 * Returns a malloc'd string (caller frees), or NULL on OOM. A NULL/empty/non-array
 * `hits` yields a "no results" line rather than an error. */
char *td_render_search_hits(const cJSON *hits, const char *query);

/* Extract up to `max` (doc_id, excerpt) pairs from `hits` into ids[]/snips[].
 * snips[] point INTO the hits cJSON (valid until it is freed). Returns the count. */
int td_extract_hit_docs(const cJSON *hits, int64_t *ids, const char **snips, int max);

#endif /* DEC_TD_SEARCH_RENDER_H */
