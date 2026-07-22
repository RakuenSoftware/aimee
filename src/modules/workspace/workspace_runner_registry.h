#ifndef WORKSPACE_RUNNER_REGISTRY_H
#define WORKSPACE_RUNNER_REGISTRY_H 1

#include "workspace_runner_queue.h"

/* workspace_runner_registry — a process-global, concurrency-safe map from a
 * workspace handle id to its runner queue (workspace-resource-plane §2–3).
 *
 * A detached workspace has exactly one runner queue: the detached provider's
 * transport enqueues ops onto it, and the /v1 reverse-channel endpoints
 * (runner poll / respond) drain it on behalf of the remote client that owns
 * the filesystem. Both sides resolve the same queue by workspace id here, so
 * the registry is the rendezvous between "the server wants a file op done" and
 * "the client is here to do it". Bounded (64 live detached workspaces). */

#define WS_RUNNER_REGISTRY_MAX   64
#define WS_RUNNER_REGISTRY_IDLEN 128

/* Return the queue for `id`, creating (and initializing) it on first use.
 * NULL only when `id` is invalid or the registry is full. */
ws_runner_queue_t *ws_runner_registry_get_or_create(const char *id);

/* Return the queue for `id`, or NULL if none is registered. */
ws_runner_queue_t *ws_runner_registry_lookup(const char *id);

/* Close + destroy + remove the queue for `id` (workspace/session teardown).
 * Closing wakes any blocked transport/poll and fails their ops. No-op if
 * `id` is absent. */
void ws_runner_registry_remove(const char *id);

/* Reverse-channel endpoint helpers — what the `runner.poll` / `runner.respond`
 * RPC handlers (and any future /v1 route) wrap. The filesystem-authority client
 * serving workspace `id` calls poll to fetch the next op the server needs done,
 * executes it locally (ws_detached_runner_handle), then posts the result. */

struct cJSON;

/* Get-or-create the queue for `id` and block up to timeout_ms for the next op
 * request. Returns the request (caller cJSON_Delete's) or NULL on timeout /
 * close — the client re-polls. */
struct cJSON *ws_runner_registry_poll(const char *id, int timeout_ms);

/* Hand `response` back to the transport blocked on `id`'s queue (queue takes
 * ownership). Returns 0, or -1 if no queue is registered for `id` (response
 * freed). */
int ws_runner_registry_respond(const char *id, struct cJSON *response);

#endif /* WORKSPACE_RUNNER_REGISTRY_H */
