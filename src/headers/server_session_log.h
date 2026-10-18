/* A log-only execution owner. Contains copied attestation, never a connection. */
#ifndef AIMEE_SERVER_SESSION_LOG_H
#define AIMEE_SERVER_SESSION_LOG_H
#include "cJSON.h"
#include <stddef.h>
typedef struct server_session_log server_session_log_t;
/* 0 installed, 1 no session or an existing primary owner, -1 refused. */
int server_session_log_begin(const char *role, const char *prompt, server_session_log_t **out);
/* Restores the calling task's sink; capture failure remains sticky. */
int server_session_log_finish(server_session_log_t *scope, int success, const char *error);
int server_session_log_active(void);
int server_session_log_history_load(cJSON **out);
int server_session_log_history_save(const cJSON *messages);
/* Copies the current private execution identity; 1 means no log-only owner. */
int server_session_log_identity(char *session, size_t capacity, char *principal,
                                size_t principal_capacity);
#endif
