/* web_extract.h -- deterministic page extraction, shared by the page reader and
 * the search-fusion path.
 *
 * Locate the query's occurrences, widen each to a readable window, merge
 * overlaps, select by distinct-query-term coverage, emit in document order until
 * the budget is spent. No chunker, no ranker, no model.
 *
 * POSIX ONLY. The implementation lives with the page reader, which needs the
 * pinned-connect egress path that exists only on posix. Search guards its calls
 * accordingly rather than varying behaviour silently. */
#ifndef DEC_WEB_EXTRACT_H
#define DEC_WEB_EXTRACT_H 1

#include <stddef.h>

/* Extract query-relevant spans from `text` within `budget` bytes.
 *
 * TAKES OWNERSHIP of `text` and frees it on every path. `ref` is the citation
 * prefix (spans are emitted as `<ref>#<n>`). `url` is used only for a log line.
 * Returns a malloc'd, caller-freed block; never NULL on success. */
char *web_extract_spans(char *text, const char *ref, const char *query, size_t budget,
                        const char *url);

/* Strip HTML to visible text. Returns a malloc'd string the caller frees. */
char *web_extract_html_to_text(const char *html);

#endif /* DEC_WEB_EXTRACT_H */
