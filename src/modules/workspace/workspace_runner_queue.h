#ifndef WORKSPACE_RUNNER_QUEUE_H
#define WORKSPACE_RUNNER_QUEUE_H 1

#include <pthread.h>

/* workspace_runner_queue — the blocking request/response handoff at the heart
 * of the detached transport (workspace-resource-plane §2–3).
 *
 * The detached provider (on the server) calls `ws_runner_queue_transport`,
 * which enqueues one op request and blocks for the response. The filesystem
 * authority — the client-side runner serving over /v1, or a worker thread —
 * drains it with `ws_runner_queue_poll`, executes the op
 * (ws_detached_runner_handle), and hands the result back with
 * `ws_runner_queue_respond`. This module is the in-process concurrency core;
 * the /v1 endpoints that let a remote client poll/respond sit on top of it.
 *
 * v1 is single-in-flight per queue ("one active writer per workspace handle",
 * per the proposal): a second transport caller waits until the current op's
 * cycle completes.
 *
 * Responses are a FIFO: a normal op posts exactly one (final) response, while a
 * STREAMING op (exec_stream — running a CLI agent on the client) posts a series
 * of {"partial":true,...} chunk responses followed by one final response. The
 * streaming transport drains chunks as they arrive without releasing the
 * single-in-flight slot until the final response lands. */

struct cJSON;

/* A tree this process can hand work to. It carries the id the module keys the
 * handoff by and nothing else: the pending request, the response FIFO, the
 * single-in-flight discipline and the condition variables that used to live
 * here are the handoff itself, and the handoff is in the workspace module. */
typedef struct
{
   char id[129];
} ws_runner_queue_t;

/* Streaming partial callback: invoked (NOT under the queue lock) with each
 * {"partial":true,...} response cJSON as it arrives (borrowed — do not free).
 * Return 0 to continue draining, non-zero to abort the stream early. */
typedef int (*ws_runner_partial_fn)(void *cb_ctx, struct cJSON *partial);

void ws_runner_queue_init(ws_runner_queue_t *q);
void ws_runner_queue_destroy(ws_runner_queue_t *q);

/* Wake all waiters and fail any pending/future op. Idempotent. */
void ws_runner_queue_close(ws_runner_queue_t *q);

/* Detached transport (ctx = ws_runner_queue_t*): enqueue `request`, block until
 * the runner responds (or the queue closes). Consumes `request` per the
 * ws_detached_transport_fn contract. Returns 0 and sets *response (caller owns)
 * on success; -1 (with *response NULL) if the queue is/becomes closed. */
int ws_runner_queue_transport(void *ctx, struct cJSON *request, struct cJSON **response);

/* Streaming detached transport (ctx = ws_runner_queue_t*): enqueue `request`,
 * then drain the response FIFO, invoking `on_partial` for each
 * {"partial":true,...} response until a final (non-partial) response lands. The
 * single-in-flight slot stays held for the whole stream. Consumes `request`.
 * Returns 0 on a clean final response (set in *final, caller owns and frees),
 * -1 on close/error (*final NULL). */
int ws_runner_queue_transport_stream(void *ctx, struct cJSON *request,
                                     ws_runner_partial_fn on_partial, void *cb_ctx,
                                     struct cJSON **final);

/* Runner side: take the next pending request, blocking up to timeout_ms
 * (<= 0 = no wait). Returns the request (caller owns and must cJSON_Delete it
 * after responding) or NULL on timeout / close. */
struct cJSON *ws_runner_queue_poll(ws_runner_queue_t *q, int timeout_ms);

/* Runner side: hand `response` back to the waiting transport (queue takes
 * ownership). Returns 0, or -1 if the queue is closed (response freed). */
int ws_runner_queue_respond(ws_runner_queue_t *q, struct cJSON *response);

#endif /* WORKSPACE_RUNNER_QUEUE_H */
