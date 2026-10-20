/* Server-owned pending harness controls. Browser replies enqueue decisions;
 * only the owning execution may consume them and write a native control frame. */
#ifndef AIMEE_SERVER_SESSION_CONTROL_H
#define AIMEE_SERVER_SESSION_CONTROL_H
#include "cJSON.h"
#include <aimee/ir/session_event.h>
typedef struct server_control_scope server_control_scope_t;
server_control_scope_t *server_control_scope_open(const char *session, const char *turn,
                                                  const char *principal);
/* A private canonical child has its own reply destination, while host callbacks
 * retain the parent's attested execution session. This immutable alias grants
 * only a lease on the already-bound thread scope; HTTP reply lookup never uses it. */
server_control_scope_t *server_control_scope_open_execution(const char *session, const char *turn,
                                                            const char *principal,
                                                            const char *execution_session);
/* Capture a lease from this thread under its attested session and principal. A missing
 * scope is a successful NULL capture. A closed/wrong-owner scope refuses.
 * Bind only while the owning execution or a captured lease remains alive. */
int server_control_scope_capture(const char *session, const char *principal,
                                 server_control_scope_t **out);
void server_control_scope_release(server_control_scope_t *scope);
void server_control_scope_close(server_control_scope_t *scope);
server_control_scope_t *server_control_scope_bind(server_control_scope_t *scope);
server_control_scope_t *server_control_scope_current(void);
int server_control_register(server_control_scope_t *scope, const char *native_id, const char *kind,
                            const cJSON *request);
/* Reserve a host-consumed control before emitting it through canonical capture.
 * Exactly one matching register consumes the reservation. Until then no reply
 * is accepted; native harness drainers never take a host-consumed reply. */
int server_control_reserve(server_control_scope_t *scope, const char *native_id, const char *kind,
                           const cJSON *request);
void server_control_scope_fail(server_control_scope_t *scope);
/* Durable host controls record before publication/consumption. The callback
 * receives immutable copied identity and a turn-qualified canonical event.
 * It must not call control APIs; it runs under this scope's lock, never the
 * global registry lock. No borrowed HTTP/sink context crosses the boundary. */
typedef int (*server_control_record_fn)(const char *session, const char *turn,
                                        const char *principal, const aimee_session_event_t *event);
int server_control_register_recorded(server_control_scope_t *scope, const char *native_id,
                                     const char *kind, const cJSON *request,
                                     server_control_record_fn record);
int server_control_abandon(server_control_scope_t *scope, const char *native_id);
/* Denial/cancellation of an unknown-effect retry blocks further mutations in
 * this host turn, without reopening the same prompt or changing old outcomes. */
int server_control_unknown_effect_denied(const server_control_scope_t *scope);
void server_control_deny_unknown_effects(server_control_scope_t *scope);
int server_control_scope_failed(const server_control_scope_t *scope);
/* 0 accepted; 1 stale/absent; -1 invalid or wrong owner. */
int server_control_reply(const char *session, const char *qualified_id, const char *principal,
                         const cJSON *reply);
/* 1 with owned outputs, 0 when none ready. NULL selects the next native reply;
 * reserved/recorded host controls require their specific ID and cannot be drained by a
 * native harness control loop. */
int server_control_take(server_control_scope_t *scope, const char *native_id, char **out_id,
                        char **out_kind, cJSON **out_request, cJSON **out_reply);
#endif
