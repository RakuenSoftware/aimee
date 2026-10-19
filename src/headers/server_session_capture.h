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
 * source session; a private child has a distinct destination, never a new actor. */
server_session_capture_t *server_session_capture_open(const char *session, const char *turn,
                                                      const char *principal,
                                                      const char *execution_session);
server_session_capture_t *server_session_capture_bind(server_session_capture_t *owner);
server_session_capture_t *server_session_capture_current(void);
/* Immutable destination history identity, valid for the held owner lease.
 * It may differ from the verified transport/source session. NULL without owner. */
const char *server_session_capture_session(const server_session_capture_t *owner);
/* A missing current owner is a successful NULL lease. Bind only inside a creator
 * or retained lease; release after unbinding and joining its callbacks. */
int server_session_capture_retain_current(const char *execution_session, const char *principal,
                                          server_session_capture_t **out);
void server_session_capture_release(server_session_capture_t *owner);
void server_session_capture_close(server_session_capture_t *owner);
void server_session_capture_fail(server_session_capture_t *owner);
int server_session_capture_failed(const server_session_capture_t *owner);
int server_session_capture_accepting(const server_session_capture_t *owner);
int server_session_capture_counts(server_session_capture_t *owner,
                                  server_session_capture_counts_t *out);
/* Optional owned transport envelope and sequence. Capture failure is sticky;
 * only a failed/cancelled terminal may still be recorded after a failure.
 * A committed terminal seals further event writes without invalidating leases. */
int server_session_capture_append(server_session_capture_t *owner,
                                  const aimee_session_event_t *event, char **envelope,
                                  char sequence[32]);
int server_session_capture_sink(const aimee_session_event_t *event, void *owner);
#endif
