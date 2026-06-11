/* request_context.h: per-request identity/transport context for ingress (#3).
 *
 * The HTTP front-end (handle_conn) sees the request id, transport, peer
 * credentials, and a few behaviour-affecting headers (idempotency key, an
 * optional proxy-stamped principal) that the buffered ingress handlers in
 * openai_chat.c / anthropic_http.c never receive — their signatures take only
 * the body. This module carries that context on a thread-local so those
 * handlers, the audit writer, and the §4 dedup key can read it without
 * threading it through every call.
 *
 * Thread model mirrors g_rpc_conn_caps / the ingress-source override: each
 * connection is handled on its own worker thread and the synchronous handlers
 * read the context on that same thread. handle_conn sets it before routing and
 * clears it after. Async paths (the /v1/runs worker) must copy what they need
 * before returning, since the thread-local is cleared at the end of the request.
 *
 * SECURITY: a client-supplied principal/source is only honoured on a *trusted*
 * transport (the local Unix-domain socket, or — when explicitly configured — a
 * trusted proxy bearer). On an untrusted TCP request these headers are ignored
 * so a remote caller cannot spoof another account's attribution. */
#ifndef DEC_REQUEST_CONTEXT_H
#define DEC_REQUEST_CONTEXT_H 1

#include <stddef.h>

typedef enum
{
   REQ_TRANSPORT_UNKNOWN = 0,
   REQ_TRANSPORT_UDS = 1, /* local Unix-domain socket (same-host, trusted) */
   REQ_TRANSPORT_TCP = 2, /* network listener (bearer-scoped) */
} req_transport_t;

typedef struct
{
   char request_id[64];       /* echoed X-Request-ID / generated <pid>-<seq> */
   char idempotency_key[128]; /* Idempotency-Key header, empty if absent */
   char principal[128];       /* account/tenant boundary; empty = anonymous */
   char source[64];           /* turn-origin tag for token_audit, empty = default */
   long peer_uid;             /* UDS peer uid (SO_PEERCRED), -1 if unknown */
   req_transport_t transport;
   int trusted; /* 1 = principal/source headers may be honoured */
} request_context_t;

/* Set the active request context for the current thread (shallow copy). Pass
 * NULL to clear. */
void request_context_set(const request_context_t *ctx);

/* Return the current thread's request context, or NULL if none is set. The
 * pointer is valid until the next set/clear on this thread. */
const request_context_t *request_context_get(void);

/* Clear the current thread's request context. */
void request_context_clear(void);

/* Convenience: the idempotency key for the active request, or "" if none / no
 * context. Never returns NULL. */
const char *request_context_idempotency_key(void);

/* Convenience: the principal for the active request, or "" if none / no
 * context / untrusted. Never returns NULL. */
const char *request_context_principal(void);

#endif /* DEC_REQUEST_CONTEXT_H */
