/* td_search_render.h: render the /v1/search {"hits":[...]} shape into the text
 * result the kb_search tool returns, and extract (doc_id, excerpt) pairs for the
 * learning-to-rank outcome capture. Pure (cJSON + dstr only) so both are unit
 * tested independently of the agent tool loop. */
#ifndef DEC_TD_SEARCH_RENDER_H
#define DEC_TD_SEARCH_RENDER_H 1

#include "cJSON.h"
#include <stdint.h>

/* Retrieval is not a boolean.  A healthy empty result, a degraded dependency,
 * and a malformed/failed response lead to different next actions. */
typedef enum
{
   TD_RETRIEVAL_FOUND = 0,
   TD_RETRIEVAL_EMPTY,
   TD_RETRIEVAL_DEGRADED,
   TD_RETRIEVAL_FAILED
} td_retrieval_outcome_t;

const char *td_retrieval_outcome_name(td_retrieval_outcome_t outcome);

/* Render a bounded, machine-readable continuation offer.  It is advice, not an
 * authorization: policy_recheck is always true and authorized is always false.
 * The returned string is owned by the caller. */
char *td_render_retrieval_continuation(td_retrieval_outcome_t outcome, const char *source,
                                       const char *query, const char *message);

/* Render a /v1/search hits array into a concise text block for the tool result.
 * Returns a malloc'd string (caller frees), or NULL on OOM. A NULL/empty/non-array
 * `hits` yields a "no results" line rather than an error. */
char *td_render_search_hits(const cJSON *hits, const char *query);

/* Extract up to `max` (doc_id, excerpt) pairs from `hits` into ids[]/snips[].
 * snips[] point INTO the hits cJSON (valid until it is freed). Returns the count. */
int td_extract_hit_docs(const cJSON *hits, int64_t *ids, const char **snips, int max);

/* Select the kb_search tool's text result from a parsed /v1/search response:
 *   - a legacy {"result":"<text>"} string, if one is present (back-compat), else
 *   - the rendered {"hits":[...]} array, else
 *   - an "error: knowledge search unavailable" line.
 * Returns a malloc'd string (caller frees), or NULL on OOM. This is the exact
 * result-vs-hits selection whose earlier breakage (reading {result} off the
 * {hits}-only endpoint) silently disabled the tool — so it is unit tested, and
 * driven with the real handler output by the contract test. */
char *td_search_result_from_response(const cJSON *resp, const char *query);

#endif /* DEC_TD_SEARCH_RENDER_H */
