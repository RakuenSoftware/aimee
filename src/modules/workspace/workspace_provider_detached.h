#ifndef WORKSPACE_PROVIDER_DETACHED_H
#define WORKSPACE_PROVIDER_DETACHED_H 1

#include "workspace_provider.h"

/* The `detached` resource provider (workspace-resource-plane §2–3).
 *
 * When aimee-server runs detached from the client's filesystem (e.g. in a
 * container), it cannot open the working tree directly. The detached provider
 * implements the same read/write/stat/list/exec primitives by MARSHALLING each
 * op as a JSON request to the filesystem authority — the client-side runner —
 * and parsing its JSON response. Binary file contents cross as base64 so NULs
 * survive (mirrors the validated ssh-delegate marshalling).
 *
 * The transport that actually carries a request to the runner is pluggable:
 * production wires it to the /v1 reverse channel; tests inject an in-memory
 * mock. This module owns only the marshalling + parsing; it is transport- and
 * filesystem-agnostic, so it unit-tests standalone. */

struct cJSON;

/* Carry one marshalled op request to the filesystem authority and return its
 * response. The provider passes ownership of `request` to the transport (the
 * transport must cJSON_Delete it). On success returns 0 and sets *response to
 * a caller-owned object (the provider cJSON_Delete's it). On transport failure
 * returns -1 and must leave *response NULL. */
typedef int (*ws_detached_transport_fn)(void *ctx, struct cJSON *request, struct cJSON **response);

/* Partial callback for the streaming transport: borrowed {"partial":true,...}
 * response cJSON (do not free). Return 0 to continue, non-zero to abort. */
typedef int (*ws_detached_partial_fn)(void *cb_ctx, struct cJSON *partial);

/* Streaming variant of the transport: enqueue `request`, deliver each partial
 * response to `on_partial`, and set *final to the terminal (non-partial)
 * response (caller owns). Consumes `request`. Returns 0 on a clean final
 * response, -1 on transport failure/abort. Matches ws_runner_queue_transport_stream.
 * NULL on a provider with no streaming transport bound. */
typedef int (*ws_detached_stream_transport_fn)(void *ctx, struct cJSON *request,
                                               ws_detached_partial_fn on_partial, void *cb_ctx,
                                               struct cJSON **final);

/* A detached provider instance: the base vtable plus the transport its ops
 * marshal to. `base` is first, so &inst.base casts to workspace_provider_t*. */
typedef struct
{
   workspace_provider_t base;
   ws_detached_transport_fn transport;
   ws_detached_stream_transport_fn stream_transport; /* NULL = no exec_stream */
   void *transport_ctx;
} ws_detached_provider_t;

/* Initialize `out` as a detached provider bound to `transport`/`ctx`.
 * Afterwards &out->base is a workspace_provider_t* whose ops marshal over the
 * transport. `transport` must not be NULL. exec_stream is unavailable. */
void ws_detached_provider_init(ws_detached_provider_t *out, ws_detached_transport_fn transport,
                               void *ctx);

/* Like ws_detached_provider_init, but also binds a streaming transport so the
 * provider's exec_stream op works (`stream_transport` may be NULL to leave it
 * unsupported). Both transports share `ctx`. */
void ws_detached_provider_init_ex(ws_detached_provider_t *out, ws_detached_transport_fn transport,
                                  ws_detached_stream_transport_fn stream_transport, void *ctx);

/* The RUNNER end of the detached protocol: execute one op `request` against the
 * LOCAL filesystem (via the shared provider) and return its JSON response
 * (caller cJSON_Delete's). This is what the client-side runner — or, for the
 * bind-mount tier, the server itself — runs; it is the exact inverse of the
 * detached provider's marshalling, so the two agree on the wire format. A
 * transport built around this closes the loop. Returns NULL only on allocation
 * failure or a missing/unknown op. */
struct cJSON *ws_detached_runner_handle(const struct cJSON *request);

/* Streaming runner end: execute an `exec_stream` op `request` locally (via the
 * shared provider's exec_stream), emitting each stdout chunk as a
 * {"partial":true,"chunk":<base64>} response through `emit` as it is produced,
 * then returning the terminal {"ok":true,"rc":<n>} response (caller
 * cJSON_Delete's). `emit` takes ownership of each partial it is handed. Returns
 * NULL on allocation failure or if `request` is not an exec_stream op. */
struct cJSON *ws_detached_runner_handle_stream(const struct cJSON *request,
                                               int (*emit)(void *ctx, struct cJSON *partial),
                                               void *emit_ctx);

/* Serve one op for the runner end: fetch the next op via `fetch` (returns NULL
 * when none is pending — the caller re-polls), execute it locally via
 * ws_detached_runner_handle, and post the result via `post` (which takes
 * ownership of the response). Returns 1 if an op was served, 0 if none was
 * pending, -1 on a post failure. The client serve loop wraps this with the /v1
 * poll/respond transport; tests inject in-memory fetch/post. */
int ws_runner_serve_once(struct cJSON *(*fetch)(void *ctx),
                         int (*post)(void *ctx, struct cJSON *response), void *ctx);

#endif /* WORKSPACE_PROVIDER_DETACHED_H */
