#ifndef AIMEE_SERVER_NATIVE_HISTORY_H
#define AIMEE_SERVER_NATIVE_HISTORY_H
#include "cJSON.h"
/* Host-only loader: both outputs are owned. Only its authenticated durable
 * sidecar may admit a reserved interruption diagnostic. include_running keeps
 * the existing active-turn history distinction; neither mode authorizes effects. */
int server_native_history_load(const char *session, const char *actor, const char *active_turn,
                               int include_running, cJSON **messages, cJSON **receipts);
/* Read-only post-policy validation using the retained host-loaded receipt set.
 * Never pass model/provider/caller JSON as receipts. No retry is authorized. */
int server_native_history_admit(const cJSON *messages, const cJSON *receipts);
/* Existing canonical loaders can retain Missing/import semantics. Ordinary
 * histories use strict generic admission; reserved markers require fresh45. */
int server_native_history_admit_source(const char *session, const char *actor,
                                       const char *active_turn, int include_running,
                                       const cJSON *messages);
int server_native_history_admit_current(const cJSON *messages);
int server_native_history_requires_permission(const cJSON *receipts);
#endif
