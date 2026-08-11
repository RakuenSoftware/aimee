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

#include <stdint.h>

/* One module call over the bus. Exposed so the handoff marshalling next door
 * shares this one path rather than growing a second. */
int ws_runner_bus_call(uint32_t event_kind, uint32_t stage_id, const void *request,
                       uint32_t request_len, void *response, uint32_t response_capacity,
                       uint32_t *response_len, uint64_t timeout_ms);

/* ws_runner_io: nobody is serving the tree. Distinct from -1 (any other
 * failure) because it is PERMANENT and returned without waiting, so a caller
 * that retries on it does so in a tight loop rather than at the poll interval. */
#define WS_RUNNER_IO_UNSERVED (-2)

/* One handoff op for tree `id`. On success and when `out` is non-NULL, *out is
 * the returned chunk (malloc'd, NUL-terminated; caller frees) and *more says
 * another chunk follows. Returns 0, WS_RUNNER_IO_UNSERVED, or -1. */
int ws_runner_io(unsigned op, const char *id, const char *payload, size_t payload_len, char **out,
                 size_t *out_len, int *more, uint64_t timeout_ms);

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
 * close — the client re-polls.
 *
 * `unserved` (optional) SEPARATES THE TWO REASONS FOR NULL, which callers must
 * not conflate. A timeout means "nothing pending yet", took the full wait, and
 * re-polling immediately is correct. NOBODY SERVING THIS TREE is permanent and
 * is refused INSTANTLY — so a caller that re-polls on it spins as fast as the
 * transport allows. That is not hypothetical: it ran at ~440 polls/second on a
 * live appliance and wrote 664,408 identical log lines in 25 minutes. Set to 1
 * only in that case.
 *
 * In that case this call also SLEEPS before returning (min(timeout_ms,
 * WS_RUNNER_UNSERVED_PACE_MS)), so the pacing every long-poll caller relies on
 * holds whether or not a runner exists. Doing it here rather than at each
 * endpoint means a new caller cannot reintroduce the spin by forgetting. */
#define WS_RUNNER_UNSERVED_PACE_MS 2000u
struct cJSON *ws_runner_registry_poll(const char *id, int timeout_ms, int *unserved);

/* Hand `response` back to the transport blocked on `id`'s queue (queue takes
 * ownership). Returns 0, or -1 if no queue is registered for `id` (response
 * freed). */
int ws_runner_registry_respond(const char *id, struct cJSON *response);

#endif /* WORKSPACE_RUNNER_REGISTRY_H */
