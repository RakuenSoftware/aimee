/* kb_client_cache.h: aimee-server's short-TTL LRU for aimee-kb read results.
 *
 * Caches kb responses (currently /v1/search) keyed by request parameters, with
 * a TTL ceiling. The kb pushes invalidation events over GET /v1/events
 * (kb_client_ws.c); on each event the cache is flushed so retrieval results
 * never outlive a release promote/rollback or new ingest by more than the
 * round-trip. The cache is OFF unless a TTL is configured (AIMEE_KB_CACHE_TTL_S
 * or cfg), so default behavior is unchanged. Thread-safe. */
#ifndef DEC_KB_CLIENT_CACHE_H
#define DEC_KB_CLIENT_CACHE_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Configure the cache TTL in seconds (0 disables). Reads AIMEE_KB_CACHE_TTL_S
    * when ttl_s < 0. Call once at startup. */
   void kb_cache_configure(int ttl_s);

   /* Cache enabled (TTL > 0)? */
   int kb_cache_enabled(void);

   /* Return a malloc'd copy of the cached value for key (caller frees), or NULL
    * on miss / expired / disabled. Updates hit/miss counters. */
   char *kb_cache_get(const char *key);

   /* Store a copy of value under key (no-op if disabled or value NULL). */
   void kb_cache_put(const char *key, const char *value);

   /* Drop all entries (called on a kb invalidation event). */
   void kb_cache_invalidate_all(void);

   /* Snapshot hit/miss/invalidation counters. */
   void kb_cache_stats(long *hits, long *misses, long *invalidations);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_CLIENT_CACHE_H */
