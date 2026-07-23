/* web_search_fuse.h -- deduplicate and re-rank search results by URL.
 *
 * Several engines asked the same question return overlapping pages in different
 * orders. Two separate jobs live here, and only the first one needs more than
 * one engine to be worth doing:
 *
 *   DEDUP. The same page must appear once. This pays off within a SINGLE
 *   engine's results too -- a scraped result page can list the same URL twice --
 *   so it is not gated on fanout being configured.
 *
 *   FUSION. When several engines DO rank the same page, that agreement is
 *   evidence, and Reciprocal Rank Fusion is the standard way to use it. Unlike
 *   the intra-page ranking this project deleted, these really are independent
 *   ranked lists over the same items, which is exactly RRF's premise.
 *
 * IDENTITY IS THE CACHE'S IDENTITY. Dedup uses db1_web_page_canonical_url, the
 * same normalisation the page cache keys on. Deliberate: if two URLs are the
 * same page for caching they are the same page for dedup, and one rule cannot
 * drift from the other. It is conservative -- it does NOT strip `www.`, drop
 * tracking parameters, sort query parameters, or unify http with https. A false
 * merge silently loses a distinct result; a missed merge costs one duplicate
 * line. Those are not equal, so this errs toward the cheap failure. */
#ifndef DEC_WEB_SEARCH_FUSE_H
#define DEC_WEB_SEARCH_FUSE_H 1

#include "web_search.h"

#include <stddef.h>

/* Most engine lists we will ever fuse at once. */
#define WEB_SEARCH_MAX_ENGINES 4

/* Fuse `n_lists` ranked result lists into `out` (capacity `max`).
 *
 * Position in each list IS its rank. `lists[i]` may be NULL or `counts[i]` 0
 * (an engine that failed or returned nothing) and is simply skipped -- a dead
 * engine must not sink the results of a live one.
 *
 * Results are ordered by fused score descending. For a URL returned by several
 * engines, the title and snippet kept are the FIRST-SEEN ones (earliest list,
 * then best rank within it): a deterministic choice, not a quality judgement.
 *
 * Strings in `out` are freshly malloc'd; free the array with
 * web_search_free_results(). Returns the number written, or -1 on a bad
 * argument. `out` is untouched on -1. */
int web_search_fuse(const web_search_result_t *const *lists, const int *counts, int n_lists,
                    web_search_result_t *out, int max);

/* Canonical identity used for dedup, exposed for tests. Writes to `out`.
 * Returns 0, or -1 when `url` cannot be canonicalised (in which case the caller
 * must treat the URL as its own distinct identity rather than merging it with
 * every other unparseable URL). */
int web_search_dedup_key(const char *url, char *out, size_t out_len);

#endif /* DEC_WEB_SEARCH_FUSE_H */
