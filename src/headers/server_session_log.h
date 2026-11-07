/* A log-only execution owner. Contains copied attestation, never a connection. */
#ifndef AIMEE_SERVER_SESSION_LOG_H
#define AIMEE_SERVER_SESSION_LOG_H
#include "cJSON.h"
#include <aimee/ir/aimee_ir.h>
#include <stddef.h>
typedef struct server_session_log server_session_log_t;
/* 0 installed, 1 no session or an existing primary owner, -1 refused.
 * Initial content is canonical IR; NULL starts without inventing a message.
 * Actual selected registration, supplied only by the admitted host runtime.
 * Missing project/job identity keeps ordinary execution observational/unbound.
 * control_replies is an explicit producer capability (0/1); a disabled parent
 * cannot be re-enabled by a child. Existing primary capture is never modified. */
int server_session_log_begin_selected(const char *role, const aimee_message_t *message,
                                      const char *agent, const char *provider, const char *model,
                                      int control_replies, server_session_log_t **out);
/* Restores the calling task's sink; capture failure remains sticky. */
int server_session_log_finish(server_session_log_t *scope, int success, const char *error);
int server_session_log_active(void);
int server_session_log_history_save(const cJSON *messages);
/* Copies the current private execution identity; 1 means no log-only owner. */
int server_session_log_identity(char *session, size_t capacity, char *principal,
                                size_t principal_capacity);
#endif
