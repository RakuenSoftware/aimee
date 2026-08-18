/* db1/web_page_cache.h: fetched web pages, stripped to text, keyed by URL.
 *
 * WHY THE KEY IS THE URL AND NOT (url, query, budget)
 *
 * An earlier record argued a retrieval cache must key on the query and budget,
 * because retrieval output is a function of them. That was correct for a
 * chunk-and-rank extractor, where the policy decision is frozen at write time.
 * Extraction is now a deterministic pure function over the stripped text, re-run
 * on every read, so (query, budget) are REAPPLIED at read time rather than
 * baked in. The cache supplies the document; it never stores a policy decision.
 *
 * The consequences are all in this direction: any query against a
 * previously-fetched page hits, changing the extractor invalidates nothing, and
 * the key has no version to bump.
 *
 * THE EGRESS INVARIANT
 *
 * A page can only enter this cache after its resolved address passed the deny
 * list and the connection was pinned to that address. `pinned_addr` records
 * which address that was. A hit re-checks it against the CURRENT deny-list, so
 * tightening policy retroactively invalidates entries it would now refuse. No
 * DNS and no connection happen on a hit -- resolving there would give a host
 * that flipped public-to-private the power to add hit-path latency.
 *
 * FAILURE POSTURE
 *
 * Every function here fails soft. A cache error is a miss, always; it never
 * fails a fetch that would otherwise succeed. The cache is an optimisation, not
 * a dependency. */
#ifndef DEC_DB1_WEB_PAGE_CACHE_H
#define DEC_DB1_WEB_PAGE_CACHE_H 1

/* How wide a pinned address may be. It crosses the module boundary, so the
   contract states it rather than each caller sizing its own buffer. */
#define DB1_WEB_PAGE_ADDR_LEN 64

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Entries older than this are treated as misses.
 *
 * A PLACEHOLDER, not a finding. There is no volatility classifier and no
 * freshness measurement; 6 hours is long enough that a refined search within a
 * working session hits, and short enough that stale content rolls off within a
 * day. Revise it when there is a hit-rate or staleness measurement -- do not
 * document it as justified. */
#define DB1_WEB_PAGE_TTL_SECONDS (6 * 3600)

/* Advisory total-bytes cap. Bounded by BYTES rather than row count because a
 * thousand archived pages and a thousand news pages cost very differently.
 * Also a guess: real pages measured at a 139KB median, so this is roughly 1,800
 * of them. Exceeding it evicts least-recently-used entries; it never refuses a
 * write, because refusing would silently disable caching for large pages. */
#define DB1_WEB_PAGE_MAX_BYTES (256u * 1024u * 1024u)

   /* Look up stripped page text for `url`.
    *
    * Returns a malloc'd body the caller frees, or NULL on miss/expiry/error.
    * On a hit, *age_secs receives the entry's age and *pinned_addr_out (when
    * non-NULL, of at least 64 bytes) receives the address validated at fetch
    * time so the caller can re-check it against current egress policy.
    *
    * A hit refreshes the entry's LRU position. */
   char *db1_web_page_get(const char *url, long *age_secs, char *pinned_addr_out,
                          size_t pinned_addr_len);

   /* Store stripped page text. `pinned_addr` is the address the egress guard
    * validated and connected to. Overwrites any existing entry for `url`.
    * Returns 0 on success, -1 on error -- callers ignore the result, because a
    * failed write must never fail the fetch that produced the body. */
   int db1_web_page_put(const char *url, const char *body, const char *pinned_addr);

   /* Drop the entry for `url` (used when a hit fails re-validation). */
   void db1_web_page_drop(const char *url);

   /* Canonical cache key for `url`: lowercase scheme and host, default port
    * suppressed, fragment stripped, path and query preserved verbatim. Writes
    * to `out`; returns 0 on success, -1 if the URL cannot be canonicalised. */
   int db1_web_page_canonical_url(const char *url, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_WEB_PAGE_CACHE_H */
