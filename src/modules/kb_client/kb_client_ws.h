/* kb_client_ws.h: aimee-server subscriber to the aimee-kb /v1/events stream.
 *
 * Starts a background thread that opens a WebSocket to the configured remote kb
 * (AIMEE_KB_API_URL) at GET /v1/events and flushes the result cache
 * (kb_client_cache.c) on every invalidation event, reconnecting with backoff.
 * No-op unless the result cache is enabled and an HTTP(S) kb URL is set. */
#ifndef DEC_KB_CLIENT_WS_H
#define DEC_KB_CLIENT_WS_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Start the /v1/events subscriber thread (idempotent; safe to call once at
    * server startup). No-op when the cache is disabled or no HTTP kb is set. */
   void kb_client_ws_start(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_CLIENT_WS_H */
