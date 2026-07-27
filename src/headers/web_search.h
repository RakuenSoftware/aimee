/*
 * web_search.h: web search tool for delegates.
 *
 * Supports three backends: duckduckgo (default, no key), searxng (self-hosted),
 * and tavily (requires API key). Backend is selected via config.
 */
#ifndef DEC_WEB_SEARCH_H
#define DEC_WEB_SEARCH_H 1

/* Maximum number of results that can be requested in one call */
#define WEB_SEARCH_MAX_RESULTS 10

/* Maximum bytes in the formatted output block before truncation */
#define WEB_SEARCH_MAX_OUTPUT_BYTES 8192

/* Pass as `fetch_pages` to mean "the caller has no opinion" -- config decides,
 * falling back to WEB_SEARCH_FETCH_PAGES_DEFAULT. Distinct from an explicit 0,
 * which means the caller really does want snippets only. */
#define WEB_SEARCH_FETCH_PAGES_UNSET (-1)

/* Built-in default for page fetching: ON.
 *
 * A search that returns the answer beats one that returns links to the answer,
 * and the alternative costs a whole extra agent turn to read one of the results.
 * The price is latency -- bounded by the per-page and total deadlines in
 * web_search.c, with partial results tolerated -- and it is why `fetch_pages:
 * false` on the tool, or `search.fetch_pages: false` in config, exists. */
#define WEB_SEARCH_FETCH_PAGES_DEFAULT 1

typedef struct
{
   char *title;
   char *url;
   char *snippet;
} web_search_result_t;

/*
 * web_search: run a search query via the configured backend.
 *
 * query       — search string (will be URL-encoded internally)
 * max_results — how many results to request (clamped to WEB_SEARCH_MAX_RESULTS)
 *
 * Returns a malloc'd, human-readable text block for the delegate context, or
 * an error string prefixed with "error:". Never returns NULL.
 * The caller must free() the returned string.
 */
char *web_search(const char *query, int max_results);

/* web_search with optional page fusion.
 *
 * fetch_pages   non-zero: fetch the top results through the guarded egress path
 *               and append query-relevant extracted spans after the (unchanged)
 *               snippet block. Off by default -- fetching pages turns a ~1s
 *               search into several seconds.
 * extract_query optional; when NULL or empty the search query is used. Useful
 *               when the caller wants something the search terms do not express,
 *               e.g. "return the README verbatim".
 *
 * Posix only for the fusion half: it needs the pinned-connect egress path. On
 * Windows fetch_pages is ignored and the snippet block is returned unchanged. */
char *web_search_ex(const char *query, int max_results, int fetch_pages, const char *extract_query);

/*
 * web_search_format_results: build the text block from a result array.
 * Exposed for unit tests.
 *
 * results / count — result array populated by a backend
 * max_bytes       — truncate output at this many bytes (0 = WEB_SEARCH_MAX_OUTPUT_BYTES)
 *
 * Returns a malloc'd string. Never returns NULL.
 */
char *web_search_format_results(const web_search_result_t *results, int count, int max_bytes);

/*
 * web_search_url_encode: percent-encode a string for use in a query parameter.
 * Returns a malloc'd string. Never returns NULL.
 */
char *web_search_url_encode(const char *s);

/*
 * web_search_parse_duckduckgo: extract results from DuckDuckGo HTML response.
 * Exposed for unit tests.
 *
 * html        — raw HTML body
 * max_results — maximum items to populate
 * out         — caller-allocated array of at least max_results entries (zeroed)
 *
 * Returns number of results populated. Strings in out are malloc'd; caller
 * must free each non-NULL title/url/snippet, or call web_search_free_results().
 */
int web_search_parse_duckduckgo(const char *html, int max_results, web_search_result_t *out);

/*
 * web_search_free_results: free malloc'd strings inside a result array.
 */
void web_search_free_results(web_search_result_t *results, int count);

#endif /* DEC_WEB_SEARCH_H */
