#ifndef AIMEE_SERVER_NATIVE_REPLAY_H
#define AIMEE_SERVER_NATIVE_REPLAY_H
#include <aimee/ir/session_event.h>

typedef struct server_native_replay server_native_replay_t;
/* Derived cache of host-observed proofs in the existing owner-checked session
 * log. The scope is an opaque digest of attested connection/account lineage. */
server_native_replay_t *server_native_replay_open(const char *session, const char *actor,
                                                  const char *scope, const char *model);
void server_native_replay_close(server_native_replay_t *replay);
int server_native_replay_restore(aimee_request_t *request, void *context);
int server_native_replay_capture(const aimee_session_event_t *event, void *context);
#endif
