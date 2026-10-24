/* Retained durable event owner. No connection, subscriber or credential state. */
#ifndef AIMEE_SERVER_SESSION_CAPTURE_H
#define AIMEE_SERVER_SESSION_CAPTURE_H
#include <aimee/ir/session_event.h>
typedef struct server_session_capture server_session_capture_t;
typedef struct
{
   unsigned long long counter, model_events, model_completions;
} server_session_capture_counts_t;
/* The host owns the registered canonical turn. execution_session is its verified
 * source session; a private child has a distinct destination, never a new actor.
 * One live writer owns each (session, turn); duplicate opens refuse until all
 * retained leases release. Consumers retain the existing owner. */
server_session_capture_t *server_session_capture_open(const char *session, const char *turn,
                                                      const char *principal,
                                                      const char *execution_session);
server_session_capture_t *server_session_capture_bind(server_session_capture_t *owner);
server_session_capture_t *server_session_capture_current(void);
/* Process-local ownership hint for authenticated native browse recovery only.
 * Exact identity remains registered through retained callback cleanup, including
 * a closed writer. This grants no history, control or execution authority. */
int server_session_capture_has_owner(const char *session, const char *actor, const char *turn);
/* Immutable destination history identity, valid for the held owner lease.
 * It may differ from the verified transport/source session. NULL without owner. */
const char *server_session_capture_session(const server_session_capture_t *owner);
const char *server_session_capture_turn(const server_session_capture_t *owner);
/* Transfer one already-quiescent derived resource to this host turn. The action
 * runs once, outside the writer lock, after a durable terminal. Success is true
 * only for a completed, healthy capture. Close without that proof calls it false.
 * Registration failure leaves ownership with the caller. This is no child-exit
 * proof: the registering owner must already have joined its child/callbacks. */
typedef void (*server_session_terminal_action_fn)(int success, void *resource);
int server_session_capture_after_terminal(server_session_capture_t *owner,
                                          server_session_terminal_action_fn action, void *resource);
/* Attach a bounded additional quiescent observer without replacing the primary
 * resource action (for example, native replay/cache custody). The same join,
 * transfer and failure rules apply. A resource cannot be transferred twice. */
int server_session_capture_observe_terminal(server_session_capture_t *owner,
                                            server_session_terminal_action_fn action,
                                            void *resource);
/* A quiescent verifier may prepare one typed receipt immediately before a
 * completed terminal is serialized. Preparation runs under the writer lock:
 * never re-enter capture APIs. NULL omits an unavailable receipt. The host
 * binds it to this actor/turn and atomically stores it with that terminal;
 * action still consumes the resource exactly once on every exit path. */
typedef const struct cJSON *(*server_session_verification_prepare_fn)(void *resource);
int server_session_capture_observe_verification(server_session_capture_t *owner,
                                                server_session_verification_prepare_fn prepare,
                                                server_session_terminal_action_fn action,
                                                void *resource);
/* A missing current owner is a successful NULL lease. Bind only inside a creator
 * or retained lease; release after unbinding and joining its callbacks. */
int server_session_capture_retain_current(const char *execution_session, const char *principal,
                                          server_session_capture_t **out);
void server_session_capture_release(server_session_capture_t *owner);
/* Normal browser operator cancellation is committed by the durable owner.
 * 0 acknowledges that exact cancelled terminal, 1 refuses, 2 classifies an
 * exact active native owner (no writes), -1 is unavailable. Native2 never seals
 * this capture or claims process quiescence. No model/client flag enters here. */
typedef int (*server_session_operator_cancel_fn)(const char *actor, const char *session,
                                                 const char *turn, char sequence[32]);
int server_session_capture_cancel_owned(const char *actor, const char *session, const char *turn,
                                        char sequence[32],
                                        server_session_operator_cancel_fn commit);
int server_session_capture_operator_cancelled(const server_session_capture_t *owner);
void server_session_capture_close(server_session_capture_t *owner);
void server_session_capture_fail(server_session_capture_t *owner);
int server_session_capture_failed(const server_session_capture_t *owner);
int server_session_capture_accepting(const server_session_capture_t *owner);
int server_session_capture_counts(server_session_capture_t *owner,
                                  server_session_capture_counts_t *out);
/* Last committed global session sequence, copied under the writer lock. "0"
 * means no committed event yet. Readable after a terminal/failure so a producer
 * can drain its durable tail; this grants no write or tool authority. */
int server_session_capture_cursor(server_session_capture_t *owner, char sequence[32]);
/* One model generation per retained host turn, including across native SDK
 * runtimes. A successful enter retains the owner until the matching leave;
 * stale/double leaves never release a later generation. Neither grants tools. */
int server_session_capture_model_enter(server_session_capture_t *owner, unsigned long long *lease);
int server_session_capture_model_leave(server_session_capture_t *owner, unsigned long long lease);
/* Once-only transient admission for an independently host-verified original
 * call at a committed source sequence/block. 0 claims, 1 already claimed, -1
 * refuses. This is not source verification or effect authorization; durable
 * permits/outcomes remain with the existing governed-action owner. */
int server_session_capture_claim_call(server_session_capture_t *owner, const char *source_sequence,
                                      int block_index);
/* Owned, admitted canonical conversation including this running turn. Never
 * imports native files; source absence/corruption makes capture failure sticky. */
int server_session_capture_history(server_session_capture_t *owner, struct cJSON **out);
/* Optional owned transport envelope and sequence. Capture failure is sticky;
 * only a failed/cancelled terminal may still be recorded after a failure.
 * A committed terminal seals further event writes without invalidating leases. */
int server_session_capture_append(server_session_capture_t *owner,
                                  const aimee_session_event_t *event, char **envelope,
                                  char sequence[32]);
int server_session_capture_sink(const aimee_session_event_t *event, void *owner);
#endif
