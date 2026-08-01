/* kb_curator_notify.h: outbound invalidation push (kb -> subscribed aimee-server).
 *
 * When kb marks a source doc's derived artifacts stale, it best-effort-pushes a
 * `curator.invalidated` NDJSON RPC to the configured server socket so the
 * subscriber learns of the invalidation in real time. Disabled (no-op) unless
 * kb.curator.invalidation_notify_socket is set. The durable record is the
 * curator_invalidation_events table / GET /v1/invalidations feed; this is the
 * push transport over it. No DB access. */
#ifndef DEC_KB_CURATOR_NOTIFY_H
#define DEC_KB_CURATOR_NOTIFY_H 1

/* Send a curator.invalidated event to `notify_socket` (NDJSON, newline-framed).
 * Best-effort: short timeout, failures logged at DEBUG and ignored so ingest is
 * never blocked. Returns 0 if sent or disabled (empty socket), -1 on send error.*/
int kb_curator_notify_send(const char *notify_socket, const char *source_kind,
                           const char *source_id, int artifacts_stale);

/* Loads the notify socket from config and calls kb_curator_notify_send. */
int kb_curator_notify_invalidation(const char *source_kind, const char *source_id,
                                   int artifacts_stale);

/* When a re-ingested source doc invalidates >0 derived curator artifacts,
 * fan the invalidation out to every transport: the best-effort RPC push to a
 * subscribed aimee-server (kb_curator_notify_invalidation) AND the in-process
 * /v1/events WebSocket subscriber registry (kb_ws_publish_invalidation). No-op
 * when artifacts_stale <= 0. project may be NULL. */
void kb_curator_invalidation_broadcast(const char *project, const char *file_path,
                                       int artifacts_stale);

#endif /* DEC_KB_CURATOR_NOTIFY_H */
